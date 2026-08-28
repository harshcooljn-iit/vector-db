# Representing a vector

🟡 Intermediate · pairs with `include/vectordb/core/vector.hpp`

## The type you would write first

```cpp
struct Vector {
    uint64_t id;
    std::vector<float> data;
};
```

Perfectly reasonable, and wrong for this system in three ways. Working out why
is most of the design.

### Problem 1: it owns its floats

Every `Vector` carries a heap allocation. A million vectors means a million
allocations scattered across the heap, and a scan that pointer-chases instead of
streaming. That is [03-memory-layout](../10-cpp-systems-foundations/03-memory-layout.md),
and it is the big one.

### Problem 2: the id is in the wrong place

Storing the id *next to* the floats means a scan reads it too. The distance loop
does not want the id — it wants floats, densely packed. Interleaving 8 bytes of
id every 3072 bytes of payload wastes a fraction of every cache line on data the
loop discards.

Worse, it makes the id and the vector share a lifetime and a layout. But an id
is metadata about a slot, and slots get renumbered by compaction.

**Separate the two.** The floats live in one contiguous block; the ids live in
their own arrays. This is the "struct of arrays" idea from graphics and ECS
work, and it applies for the same reason: iterate over what the loop reads.

### Problem 3: it forces a copy to pass one around

```cpp
float distance(Vector a, Vector b);        // two heap copies per call
float distance(const Vector& a, ...);      // better, but still ties you to
                                           // "the floats live in a Vector"
```

The distance kernel does not care where the floats came from. It should accept
*any* contiguous run of `D` floats — from the store, from a query the user just
parsed, from a scratch buffer, from a memory-mapped file.

## What we use instead

```cpp
using VectorView        = std::span<const float>;
using MutableVectorView = std::span<float>;
```

A `std::span` is a pointer and a length. Two words, trivially copyable, owning
nothing. Pass it **by value** — taking `const std::span&` is a pessimisation,
adding an indirection to something already pointer-sized.

```cpp
VectorView a = store[42];             // into the big contiguous block
VectorView b = query_buffer;          // into a stack array
VectorView c = mapped_file_region;    // into an mmap'd page
float d = distance(a, b);             // the kernel cannot tell them apart
```

That decoupling is what will let memory mapping arrive later without touching
any kernel.

### The catch: a span does not keep anything alive

```cpp
VectorView v = array[5];
array.push_back(x);       // may reallocate the whole block
use(v[0]);                // ☠️ dangling
```

Same rule as `std::vector`'s iterators. Never hold a view across an insertion.
The `asan` preset catches it immediately; `release` does not catch it at all.

## Where the ids go

```
VectorArray            ids                    reverse map
[ v0 ][ v1 ][ v2 ]     [ 1001, 7, 4242 ]      { 1001→0, 7→1, 4242→2 }
   0     1     2        LocalId → VectorId     VectorId → LocalId
```

Three separate structures, each dense and each read by different code:

- the **float block** is read by the distance kernels, once per candidate;
- `LocalId → VectorId` is read once per *result*, at the very end (k times, not
  N times);
- `VectorId → LocalId` is read once per user-facing lookup.

Putting the id inline would move a k-times-per-query cost into the
N-times-per-query loop. See [ADR-0003](../../docs/decisions/ADR-0003-id-model.md).

## Validation happens at the boundary, once

`validate_vector()` enforces two things:

1. **Dimension matches exactly.** Never truncated, never padded
   ([ADR-0002](../../docs/decisions/ADR-0002-fixed-dimension.md)).
2. **Every component is finite.** No NaN, no ±Inf
   ([ADR-0007](../../docs/decisions/ADR-0007-nan-policy.md)).

The NaN rule deserves more than a mention, because it is the kind of thing that
looks like fussiness and is actually load-bearing.

`NaN` compares **false** against everything, including itself. So:

- `std::sort` with a comparator that is not a strict weak ordering is
  **undefined behaviour** — not "gives a weird order", but "may read out of
  bounds". One NaN distance can corrupt memory.
- A bounded top-k heap's "is this better than my current worst?" test is false
  for a NaN candidate *and* false in the other direction, so the heap's
  invariant silently breaks.
- HNSW's greedy descent is "move to a closer neighbour". With NaN, nothing is
  ever closer, so the walk stops at the entry point and returns garbage — with
  no error, at normal speed.

Notice the shape of every failure: **nothing throws, results keep coming, and
they are wrong.** Rejecting at insertion is the only cheap place to check — once
per vector rather than once per comparison — and it lets every algorithm above
assume a total order.

Note where the check is *not*: `VectorArray::push_back` does not validate.
Internal copies, index rebuilds and file loads move already-validated data;
re-checking would put an `O(N·D)` pass on every rebuild for nothing.
Validation belongs at the database boundary, where untrusted data enters.

## Normalization: an explicit choice, never a silent one

For cosine similarity:

```
cos(a, b) = (a · b) / (|a| · |b|)
```

If every stored vector is pre-normalised to unit length, `|a| = |b| = 1` and
this collapses to a plain dot product — a real saving, since you skip two norm
computations per candidate.

So why not always normalise?

- It **destroys the magnitude**. If your embeddings encode confidence or term
  frequency in their length, normalising throws it away, and an inner-product
  search over normalised vectors is no longer the search you asked for.
- It is **not reversible**. The original vector is gone unless you stored the
  norm.

So normalization is opt-in per database, recorded in the config, and the API
keeps "raw vector" and "normalized vector" distinct in its vocabulary.

One edge case matters: the **zero vector** has no direction. `x / 0` gives NaN,
which is exactly what validation exists to keep out — so `normalize_in_place`
leaves a zero vector alone and returns `0.0F` rather than manufacturing a NaN.
There is a test named for precisely this.

## Accumulator precision — a small decision made twice, differently

```cpp
// l2_norm_squared: double accumulator
double sum = 0.0;
for (float v : vector) sum += double(v) * double(v);
```

`float` has ~7 significant decimal digits. Summing 4096 values loses low bits
steadily: once `sum` is large, adding a small term rounds it away entirely.
A `double` accumulator makes that vanish.

But `l2_norm_squared` runs **once per vector**, at insert or normalize time.
The distance kernels run **once per candidate** — billions of times — and there
`double` would halve SIMD lane count. So they make the opposite choice, and
recover the accuracy a different way (several independent `float` accumulators,
which shortens each dependency chain and reduces drift *and* runs faster).

Two call sites, same question, different answers, both documented where they
are made. That is what "engineering judgement" looks like in practice: not a
rule you memorise, but a trade-off you re-evaluate with the local cost in hand.

## Experiments

1. Sum 1,000,000 copies of `0.1F` with a `float` accumulator, then a `double`
   one, then with four interleaved `float` accumulators. Compare all three to
   `100000.0`. The four-accumulator result is the interesting one — explain why
   it beats the single `float`.
2. Take `normalize_in_place`, remove the zero-norm guard, and normalise a zero
   vector. Then feed the result to `validate_vector`. Trace how far a NaN would
   have travelled before anything noticed.
3. Change `VectorView` from `std::span<const float>` to
   `const std::vector<float>&` throughout and try to write a kernel that works
   on both an owned vector and a memory-mapped region. Note what stops you.

## Where this lives in the code

- `include/vectordb/core/vector.hpp` — the views and the validation contract
- `src/core/vector.cpp` — `validate_vector`, `l2_norm*`, `normalize_*`
- `include/vectordb/core/vector_array.hpp` — the contiguous block
- `tests/unit/core_vector_test.cpp` — every edge case above has a named test
- ADRs [0002](../../docs/decisions/ADR-0002-fixed-dimension.md),
  [0003](../../docs/decisions/ADR-0003-id-model.md),
  [0004](../../docs/decisions/ADR-0004-contiguous-storage.md),
  [0005](../../docs/decisions/ADR-0005-float32.md),
  [0007](../../docs/decisions/ADR-0007-nan-policy.md)

---

Next: [03-distance-metrics.md](03-distance-metrics.md)
