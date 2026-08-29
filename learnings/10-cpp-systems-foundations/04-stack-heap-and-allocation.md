# Stack, heap, and where allocations hide

🟡 Intermediate

## The two regions

**Stack** — a per-thread region, allocated by moving a pointer. Freed by moving
it back. Nanoseconds, always cache-hot, and bounded (8 MB by default on macOS,
often 512 KB per non-main thread).

**Heap** — a general-purpose allocator. `malloc` walks free lists, may take a
lock, may call the kernel. Tens to hundreds of nanoseconds, and the memory could
be anywhere.

```cpp
float buffer[768];              // stack — free
std::vector<float> v(768);      // heap — one malloc, one free, cache-cold
```

## Why it matters here

An allocation in a loop that runs millions of times per query is not a small
cost; it is *the* cost.

```cpp
for (candidate : candidates) {
    std::vector<float> temp(dimension);    // ☠️ N allocations per query
    ...
}
```

VectorDB's inner loops allocate **nothing**. That is a design constraint, not an
accident, and it takes deliberate structure to achieve.

## Where the allocations were removed

### Reserve once, outside the loop

```cpp
TopKCollector::TopKCollector(std::size_t k) : k_(k) {
    heap_.reserve(k);      // offer() then never allocates
}
```

`offer` is called once per candidate. An allocation there would dominate the
float comparison it exists to perform.

### Bulk loads pre-size

```cpp
database.reserve(count);   // one allocation instead of log2(n) copies
```

Without it, a bulk load performs a logarithmic number of
reallocate-and-copy rounds over an ever-larger array. At 1M vectors of dimension
768, the final copy alone moves 3 GB.

There is a test asserting that 1000 `push_back`s after `reserve(1000)` cause zero
reallocations.

### Reusable scratch

```cpp
struct SearchScratch {
    VisitedSet visited;                 // O(N) — the expensive one
    std::vector<Candidate> candidates;
    std::vector<Candidate> results;
};
thread_local SearchScratch scratch;
```

The visited set is `O(node count)`. Allocating it per query would dominate a
search that touches a few thousand nodes.

`thread_local` because `search` is `const` and must be safe under concurrent
readers — shared mutable scratch would be a data race.

### The cached-norm trade

Cosine needs `|b|` for every candidate. Computing it is an `O(D)` pass; storing
it is 4 bytes per vector.

**4 bytes of memory to avoid 2·D bytes of traffic per query** — a 1536:1 return
at D = 768. This is the most common shape of a systems optimisation: precompute
something small that is reused, to avoid recomputing something large.

## Allocations you cannot see

```cpp
std::function<void(int)> f = [big_capture] { ... };  // heap if the capture is large
std::string s = a + b;                                // heap unless very short
std::vector<std::vector<float>> v;                    // one per inner vector
```

**Small string optimisation**: most implementations store strings up to ~15–22
characters inline. `std::to_string(42)` does not allocate; a 40-character path
does.

**`std::function`** has a small-buffer optimisation too, but a lambda capturing
several words will spill to the heap. In a hot path, prefer a template parameter
or a plain function pointer — which is why `DistanceKernel` is a struct of
function pointers.

## The stack has a limit

```cpp
void f() { float buffer[1000000]; }   // 4 MB — likely a stack overflow
```

Especially on a non-main thread, where 512 KB is common. And a stack overflow
does not throw; it faults, often with a confusing trace.

Rule of thumb: **anything over a few KB, or sized at runtime, goes on the heap.**

`alloca` and VLAs exist and are a bad idea — they move the failure to run time
with no diagnostic.

## Measuring

```sh
# macOS: peak RSS
/usr/bin/time -l ./build/release/bin/vectordb-bench
# Linux
/usr/bin/time -v ./build/release/bin/vectordb-bench

# Count allocations
valgrind --tool=massif ./build/release/bin/vectordb_tests
heaptrack ./build/release/bin/vectordb-bench        # Linux
```

`vectordb stats` reports resident memory split by component.

## Experiments

1. Add `std::vector<float> temp(dimension);` inside the brute-force scan loop and
   measure. That is what an allocation in a hot loop costs.
2. Remove `heap_.reserve(k)` from `TopKCollector` and measure at k = 10 and
   k = 1000.
3. Remove `database.reserve()` from the import path and time a 100,000-vector
   import. Also watch peak RSS — the reallocation needs both buffers live.
4. Make `SearchScratch` a local instead of `thread_local` and measure a search at
   N = 1,000,000. You are measuring the visited-set allocation.
5. Allocate a 4 MB array on the stack of a worker thread. Note that it does not
   throw.

## Where this lives in the code

- `src/core/top_k.cpp` — reserve in the constructor
- `src/index/hnsw/hnsw_index.cpp` — `SearchScratch`, `VisitedSet`
- `src/db/database.cpp` — `reserve`
- `src/storage/vector_store.cpp` — the cached norms
- `tests/unit/core_vector_array_test.cpp` — `ReserveAvoidsReallocationForABulkLoad`

---

Next: [05-files-and-binary-data.md](05-files-and-binary-data.md)
