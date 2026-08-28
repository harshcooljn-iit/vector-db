# Distance metrics

Mathematical definitions, the ordering convention, and the numerical policy.

Implementation: `src/distance/`. Learning note:
[`learnings/30-vector-database/03-distance-metrics.md`](../learnings/30-vector-database/03-distance-metrics.md).

## Notation

`a` and `b` are vectors of dimension `D` with components `a_i`, `b_i`.
`|a|` is the Euclidean norm `sqrt(sum_i a_i^2)`.

---

## The ordering convention

> **Internally, every metric produces a `RankKey` where smaller is better.**

This is the single most load-bearing convention in the codebase. Because it
holds unconditionally, the top-k heap, the HNSW candidate and result queues, the
merge step and every comparison in the engine are written once, with no
per-metric branching and no ascending/descending flag to get wrong.

Users are shown a different number. `1 - cos` is the right thing to sort by and
the wrong thing to print; nobody reads "0.09" as a strong match. The conversion
happens exactly once per returned result, at the API boundary — never per
candidate.

| Metric | `RankKey` (internal, smaller better) | `score` (reported) | Score range | Better |
|---|---|---|---|---|
| `l2` | `sum_i (a_i - b_i)^2` | same | `[0, inf)` | smaller |
| `cosine` | `1 - cos(a, b)` | `cos(a, b)` | `[-1, 1]` | larger |
| `dot` | `-(a . b)` | `a . b` | `(-inf, inf)` | larger |

`score_from_rank_key` / `rank_key_from_score` in
`include/vectordb/distance/distance.hpp` are the only place the mapping exists.

---

## 1. Squared Euclidean distance — `l2`

```
d2(a, b) = sum_i (a_i - b_i)^2
```

**The square root is deliberately not taken.** `sqrt` is strictly increasing on
`[0, inf)`, so it cannot change the relative order of any two distances — the
top-k under `d2` is exactly the top-k under `d`. Taking it would add a `sqrt`
per candidate to compute a number nothing needs.

If a caller genuinely wants the true Euclidean distance (for a threshold in
input units, say), they take the root of the reported score. That is one `sqrt`
per *result*, not per candidate.

**Properties.** Symmetric; zero iff `a == b`; sensitive to magnitude — `[1,0]`
and `[100,0]` point the same way but are far apart under `l2`.

**Use it when** magnitude is meaningful: raw feature vectors, coordinates,
image descriptors that are not length-normalised.

---

## 2. Cosine — `cosine`

```
cos(a, b) = (a . b) / (|a| |b|)
distance  = 1 - cos(a, b)          in [0, 2]
```

Measures the angle between vectors and ignores their lengths entirely.
`[1,0]` and `[100,0]` are identical under cosine.

**Use it when** direction carries the meaning and magnitude is an artefact.
This is the default for text embeddings, where the length of an embedding tends
to correlate with document length rather than with content.

### The cost, and how it is avoided

The naive formula needs three passes over the data — one dot product and two
norms — where `l2` needs one. On the hottest loop in the system that is 3× the
memory traffic.

Two mitigations, both in the code:

1. **Cache the stored vector's norm.** `DistanceFunction::with_norms` takes
   precomputed norms, so a search computes the query's norm once and reads each
   candidate's from an array. Back to one pass.
2. **Normalize on insert.** If every stored vector has `|v| = 1` and the query
   is normalized too, then `cos(a, b) = a . b` and cosine costs exactly what an
   inner product costs. This is why the database offers optional normalization
   — see [ADR-0005](decisions/ADR-0005-float32.md) and
   `include/vectordb/core/vector.hpp`.

### The zero-vector rule

`cos(0, b)` is `0/0` — undefined. Returning NaN would be catastrophic: NaN
compares false against everything, which breaks the total order that every
ranking structure in the engine assumes, and makes `std::sort` undefined
behaviour rather than merely wrong (see
[ADR-0007](decisions/ADR-0007-nan-policy.md)).

**VectorDB defines the cosine similarity involving a zero vector as 0**
(rank key `1.0`): unrelated to everything. A zero vector therefore sorts behind
anything with real overlap and ahead of anything actively opposed. This is a
definition, not a computation, and it is tested by name.

### Clamping

Floating-point error can produce `cos = 1.0000001` for a vector compared with
itself. The result is clamped to `[-1, 1]`. This does not change any ranking; it
keeps the reported score inside its documented range so that nothing displays a
similarity above 1.

---

## 3. Inner product — `dot`

```
a . b = sum_i a_i b_i
rank key = -(a . b)
```

**Not a distance.** It fails the axioms: `a . a = |a|^2` is not zero, and a
longer vector scores higher against everything, so there is no "self is
nearest" guarantee.

That is sometimes exactly what you want — recommendation models often encode
item popularity or confidence in the vector's magnitude, and maximum
inner-product search (MIPS) is the correct query.

It is negated internally purely to satisfy the smaller-is-better convention.

**Caution.** HNSW's graph construction assumes distances behave metrically
(triangle-inequality-ish). Inner product does not, so recall under `dot` can be
worse than under `l2` or `cosine` for the same parameters. This is a known
property of the algorithm, not a bug in this implementation, and it is
documented in [`hnsw.md`](hnsw.md).

---

## Relationships worth knowing

For **unit vectors** (`|a| = |b| = 1`) all three collapse into each other:

```
d2(a, b) = |a|^2 + |b|^2 - 2(a . b) = 2 - 2(a . b) = 2 * (1 - cos(a, b))
```

So over normalized vectors, `l2` ranking, `cosine` ranking and `dot` ranking are
**identical orderings**. They differ only in the number reported.

This is a genuinely useful fact: normalize on insert and you may pick whichever
metric produces the score you want to display, without changing which results
come back.

The expansion also shows why `l2` never needs a norm cache: expanding
`(a_i - b_i)^2` needs no normalisation at all, because the magnitudes are part
of the answer rather than something to divide out.

---

## Numerical policy

### Accumulation

The kernels use **four independent `float` accumulators** rather than one.

A single accumulator creates a serial dependency chain: each `+=` waits for the
previous one. A float add has ~3–4 cycles of latency but can be issued every
cycle, so the chain leaves most of the FPU idle. Four independent chains fill
it. They also each sum a quarter of the terms, so each stays smaller and rounds
less — the accuracy improves as a side effect of the speed fix.

Four matches the NEON lane count, so the scalar and SIMD kernels sum in the same
groupings and agree to a much tighter tolerance than they otherwise would.

Contrast `l2_norm_squared()` in `src/core/vector.cpp`, which uses a single
`double` accumulator: it runs once per vector rather than once per candidate, so
precision is free there. Same question, different local costs, different answer.

### `-ffast-math` is not enabled

It would let the compiler perform the multiple-accumulator transformation
itself. We do it explicitly instead, because the SIMD kernels are validated
against the scalar kernel — if the compiler is silently reassociating both, the
test compares two moving targets. `-ffast-math` also implies
`-ffinite-math-only`, which permits deleting the NaN checks that
[ADR-0007](decisions/ADR-0007-nan-policy.md) depends on.

### Test tolerances

Comparisons use **relative** tolerances, because absolute float error grows with
the number of terms: an epsilon that is right at `D = 4` is far too tight at
`D = 1536`.

Exact equality is used only where a value was *copied* rather than computed —
there, any difference is a real bug rather than drift.

---

## Kernels

Each metric is expressed in terms of three primitives, and only those primitives
are ever reimplemented for a new instruction set:

```cpp
struct DistanceKernel {
    BinaryReduceFn l2_squared;    // sum((a-b)^2)
    BinaryReduceFn dot;           // sum(a*b)
    UnaryReduceFn  norm_squared;  // sum(v^2)
    std::string_view name;        // "scalar", "neon", "avx2"
};
```

`scalar_kernel()` is always available and is the oracle: an optimised kernel
that disagrees with it is wrong, in exactly the way an HNSW result that
disagrees with brute force is wrong.

`active_kernel()` returns the fastest kernel that this CPU can actually execute,
chosen once via runtime feature detection. A kernel is never offered unless its
instructions are supported — compiling AVX2 and running it unconditionally is a
SIGILL on a user's older machine.

`vectordb benchmark-distance` times every available kernel against the others,
and the kernel name is recorded in every benchmark result. A timing without the
kernel that produced it is not a measurement.
