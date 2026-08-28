# Distance metrics — three ways to be "close"

🟡 Intermediate · pairs with `src/distance/`

## The question the whole database is built around

Given a query vector and a stored vector, produce one number saying how well
they match. Everything else — the index, the storage layout, the SIMD work — is
machinery for calling that function fewer times or faster.

You already know the maths. What is new is the engineering around it: an
ordering convention, a numerical policy, and a hot-loop budget.

## The three metrics

### Squared Euclidean

```
d²(a, b) = Σ (aᵢ - bᵢ)²
```

Straight-line distance, without the square root.

**Why no square root?** `sqrt` is strictly increasing, so it cannot change which
vector is closer. The top-k under `d²` is exactly the top-k under `d`. Taking
the root would add one `sqrt` per candidate — millions per query — to compute a
number nothing uses.

This is a small idea with a general shape worth internalising: **if a monotone
transform does not change your decision, do not apply it in the loop.** Apply it
once at the end if a human needs to read it.

### Cosine

```
cos(a, b) = (a · b) / (|a| |b|)
```

The angle between the vectors. Magnitude is discarded entirely: `[1,0]` and
`[100,0]` are identical.

That is usually what you want for text. The length of a text embedding tends to
track document length rather than meaning, so ignoring it is a feature.

### Inner product

```
a · b = Σ aᵢ bᵢ
```

**Not a distance.** `a · a = |a|²`, not 0, so a vector is not its own nearest
neighbour, and a longer vector beats a shorter one against every query.

Sometimes that is the point: recommendation models often encode popularity or
confidence in the magnitude, and you *want* the popular item to win ties.

## The convention that makes everything else simple

Three metrics, and they disagree about direction: smaller `l2` is better, larger
cosine is better, larger dot is better.

Without a convention, every comparison in the engine needs to know which metric
it is working with:

```cpp
if (metric == Metric::kCosine || metric == Metric::kInnerProduct) {
    if (candidate > worst) { ... }
} else {
    if (candidate < worst) { ... }
}
```

Now write that inside the top-k heap. And the HNSW candidate queue. And the
result queue. And the merge step. Each is a place to get the direction backwards
— and getting it backwards does not crash, it just returns the *worst* k
results, plausibly formatted.

So VectorDB fixes it once:

> **`RankKey`: smaller is always better, for every metric.**

| Metric | RankKey | How |
|---|---|---|
| `l2` | `Σ(aᵢ-bᵢ)²` | already ascending |
| `cosine` | `1 - cos` | flipped |
| `dot` | `-(a·b)` | negated |

Every comparison in the engine is now `<`. No flags, no branches, no direction
bugs.

But `-32.5` is a terrible thing to show a user. So there is exactly one
conversion point, at the API boundary:

```cpp
float score_from_rank_key(Metric, RankKey);
```

It runs **k times per query** (once per returned result), not N times. Putting
the human-facing representation in the inner loop would be paying millions of
conversions so that ten of them can be printed.

The general pattern: **pick the representation your algorithm wants, and
convert at the boundary.** Not the other way round.

## Cosine's hidden cost, and two ways out

Count the passes over the data:

| Metric | Passes |
|---|---|
| `l2` | 1 |
| `dot` | 1 |
| `cosine` | **3** — one dot product, two norms |

Three times the memory traffic on the hottest loop. Two fixes, both in the code:

**1. Cache the stored norms.** `|b|` does not change between queries, so compute
it once at insert and keep it in an array. `DistanceFunction::with_norms` takes
them. Back to one pass.

```cpp
// naive: three passes over 3 KB per candidate
cosine(query, candidate);
// cached: one pass, plus one float read
cosine.with_norms(query, query_norm, candidate, norms[i]);
```

**2. Normalize on insert.** If `|a| = |b| = 1` then `cos(a,b) = a · b`, and
cosine costs exactly what an inner product costs. This is the entire reason the
database offers optional normalization.

Notice the trade-off in fix 1: you spend `4·N` bytes of memory to save `2·D·N`
bytes of traffic per query. At `D = 768` that is a 1536:1 return. This is the
most common shape of a systems optimisation — **precompute something small that
is reused, to avoid recomputing something large.**

## The identity that ties them together

For unit vectors:

```
d²(a,b) = |a|² + |b|² - 2(a·b) = 2 - 2(a·b) = 2·(1 - cos(a,b))
```

So over normalized vectors, **all three metrics produce the same ranking.** They
differ only in the number printed.

Practical consequence: normalize on insert and you can pick whichever metric
gives the score you want to display, without changing which results come back.
Worth knowing before you spend an afternoon debugging why `l2` and `cosine`
"agree suspiciously".

## Numerical policy: why four accumulators

The obvious loop:

```cpp
float sum = 0.0F;
for (size_t i = 0; i < n; ++i) sum += a[i] * b[i];
```

Two problems, and they have the same fix.

**Accuracy.** Each `+=` rounds to the nearest representable float. Once `sum` is
large relative to the next term, small terms round away entirely. Over 1536
dimensions this is measurable.

**Speed.** Every `+=` depends on the previous one. A float add has ~3–4 cycles
of *latency* but the FPU can start a new one every cycle. A serial chain
therefore leaves roughly 75% of the pipeline idle — you are latency-bound on an
operation that should be throughput-bound.

```
serial:      add ──wait──> add ──wait──> add ──wait──> add
4 chains:    add ─┐  add ─┐  add ─┐  add ─┐
                  add ─┐  add ─┐  add ─┐  add     (pipeline full)
```

Four independent accumulators fix both: four chains in flight, and each sums a
quarter of the terms so each stays smaller and rounds less. **The accuracy
improvement is a side effect of the speed fix**, which is a pleasing and not
uncommon outcome.

Why four specifically? It matches the NEON lane count, so the scalar and SIMD
kernels sum in the same groupings and their results agree to a much tighter
tolerance than they otherwise would. That makes "SIMD must match scalar" a
strict test rather than a loose one.

### Why not just `-ffast-math`?

That flag lets the compiler do this transformation itself, by permitting it to
treat float addition as associative (it is not).

We do it by hand because:

1. The SIMD kernels are validated *against* the scalar kernel. If the compiler
   is silently reassociating both, the test compares two moving targets and
   cannot tell you whether your SIMD code is correct.
2. `-ffast-math` implies `-ffinite-math-only`: the compiler may assume NaN and
   Inf never occur, and is then free to delete the checks that
   [ADR-0007](../../docs/decisions/ADR-0007-nan-policy.md) depends on.

**Explicit beats implicit in code whose correctness is being asserted.**

### And the opposite choice, ten metres away

`l2_norm_squared()` in `src/core/vector.cpp` uses a single `double`
accumulator — the exact opposite decision.

It runs **once per vector** at insert time, not once per candidate. Precision is
free there, and `double` gives more of it than four float chains do. Different
local costs, different answer, both documented where they are made.

That is what engineering judgement actually looks like: not a rule you memorise,
but the same question re-answered with the local budget in hand.

## The kernel indirection

```cpp
struct DistanceKernel {
    BinaryReduceFn l2_squared;
    BinaryReduceFn dot;
    UnaryReduceFn  norm_squared;
    std::string_view name;
};
```

Three function pointers. Every metric is built from them, so adding NEON or
AVX2 means adding a table — not editing a single caller.

**"Isn't an indirect call in the hot loop expensive?"** Good instinct. Look at
what it wraps: at `D = 768`, `l2_squared` performs 768 multiply-adds and streams
6 KB. The indirect call is a few nanoseconds against hundreds. It is under 1%.

At `D = 4` the ratio inverts and the call dominates. That is a real limitation,
honestly stated: this system is designed for embedding-sized vectors, and the
CLI demo uses `--dimension 4` for readability, not for speed.

**"Why not virtual functions?"** Same cost, more machinery — a vtable pointer,
an object to keep alive, a class hierarchy. A table of function pointers is a
constexpr-friendly aggregate you can swap in a test with an assignment.

**"Why not templates?"** Zero call overhead, and it would work — but the kernel
must be chosen at *runtime* from CPU features detected at *runtime*. A template
picks at compile time. You would end up with a runtime switch selecting between
template instantiations, which is a function pointer table with more steps.

## Experiments

1. Sum `0.1F` a million times with one accumulator, then four, then a `double`.
   Compare all three to `100000.0`. Explain why four floats beat one float, and
   why they do not beat one double.
2. Time `cosine` naively versus `with_norms` over 100,000 vectors at `D = 768`
   in the `release` build. Predict the ratio from the pass count first (3:1),
   then measure. If the measurement is lower than 3×, work out what else is the
   bottleneck.
3. Normalize a dataset and confirm the claim that `l2`, `cosine` and `dot`
   return identical result *sets* — and different scores.
4. Delete the four-accumulator unrolling, leaving a single `float sum`. Rerun
   `StaysAccurateOverLongVectors`. Then measure the speed difference in
   `release`. You have just isolated both effects.
5. Remove the zero-norm guard in `cosine_rank_key`, feed a zero vector through,
   and follow the resulting NaN into a `std::sort`. Under `asan`, notice what
   kind of error it becomes.

## Where this lives in the code

- `include/vectordb/distance/kernel.hpp` — the three primitives and the registry
- `src/distance/kernel_scalar.cpp` — the reference implementation, with the
  accumulator reasoning inline
- `src/distance/kernel_registry.cpp` — runtime selection; never returns a kernel
  this CPU cannot execute
- `include/vectordb/distance/distance.hpp` — `RankKey`, the score conversions,
  `DistanceFunction`
- `src/distance/distance.cpp` — `cosine_rank_key`, including the zero-vector
  definition and the clamp
- `tests/unit/distance_kernel_test.cpp` — every kernel runs the same suite;
  tails from 1 to 17; a double-precision reference
- `tests/unit/distance_metric_test.cpp` — the ordering convention, score
  round-trips, the zero-vector case
- `docs/distance-metrics.md` — the formal definitions

---

Next: [04-top-k-search.md](04-top-k-search.md)
