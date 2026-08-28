# Brute force — the index that is not an index

🟢 Foundational → 🟡 Intermediate · pairs with `src/index/brute_force/`

## The algorithm you already know

```
for each stored vector v:
    d = distance(query, v)
    offer(v, d) to a bounded top-k heap
return the heap, sorted
```

`O(N·D)` time, `O(k)` extra memory, exactly correct. You could have written it
before opening this repository.

So why is it worth a phase of its own, and why does it survive once HNSW
exists?

## Reason 1: it is the oracle

HNSW is approximate. Crucially, **it fails silently**: results come back,
distances look plausible, latency is fine. There is no internal signal saying
"that was wrong".

So "is my HNSW correct?" is unanswerable in isolation. It only becomes
answerable relative to something exact:

```
recall@10 = |HNSW top-10 ∩ brute-force top-10| / 10
```

That turns a vague worry into a number you can put a threshold on, track across
commits, and trade against latency by turning `efSearch`.

This is a pattern far beyond vector search. **Before you build the fast
approximate thing, build the slow exact thing, and keep it.** The slow one is
how you find out whether the fast one works. Compilers keep an interpreter.
Physics engines keep a reference solver. Graphics keeps a path tracer.

## Reason 2: below a few tens of thousands of vectors, it wins

HNSW's advantage is asymptotic and it has real fixed costs: build time,
memory for the graph, a file to persist and validate, and pointer-chasing
through a graph that does not prefetch.

Brute force has none of those and streams memory perfectly linearly. For 10,000
vectors of dimension 128 — 5 MB, comfortably in L2 — the scan is fast enough
that the graph never pays for itself.

## Reason 3: it is the fallback

If an index file fails validation, the vector store is still authoritative, so
exact search keeps working while the graph is rebuilt. Degrading to "slower but
correct" is a much better failure mode than "fast and wrong".

## The design detail worth noticing

```cpp
void BruteForceIndex::add(LocalId) noexcept { }
void BruteForceIndex::remove(LocalId) noexcept { }
std::size_t index_bytes() const noexcept { return 0; }
```

Those are not stubs. **Brute force holds no state.** The store already knows
which slots exist and which are tombstoned, and the scan reads both directly.

That emptiness is the point: an index that stores nothing cannot be stale,
cannot be corrupted, and needs no persistence. Every one of HNSW's failure modes
is a consequence of having derived state to keep in sync, and it is worth
seeing clearly what that state buys and what it costs.

## Why the index does not own the vectors

`BruteForceIndex` holds a `VectorAccessor` — three pointers into the store's
arrays. Not a copy.

Two copies of a million 768-dimensional vectors is 6 GB instead of 3, and — the
worse problem — two things that could disagree about what the data is.

**The store is authoritative. Indexes are derived, disposable state.** That
asymmetry decides the whole persistence design: a corrupt index is a rebuild, a
corrupt vector store is data loss.

Note also that `VectorAccessor` is deliberately *not* a virtual interface,
while `VectorIndex` is. The difference is call frequency:

| | called | virtual cost |
|---|---|---|
| `VectorIndex::search` | once per query | free — it does thousands of distance computations |
| `VectorAccessor::vector` | once per candidate | inside that loop; also blocks inlining |

**Virtual dispatch is priced per call site, not per codebase.** The same
mechanism is free in one place and unacceptable ten lines away.

## Two optimisations that are already in the loop

```cpp
const float query_norm = metric_ == Metric::kCosine ? l2_norm(query) : 0.0F;

for (LocalId id = first; id < last; ++id) {
    if (!accessor_.live(id)) continue;
    const float key = distance_.with_norms(query, query_norm,
                                           accessor_.vector(id), accessor_.norm(id));
    collector.offer(id, key);
}
```

1. **The query norm is hoisted.** It is a property of the query, not of the
   candidate. Computing it inside the loop would add an `O(D)` pass per
   candidate — doubling the work for cosine.
2. **The stored norm is read, not computed.** Cached at insert time. 4 bytes per
   vector to avoid `2·D` bytes of traffic per query.

Together these take cosine from three passes over the data per candidate to
one, which is the difference between cosine being expensive and cosine being
free.

## `search_range` — designed for parallelism before parallelism exists

```cpp
void search_range(VectorView query, LocalId first, LocalId last,
                  TopKCollector& collector) const noexcept;
```

Scans a slice into a caller-supplied collector. Four things fall out:

- a thread pool gives each worker a slice and its own collector, then
  `merge_top_k` combines them;
- no per-worker allocation, because the collector is supplied;
- a batch of queries can reuse one collector across all of them;
- and there is a test that four slices merged give *exactly* the same answer as
  one whole scan — which is the property parallelism must not break.

Writing the sliced form first, before threads exist, means concurrency later is
a scheduling change rather than a rewrite of the search loop.

## Why it is bandwidth-bound, not compute-bound

At N = 1,000,000 and D = 768, each query reads:

```
1,000,000 × 768 × 4 bytes = 3.07 GB
```

and performs about 1.5 billion floating-point operations (a subtract, a
multiply and an add per component).

An M1 core sustains roughly 50 GB/s from DRAM and can retire far more than 1.5
billion FLOPs per second when fed. So the arithmetic finishes long before the
bytes arrive: **the loop waits on memory**.

Two consequences that shape everything downstream:

- **SIMD helps less than you would hope.** Making the arithmetic 4× faster does
  not help a loop that is waiting for data. (Measure it. It still helps some —
  fewer instructions means the prefetcher gets further ahead — but not 4×.)
- **The real win is touching less data.** That is precisely what HNSW does: it
  visits maybe 0.1% of the vectors. Not faster arithmetic — *less* arithmetic.
  Same idea as quantization, which makes each vector smaller instead of visiting
  fewer of them.

## Experiments

1. Time brute force at N = 1k, 10k, 100k, 1M with D = 128, in `release`. Confirm
   the linearity, then compute achieved GB/s and compare it with your machine's
   memory bandwidth. How close to the roofline are you?
2. Hold N·D constant — (N=100k, D=128) versus (N=12.5k, D=1024) — and compare.
   Same bytes, same FLOPs. Explain any difference in terms of the per-candidate
   overhead (the indirect call, the liveness check, the heap comparison).
3. Delete the `live()` check and re-time. You have measured what tombstones cost
   per query, which is the input to deciding when compaction is worth running.
4. Move `l2_norm(query)` inside the loop and re-time under cosine. Predict the
   ratio first.
5. Set `k = 1` and `k = 1000` and compare. Where does the top-k heap start to
   matter relative to the distance computation?

## Where this lives in the code

- `include/vectordb/index/vector_index.hpp` — the interface both indexes satisfy
- `include/vectordb/index/vector_accessor.hpp` — the non-owning view, and why it
  is not virtual
- `include/vectordb/index/brute_force_index.hpp` — the class, and why keeping it
  matters
- `src/index/brute_force/brute_force_index.cpp` — the scan, with the hoisted
  query norm
- `tests/unit/index_brute_force_test.cpp` — hand-computed geometry, tombstone
  skipping, cosine-ignores-magnitude, and agreement with a full sort
- `docs/decisions/ADR-0006-brute-force-first.md`

---

Next: [02-approximate-nearest-neighbour.md](02-approximate-nearest-neighbour.md)
