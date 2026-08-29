# VectorDB — engineering report

A local-first vector database in C++20. This report covers what was built, how
it performs, what it does not do, and what was learned building it.

**Date:** 2026-08-29 · **Version:** 0.1.0 · **Tests:** 327 passing

---

## 1. What it is

An embedded vector database — SQLite's model, not Postgres's. One process, files
on disk, no server.

It stores `float32` vectors of a fixed dimension with optional metadata, and
answers k-nearest-neighbour queries either exactly or approximately.

The search algorithms are implemented from scratch. FAISS, hnswlib and every
other ANN library are excluded on purpose
([ADR-0001](decisions/ADR-0001-implement-algorithms-ourselves.md)): the
interesting engineering is exactly what a library encapsulates.

### Scope

| Implemented | Deliberately excluded |
|---|---|
| Exact and approximate search | Distributed / sharded operation |
| Three metrics, SIMD kernels | Network API, authentication, GUI |
| Versioned durable storage | GPU acceleration |
| Metadata and filtering | Quantization (PQ / IVF) |
| Concurrent reads | Concurrent writes |
| A CLI | `float16` / `int8` storage |

---

## 2. Architecture

Eight layers, with dependencies pointing downward only:

```
CLI → ConcurrentDatabase → Database → { Index, Storage } → { Distance, Persistence } → Core
```

The rule is enforced rather than requested. SQLite's wrapper header lives under
`src/storage/` and the library links `sqlite3` `PRIVATE`, so `#include
<sqlite3.h>` anywhere above the metadata store is a compile error.

A database is a directory:

```
mydb/
├── vectors.bin       authoritative
├── index.hnsw        derived, disposable
└── metadata.sqlite   metadata rows + configuration
```

**That asymmetry is the central design decision.** A corrupt index is a rebuild;
a corrupt vector store is data loss. It determines the recovery path, the flush
ordering, and what `check` treats as an error rather than a warning.

Full write-up: [architecture.md](architecture.md).

---

## 3. Vector representation

One contiguous `std::vector<float>` of `count × dimension`, vector `i` at
`data() + i * dimension`.

The obvious alternative — `std::vector<std::vector<float>>` — costs, at 1M × 768:
a million allocations, ~40 MB of pointer overhead, and (decisively) inner buffers
scattered in allocation order rather than scan order, which defeats the
prefetcher and turns the hottest loop from bandwidth-bound into latency-bound.

**Stride equals dimension — no padding.** Padding would let SIMD kernels drop
tail handling, but it would fork the in-memory layout from the on-disk layout and
forfeit direct memory mapping. `VectorArray::stride()` exists separately from
`dimension()` so reversing that decision stays contained
([ADR-0004](decisions/ADR-0004-contiguous-storage.md)).

Vectors, norms, liveness and ids are **parallel arrays**, because they are read
by different loops at wildly different rates: the distance loop touches vectors
and norms N times per query, the id map k times.

### Two id spaces

| | Type | Stable | Used by |
|---|---|---|---|
| `VectorId` | `uint64` | forever | users, metadata, results |
| `LocalId` | `uint32` | until compaction | storage offsets, graph edges |

At 1M vectors with M=16, HNSW stores ~32M references: 128 MB at 32 bits, 256 MB
at 64. And a dense `LocalId` makes vector lookup a multiply-add rather than a
hash probe, in the innermost loop.

It is also what lets compaction renumber slots without invalidating a single user
reference ([ADR-0003](decisions/ADR-0003-id-model.md)).

---

## 4. Distance kernels

Three metrics over three swappable primitives (`l2_squared`, `dot`,
`norm_squared`), so adding an instruction set means adding a table rather than
editing a caller.

**The ordering convention** is the load-bearing part: internally every metric
produces a `RankKey` where **smaller is better** — L2 keeps the squared form,
cosine becomes `1 − cos`, inner product is negated. Every comparison in the
engine is then `<`, with no per-metric branching and no direction flag to get
wrong. Conversion to a human-facing score happens once per *result*, not per
candidate.

The scalar kernel uses **four independent accumulators**, because a float add
has several cycles of latency but issues every cycle — one accumulator leaves
most of the pipeline idle. The accuracy improvement falls out of the speed fix.

`-ffast-math` is deliberately not enabled: it would let the compiler perform that
transformation invisibly, which would make the scalar-vs-SIMD equivalence test
compare two moving targets, and it implies `-ffinite-math-only`, permitting the
deletion of the NaN checks the whole design depends on.

### Measured

| dim | scalar | NEON | speedup |
|---|---|---|---|
| 128 | 37.1 ns | 10.3 ns | 3.59× |
| 384 | 100.4 ns | 24.3 ns | 4.13× |
| 768 | 200.0 ns | 51.0 ns | 3.92× |
| 1536 | 398.7 ns | 106.7 ns | 3.74× |

AVX2 is implemented, guarded by runtime detection (including the XCR0 check that
verifies the OS preserves YMM registers) and validated by the same test suite —
but **has never been executed on x86-64 hardware**, so no AVX2 numbers are
reported.

---

## 5. Brute force

`O(N·D)`, and it holds no state: `add` and `remove` are genuinely empty,
`index_bytes()` genuinely returns 0. The store already knows which slots exist
and which are dead.

That emptiness is the point. An index that stores nothing cannot be stale, cannot
be corrupted and needs no persistence — every one of HNSW's failure modes is the
price of having derived state.

It ships because it is **the oracle**. HNSW fails silently, so "is it correct?" is
only answerable relative to something exact.

| dim | N | p50 | effective GB/s |
|---|---|---|---|
| 128 | 100,000 | 1,289 µs | 39.7 |
| 768 | 100,000 | 6,413 µs | 47.9 |

Throughput plateaus at 38–48 GB/s regardless of dimension — the machine's memory
bandwidth. The scan is **bandwidth-bound**, which is why a 3.9× SIMD speedup on
the kernel does not become 3.9× here, and why HNSW's advantage comes from
touching less data rather than computing faster.

---

## 6. HNSW

A layered proximity graph. Greedy search in a single near-neighbour graph gets
stuck in local minima; stacking sparse graphs above the full one means the search
takes huge strides through a nearly-empty top layer and arrives *inside the right
basin* before the expensive layer starts.

Because the top layers are sparse, their nodes are far apart and their edges span
long distances — **the long-range links are a consequence of sparseness, not a
design**.

### Memory layout

Layer 0 holds every node and takes nearly all the traffic, so its links are one
flat array of stride `2M+1`, with the neighbour count in slot 0 so it shares a
cache line with the neighbours it describes. Upper layers hold ~1/M of the nodes
each, so a fixed stride would be ~94% empty; they share an append-only arena.

### Three pieces do the work

**`random_level`** draws from an exponential distribution with `mL = 1/ln(M)`, so
each layer holds ~1/M of the one below and the tower is `~log_M(N)` deep. That
constant is what makes the descent logarithmic.

**`search_layer`** is best-first with two heaps in opposite directions — a
min-heap frontier and a max-heap of the best `ef`. It stops when the nearest
unexpanded node is worse than the worst result held. **That single line is where
the approximation lives**; everything else is exact given the graph.

**`select_neighbours`** is the diversity heuristic, and the part most often
replaced by "take the closest M". A candidate is accepted only if it is closer to
the target than to anything already chosen — otherwise it is reachable *through*
an existing pick and the edge is redundant. The result is a neighbourhood that
fans out in different directions, which is what greedy search needs.

`VisitedSet` uses generation stamps rather than a cleared bitset: at a million
nodes, a 125 KB memset before every query is often more work than the query.

Search scratch is `thread_local`, because `search` is `const` and must be safe
under concurrent readers.

### Measured — N = 50,000, D = 128, M = 16, efConstruction = 200

Build: 7.73 s (6,466 vectors/s). Index: 6 MB, 145 bytes/vector, 5 layers.

| efSearch | p50 | recall@10 | vs brute force |
|---|---|---|---|
| 10 | 20.0 µs | 0.9590 | **32.3×** |
| 20 | 25.0 µs | 0.9880 | 25.9× |
| 100 | 46.6 µs | 1.0000 | 13.9× |
| 400 | 149.1 µs | 1.0000 | 4.3× |

**The knee is at efSearch ≈ 20** — the default of 64 is conservative for
datasets of this size. Index overhead is 145 bytes/vector independent of
dimension: 28% on top of a D=128 payload, 4.7% at D=768.

---

## 7. Persistence

Versioned binary formats with magic, CRC and explicit field serialization —
nothing is ever written by dumping a struct. `sizeof({u32,u64,u32})` is 16 rather
than 12, the padding is uninitialised, and the layout is an ABI property that
changes with the compiler.

### Atomic replacement

```
write to .tmp → fsync → rename → fsync the directory
```

The last step is the one everybody forgets: `rename` modifies a directory entry,
which is separately cached metadata, so without it a crash can leave the new
contents durable while the directory still points at the old name. On macOS the
fsync is `fcntl(F_FULLFSYNC)`, because plain `fsync` only reaches the drive's own
volatile cache.

### Ordering

Vectors first, index second. A crash between them leaves an index disagreeing
with the store — detectable at open and recoverable by rebuilding. The reverse
would leave an index describing vectors that were never written.

> When you cannot make two writes atomic, order them so the detectable
> inconsistency is the recoverable one.

### Validation

Nine steps for `vectors.bin`, cheapest first. Two rules generalise:

- **A length read from a file is checked against the bytes that remain before it
  is used to size anything.** Otherwise a truncated file becomes a 300-exabyte
  allocation.
- **Arithmetic on file-supplied values is overflow-checked.** A header claiming
  2⁴⁰ slots of dimension 60000 wraps in 64-bit arithmetic to a small, plausible
  size. Validation that itself overflows is not validation.

The index adds a full pass over every neighbour reference. `O(edges)`, and it
buys two things: the search loop never bounds-checks, and a damaged graph is
reported rather than returning plausible wrong answers forever.

**Payload checksums are not verified on open** — a documented gap, with
`check --deep` as the explicit command and tests for both halves.

### Measured

| | |
|---|---|
| Flush 50,000 × 128 | 0.113 s |
| Reopen + validate | **0.009 s** |
| Rebuild instead | 7.8 s |

**860×** — the entire justification for persisting the index.

---

## 8. Metadata and filtering

SQLite, in entity-attribute-value form with `WITHOUT ROWID`, so one vector's
metadata is physically adjacent and reads as a single range scan. An explicit
type column, because SQLite is dynamically typed and `"2026"` would otherwise be
indistinguishable from `2026`.

The filter language is six operators combined with `and`. **No `or`, no
nesting** — the useful 95% of vector-search filtering is narrowing predicates,
and every construct beyond that adds a parser case, an evaluator case and a class
of bug.

Two semantics are choices, documented and tested: a missing key never matches
(including for `!=`), and cross-type comparison never matches.

**Filtered approximate search over-fetches and post-filters**, which degrades
with selectivity. `--exact` is the escape hatch, and the trade is stated rather
than hidden.

---

## 9. Concurrency

Many readers or one writer, via `std::shared_mutex` in an opt-in wrapper — so
single-threaded callers pay nothing for a lock they do not need.

`MetadataStore` breaks that pattern and locks itself, because **its reads are not
read-only**: SQLite prepared statements carry a cursor, so two concurrent readers
corrupt each other. No caller-side reader/writer lock can fix that.

### Measured — N = 50,000, D = 128

| threads | queries/s | scaling |
|---|---|---|
| 1 | 51,368 | 1.00× |
| 4 | 187,410 | 3.65× |
| 8 | 214,987 | **4.19×** |

Near-linear to the physical core count, then flat — the M1 has 4 performance and
4 efficiency cores, and the workload is memory-bound.

Concurrent writers are **not supported and not claimed**.

---

## 10. Testing

327 tests: unit, integration, CLI (driving the real binary) and concurrency
(also run under ThreadSanitizer).

Two kinds of assertion, because an approximate algorithm needs both. **Graph
structure is checked exactly** — no dangling references, no self-links,
populations thinning geometrically by M. **Result quality is checked against
thresholds** via recall as set intersection.

Thresholds sit just below what was measured, not at a comfortable margin: the
headline test asserts `> 0.99` against a measured 1.000, because a 0.95 threshold
would have passed the bug that produced 0.752.

### What the tests actually caught

- A **SQLite prepared statement shared across threads** — eight legitimate
  readers corrupting each other.
- A **missing `add_subdirectory(persistence)`** — invisible until a clean
  configure, because the stale build directory still had the objects.
- The **query-generation bug** below.

---

## 11. Three bugs worth recording

### Recall 0.752 on a correct index

`generate_queries` derived a fresh seed for everything including the cluster
centroids, so queries inhabited a different cluster layout and landed in the
empty space between the dataset's clusters. A query's 10th nearest neighbour sat
3% further than its 1st, against 15% for a genuine in-distribution point.

Recall was measuring tie-breaking among hundreds of near-equidistant points.
Fixed by separating the structure seed from the sample seed; the same
configuration now measures 0.999.

**If an ANN benchmark shows terrible recall, suspect the data before the index.**

### 13 million queries per second

The batch benchmark reported a physically impossible number because the setup
never flushed before reopening — it was searching an empty database. Nothing
failed; it produced a beautifully formatted table.

The driver now asserts the database holds the expected count and that a probe
query returns `k` results, before timing.

**A benchmark without a correctness assertion measures whatever it happens to be
doing, and a broken configuration is usually much faster than a working one.**

### A scaling collapse two layers away

Batch search scaled 0.90× at 8 threads while HNSW search alone scaled 4.3×. Four
hypotheses — mutex contention, allocator contention, `validate_vector`, the
thread pool — each tested with an isolating benchmark, each wrong.

A four-second `sample` found it: 89% of search time in `MetadataStore::get`,
blocked on that class's mutex. `Database::search` was fetching metadata for every
result even when the database had none and the caller never read it.

Single-thread throughput went from 28,357 to 51,368 q/s; scaling from 0.90× to
4.19×.

**Profile before you theorise.**

---

## 12. Limitations

Stated rather than left to be discovered.

| Limitation | Why |
|---|---|
| No concurrent writers | needs per-node locking and a reclamation scheme |
| Durability at `flush`, not per insert | no WAL; a crash loses un-flushed inserts |
| AVX2 never run on real hardware | developed on Apple Silicon; guarded, validated, unmeasured |
| mmap saves a copy, not the footprint | searching a live mapping is not implemented |
| Filtered ANN degrades with selectivity | post-filtering; `--exact` is the escape hatch |
| `float32` only, one dimension per database | no quantization, no collections |
| Payload checksums not verified on open | seconds per open; `check --deep` instead |
| N = 1,000,000 not benchmarked | 8 GB machine; would measure swap at D=768 |
| No comparison against FAISS or hnswlib | the most useful missing number here |

---

## 13. What would come next

In order of value per effort ([full list](../learnings/next-steps.md)):

1. **Product quantization** — 32× less memory, and it composes with HNSW.
2. **An hnswlib baseline** — unglamorous, and the single most useful number this
   report is missing.
3. **Search over a live mapping** — opens databases larger than RAM.
4. **A write-ahead log** — closes the durability gap.
5. **Concurrent inserts** — or sharding, which is much simpler and often enough.

---

## 14. What the project demonstrates

Exactly one of the eight problems in building this was an algorithms problem. The
other seven — memory layout, durability, crash consistency, corruption,
concurrency, SIMD, honest measurement — are systems problems, and that ratio is
the point.

Three ideas recur and generalise well beyond vector search:

**Layout follows access pattern.** Contiguous storage, struct-of-arrays, the
neighbour count in slot 0, a flat layer 0 and an arena above it — every one is
the same question answered for a different loop.

**Validate at the boundary, assume inside.** NaN is rejected at insert so the
inner loop can assume a total order. Graph references are validated at load so
the search loop never bounds-checks. Both make the hot path *simpler* as well as
faster.

**Name what you did not do.** Concurrent writers, AVX2 measurements,
million-vector benchmarks, payload checksums on open — each is absent, and each
is listed as absent. An unimplemented feature is a limitation; an undocumented
one is a trap.

---

## Appendix — reproducing

```sh
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset release && cmake --build --preset release
ctest --preset release                        # 327 tests
./build/release/bin/vectordb-bench            # every number in this report
./examples/01-quickstart.sh
```

Measured on Apple M1, 8 cores, 8 GB, macOS 15.5, AppleClang 17.0.0, Release,
NEON. Full methodology: [benchmarking.md](benchmarking.md).
