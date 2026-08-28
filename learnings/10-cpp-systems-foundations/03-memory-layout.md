# Memory layout — why the same data is 10× faster in a different shape

🟡 Intermediate · read before `src/core/vector_array.cpp`

## The claim

These two programs store identical numbers and compute identical results:

```cpp
// A
std::vector<std::vector<float>> vectors;   // 1M vectors of 768 floats

// B
std::vector<float> vectors;                // 1M * 768 floats, back to back
```

B scans dramatically faster than A. Not 5% — a large factor, and the gap grows
with the dataset. Understanding *why* is the difference between writing code
that works and writing code that works at scale.

## What the CPU actually does

Your mental model is probably "the CPU reads memory". The real hierarchy on an
Apple M1 (and every modern CPU is similar in shape):

| Level | Size | Latency | Relative |
|---|---|---|---|
| Register | ~KB | 0 cycles | 1× |
| L1 data cache | 64–128 KB/core | ~4 cycles | ~4× |
| L2 cache | 4 MB (shared per cluster) | ~15 cycles | ~15× |
| DRAM | 8 GB | ~100–300 cycles | **~100×** |

Two facts follow, and they drive everything:

### Fact 1: memory moves in cache lines, not bytes

The smallest unit transferred is a **cache line** — 64 bytes on x86, 128 on
Apple Silicon. Read one `float` and the hardware fetches the whole line.

If the next float you need is in that line, it is free. If it is somewhere else,
you pay the full trip again. **Sequential access gets 16–32 floats per DRAM
trip; random access gets 1.**

### Fact 2: the prefetcher only helps predictable patterns

The CPU watches your access pattern. If you walk forward through memory, it
starts fetching lines *ahead of you*, so by the time you ask, the data is
already in L1. This is why a sequential scan of an array can run at near
memory-bandwidth speed instead of memory-latency speed.

The prefetcher cannot follow pointers. It does not know that the value at
`vectors[i]` is an address it should chase. Pointer-chasing gets zero
prefetching, and every dereference is a potential ~200-cycle stall.

## Applying it to layout A

```
std::vector<std::vector<float>> — one million separate heap allocations
```

```
outer array:  [ptr0][ptr1][ptr2][ptr3] ...        contiguous, fine
                 |     |     |     |
                 v     v     v     v
heap:         ...scattered wherever malloc had room...
```

The inner buffers land in *allocation* order, interleaved with whatever else
the program allocated, separated by malloc headers. Scan order is not
allocation order once anything has been freed or resized.

Three costs, in increasing order of importance:

1. **1,000,000 allocations.** Each is an allocator round trip on the way in and
   a free on the way out.
2. **~40 MB of overhead.** 24 bytes per `std::vector` (three pointers) plus
   ~16 bytes of malloc header, one million times.
3. **The prefetcher gives up.** This is the one that hurts. Each iteration
   dereferences a pointer to an unpredictable address, stalls on DRAM, and the
   loop becomes latency-bound rather than bandwidth-bound.

## Applying it to layout B

```
std::vector<float> — one allocation of 768,000,000 floats
```

```
[v0: 768 floats][v1: 768 floats][v2: 768 floats][v3: ...
 ^ contiguous, in scan order, forever
```

Vector `i` is at `data() + i * 768`. One multiply, no indirection. The scan
walks straight forward, the prefetcher stays several lines ahead the entire
time, and every cache line fetched is fully consumed before the next one is
needed.

That is our `VectorArray`.

## "Why not just..." — the objections

**"Why not `std::deque`?"** Chunked, so it avoids the giant reallocation. But
`operator[]` is a two-step lookup (which chunk, then which offset within it) and
the chunk size is not yours to control. In the hottest loop in the system, a
branch and an extra indirection per candidate is exactly what we are trying to
remove. *(That said — chunked storage is genuinely the right answer at 100M
vectors, where one contiguous 300 GB allocation is not obtainable. See the
alternatives section of ADR-0004.)*

**"Why not a `std::vector<std::array<float, 768>>`?"** Contiguous, and it works
— but the dimension becomes a compile-time constant. You would need a separate
instantiation of the entire engine for every dimension anyone might use.

**"Won't `push_back` copying 3 GB on reallocation be catastrophic?"** It would
be, which is why bulk loads call `reserve()` first. There is a unit test
asserting that 1000 `push_back`s after `reserve(1000)` cause zero
reallocations. Growth is still amortised O(1) even without it, but "amortised"
hides a single 3 GB memcpy you would rather not take.

**"Isn't this premature optimization?"** Premature optimization is contorting
code for a speedup you have not measured. This is *choosing a data structure*,
which is a design decision — and it is one you cannot change later without
touching the file format, the mmap path and every kernel. The cost of choosing
right now is zero; the cost of changing later is a rewrite.

## The consequence you must remember

```cpp
VectorView v = array[5];
array.push_back(something);   // may reallocate
float x = v[0];               // ☠️ dangling — v points into freed memory
```

`VectorView` is `std::span<const float>` — a pointer and a length. It does not
own anything and does not keep the array alive. This is exactly `std::vector`'s
iterator-invalidation rule, and the same discipline applies: **never hold a view
across an insertion.**

Under the `asan` preset this is caught instantly with a stack trace. Under
`release` it is a silent read of freed memory. Run the sanitizer.

## Experiments

1. **Measure the claim.** Write two programs that sum every float in 100,000
   vectors of dimension 768, one using each layout. Time both under the
   `release` preset. Write down the ratio before you run it, then compare.
2. **Break the locality deliberately.** In the contiguous version, visit slots
   in a random permutation instead of in order. Same data, same total bytes,
   same arithmetic — now measure. You have just isolated the prefetcher's
   contribution from everything else.
3. **Find the cliff.** Run experiment 1 at dimension 768 for N = 100, 1,000,
   10,000, 100,000. Plot time per vector. The curve is flat while the working
   set fits in L2 and steps up when it does not. You have located your L2 cache
   with a stopwatch.
4. **Watch it dangle.** Hold a `VectorView`, `push_back` enough to reallocate,
   then read the view. Run under `debug` (may appear to work — worse), then
   under `asan` (immediate diagnosis).

## Where this lives in the code

- `include/vectordb/core/vector_array.hpp` — the class, and the reasoning in its
  doc comment
- `src/core/vector_array.cpp` — `offset_of()`, the multiply that replaces a lookup
- `tests/unit/core_vector_array_test.cpp` —
  `StoresVectorsBackToBackInOneContiguousBlock` pins the layout;
  `ReserveAvoidsReallocationForABulkLoad` pins the allocation behaviour
- `docs/decisions/ADR-0004-contiguous-storage.md` — the full decision, including
  the padding option we did *not* take and the measurement that would change our
  minds

---

Next: [../30-vector-database/01-what-is-a-vector-database.md](../30-vector-database/01-what-is-a-vector-database.md)
