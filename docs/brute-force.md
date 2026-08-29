# Brute-force index

Exact nearest-neighbour search by scanning every live vector.

Implementation: `src/index/brute_force/brute_force_index.cpp`.
Decision record: [ADR-0006](decisions/ADR-0006-brute-force-first.md).

## The algorithm

```
for each slot in [0, slot_count):
    if not live(slot): continue
    key = distance(query, vector(slot))
    collector.offer(slot, key)
return collector.take_sorted()
```

`O(N·D)` time, `O(k)` extra memory.

## It holds no state

```cpp
void add(LocalId) noexcept { }
void remove(LocalId) noexcept { }
std::size_t index_bytes() const noexcept { return 0; }
```

Not stubs. The store already knows which slots exist and which are tombstoned,
and the scan reads both directly.

That emptiness is the defining property: an index that stores nothing **cannot
be stale, cannot be corrupted, and needs no persistence.** Every one of HNSW's
failure modes is the price of having derived state.

## Why it ships

**It is the oracle.** HNSW is approximate and fails silently — results come back,
distances look plausible, latency is fine. "Is HNSW correct?" is only answerable
relative to something exact. Every recall number in this project is measured
against this class.

**It is faster below a few tens of thousands of vectors.** No graph to build,
persist, load or traverse, and perfectly sequential memory access.

**It is the fallback.** If an index file fails validation, exact search still
works while the graph rebuilds. Degrading to "slower but correct" beats "fast and
wrong".

## Two optimisations in the loop

```cpp
const float query_norm = metric_ == Metric::kCosine ? l2_norm(query) : 0.0F;

for (LocalId id = first; id < last; ++id) {
    if (!accessor_.live(id)) continue;
    const float key = distance_.with_norms(query, query_norm,
                                           accessor_.vector(id), accessor_.norm(id));
    collector.offer(id, key);
}
```

1. **The query norm is hoisted** — it is a property of the query, not the
   candidate. Computing it inside would add an `O(D)` pass per candidate.
2. **The stored norm is read, not computed** — cached at insert.

Together these take cosine from three passes over the data per candidate to one.

## `search_range`

```cpp
void search_range(VectorView query, LocalId first, LocalId last,
                  TopKCollector& collector) const noexcept;
```

Scans a slice into a caller-supplied collector. A thread pool gives each worker a
range and its own collector; `merge_top_k` combines them. No per-worker
allocation.

Written before threads existed, so concurrency became a scheduling change rather
than a rewrite of the search loop. There is a test that four merged slices give
*exactly* the same answer as one whole scan.

## Measured

Apple M1, Release, NEON, `k = 10`, L2:

| dim | N | p50 | p95 | effective GB/s |
|---|---|---|---|---|
| 128 | 10,000 | 133 µs | 136 µs | 38.5 |
| 128 | 100,000 | 1,289 µs | 1,372 µs | 39.7 |
| 768 | 10,000 | 643 µs | 669 µs | 47.8 |
| 768 | 100,000 | 6,413 µs | 7,648 µs | 47.9 |

**Latency is linear in N**, as `O(N·D)` predicts.

**Throughput plateaus at 38–48 GB/s regardless of dimension** — that is the
machine's memory bandwidth. The scan is **bandwidth-bound, not compute-bound**,
which is why a 3.9× SIMD speedup on the kernel does not become 3.9× here, and why
HNSW's advantage comes from touching less data rather than computing faster.

## When to use it

| Use brute force when | Use HNSW when |
|---|---|
| N is small (< ~10,000) | N is large |
| You need exact answers | Approximate is fine |
| Data changes constantly | Data is mostly static |
| The filter is very selective | The filter is permissive or absent |
| You are measuring something else | You are serving queries |

```sh
vectordb create mydb --dimension 768 --index brute_force
vectordb search mydb --query-file q.vecs --exact    # force it on an HNSW database
```
