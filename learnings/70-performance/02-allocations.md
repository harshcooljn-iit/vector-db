# Allocations — the cost you did not write down

🟡 Intermediate

## What `new` actually does

```cpp
std::vector<float> v(768);      // looks like one line
```

Underneath: `operator new` → `malloc` → walk a free list, possibly take a lock,
possibly `mmap` from the kernel, return a pointer to memory that is **cache-cold**
and must be faulted in on first touch.

Tens to hundreds of nanoseconds, and unpredictable — the tail is much worse than
the mean.

Compare: a stack allocation is moving a register.

## The rule

> **No allocation in a loop that runs per candidate.**

A distance computation at D = 768 takes ~50 ns with NEON. One allocation costs
about the same. Allocating per candidate doubles your search time before you have
computed anything.

## Where VectorDB removed them

### `TopKCollector`

```cpp
TopKCollector::TopKCollector(std::size_t k) : k_(k) { heap_.reserve(k); }
```

`offer` runs once per candidate — millions of times per query — and never
allocates. It replaces the heap root in place rather than pushing and popping.

### HNSW search scratch

```cpp
thread_local SearchScratch scratch;   // visited set, two heaps, working buffers
```

The visited set is `O(node count)`. Allocating it per query would dominate a
search that touches a few thousand nodes.

`thread_local` because `search` is `const` and must be safe under concurrent
readers — a shared mutable member would be a data race.

The cost is stated in the header: those buffers live for the life of the thread,
and a thread that searched a 10M-node index keeps a 40 MB visited set.

### Bulk loads

```cpp
database.reserve(count);
```

Without it, growth reallocates and copies logarithmically often. At 1M × 768
floats the final copy alone moves 3 GB — and needs both buffers resident, so peak
memory nearly doubles.

There is a test asserting 1000 `push_back`s after `reserve(1000)` cause zero
reallocations.

### Chunked `parallel_for`

One task per index would be one queue push, one mutex acquisition and one notify
per item. Contiguous chunks amortise that over many iterations.

## The trade that keeps recurring

| Spend | Save | Ratio at D=768 |
|---|---|---|
| 4 bytes per vector (cached norm) | 2·D bytes of traffic per query | **1536:1** |
| `k` × 8 bytes (top-k heap) | N × 8 bytes (sorting everything) | N/k |
| `O(N)` visited set per thread | an `O(N)` memset per query | queries per thread |

**Precompute something small that is reused, to avoid recomputing something
large.** Almost every optimisation in this codebase is that shape.

## Allocations that hide

```cpp
std::function<void(int)> f = [big_capture] { ... };   // heap if capture is large
std::string s = a + b;                                 // heap unless short
return std::vector<Candidate>{...};                    // one, unless RVO applies
```

**Small string optimisation** stores strings up to ~15–22 chars inline. So
`std::to_string(42)` is free and a 40-character path is not.

**`std::function`** has a small-buffer optimisation too, but a lambda capturing
several words spills. This is why `DistanceKernel` is a struct of function
pointers rather than `std::function` — the hot path must not allocate.

## Returning by value is usually fine

```cpp
std::vector<Candidate> search(...) const;    // one allocation per query
```

Guaranteed copy elision (C++17) means the vector is constructed directly in the
caller. **Once per query is not a hot path** — once per candidate is.

Know the difference before optimising. Chasing the per-query allocation would buy
nothing and make the API worse.

## Measuring

```sh
/usr/bin/time -l ./build/release/bin/vectordb-bench      # macOS: peak RSS
/usr/bin/time -v ./build/release/bin/vectordb-bench      # Linux
valgrind --tool=massif ./build/release/bin/vectordb_tests
heaptrack ./build/release/bin/vectordb-bench             # Linux, counts them
```

`vectordb stats` reports resident memory split by component.

## Experiments

1. Add `std::vector<float> temp(dimension);` inside the brute-force scan loop and
   measure. That is the cost of one allocation per candidate.
2. Remove `heap_.reserve(k)` and measure at k = 10 and k = 1000.
3. Remove `database.reserve()` from import and measure both time and **peak RSS**
   for 100,000 vectors.
4. Make `SearchScratch` a local and measure a search at N = 1,000,000.
5. Replace `DistanceKernel`'s function pointers with `std::function` and measure.

## Where this lives in the code

- `src/core/top_k.cpp` — reserve in the constructor, replace-in-place in `offer`
- `src/index/hnsw/hnsw_index.cpp` — `SearchScratch`, `VisitedSet`
- `src/db/database.cpp` — `reserve`
- `src/concurrency/thread_pool.cpp` — chunked `parallel_for`

---

Next: [03-simd-introduction.md](03-simd-introduction.md)
