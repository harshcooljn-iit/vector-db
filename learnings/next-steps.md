# Next steps

Where to take this after v1. Each entry says what it teaches, why it is hard,
what you need first, and where it plugs in.

Ordered roughly by value-per-effort.

---

## 1. Product quantization (PQ)

**What it teaches.** That there are two independent ways to make search faster:
touch fewer vectors (HNSW) and make each vector smaller (quantization). They
compose.

**The idea.** Split a 768-dimensional vector into 96 sub-vectors of 8 dimensions.
Cluster each sub-space into 256 centroids. Store 96 bytes per vector instead of
3072 — a **32× reduction**. Compute approximate distances from a lookup table,
then rescore the top candidates exactly.

**Why it is hard.** You need k-means, a codebook trained on a sample, an
asymmetric distance computation, and a rescoring pass. The accuracy loss is real
and must be measured.

**Prerequisites.** k-means; comfort with the recall harness.

**Where it plugs in.** A new element type in `VectorArray`, a codebook in the
file format (version bump), and a new `DistanceKernel` over codes. The index
above it does not change — which is the payoff of the kernel abstraction.

---

## 2. An hnswlib backend, purely as a baseline

**What it teaches.** How far a from-scratch implementation is from a mature one —
the single most useful number missing from `docs/benchmark-results.md`.

**Why it is hard.** It is not. That is the point.

**Where it plugs in.** A third `VectorIndex` implementation behind the existing
interface, off by default, built only when a CMake option is set.

**Why do it.** Right now the docs say "we are slower than hnswlib and we say so".
Saying it with a number is better. It may also expose a real bug.

---

## 3. Memory-mapped search without loading

**What it teaches.** The difference between "we use mmap" and "we never
materialise the data".

**The gap.** `read_vector_file` maps the file and then **copies** into a
`VectorStore`. That saves the second copy and the explicit `read`, but the
resident footprint is unchanged — which the mmap note states plainly.

**The goal.** A database larger than RAM that opens instantly and pages in only
what a query touches.

**Why it is hard.** `VectorStore` must hold a borrowed, read-only payload as well
as an owned one. Writes then need copy-on-write or an overlay. And an I/O error
becomes SIGBUS, which cannot portably become a C++ exception.

**Prerequisites.** [50-storage/03](50-storage/03-memory-mapped-files.md).

**Where it plugs in.** `VectorStore` gains a borrowed mode; `VectorAccessor` is
already the right abstraction and does not change.

---

## 4. Write-ahead logging

**What it teaches.** How real durability is built — the gap between "consistent
after a crash" and "nothing is lost".

**The gap.** Today, vectors are durable at `flush`. A crash loses un-flushed
inserts. That is stated honestly, and it is still a gap.

**Why it is hard.** A log format, a replay path, checkpointing, log truncation,
and a recovery routine exercised often enough to be trustworthy. Recovery code
that runs once a year is recovery code that does not work.

**Prerequisites.** [80-database-engineering/02](80-database-engineering/02-crash-safety.md).

**Where it plugs in.** Between `Database::insert` and `flush`. The vector file
format is unchanged; a `wal.log` joins the directory.

---

## 5. Concurrent inserts

**What it teaches.** Fine-grained locking on a mutable graph, which is genuinely
hard and genuinely instructive.

**Why it is hard.** Per-node locks need a fixed acquisition order or you
deadlock. Replacing a neighbour list while a reader traverses it needs epoch
reclamation or hazard pointers. And the entry point can move.

**Easier alternative worth considering first.** Shard the index and give each
shard its own lock; a query fans out and merges. Much simpler, often enough.

**Prerequisites.** [60-concurrency/06](60-concurrency/06-atomics.md); the `tsan`
preset in your muscle memory.

**Where it plugs in.** `HnswIndex::add`, and the contract in
`ConcurrentDatabase`.

---

## 6. IVF (inverted file index)

**What it teaches.** A completely different ANN family — partition-based rather
than graph-based — and why production systems combine IVF with PQ.

**The idea.** Cluster vectors into `sqrt(N)` cells. Search only the `nprobe`
nearest cells.

**Why it is hard.** k-means at scale, and a recall/`nprobe` trade that behaves
differently from HNSW's.

**Where it plugs in.** A third `VectorIndex`. Its own file format.

---

## 7. Better filtered search

**What it teaches.** Query planning — choosing a strategy from statistics rather
than a flag.

**The gap.** Filtering is over-fetch-then-post-filter, which degrades with
selectivity, and the caller must pass `--exact` themselves.

**The goal.** Estimate selectivity from metadata statistics and choose: post-
filter for a permissive filter, pre-filter into a candidate set for a selective
one, exact scan when the filter is very selective.

**Why it is hard.** Selectivity estimation, and HNSW cannot easily search "only
these ids" without hurting connectivity.

**Where it plugs in.** `Database::search`, plus statistics in `MetadataStore`.

---

## 8. int8 or float16 storage

**What it teaches.** The precision/space trade, and hardware conversion support.

**Why it is hard.** ARM has native `float16` arithmetic; x86 mostly has
conversion instructions. Two kernel sets, a per-database element type, a format
version bump.

**Where it plugs in.** `VectorArray`'s element type, the file format, the
kernels.

---

## 9. A network API

**What it teaches.** That the engine was built so this is possible without
rewriting anything below it.

**Why it is not in v1.** It would triple the surface area — serialization,
auth, connection handling, backpressure — and halve the depth of everything else.

**Where it plugs in.** Above `ConcurrentDatabase`, which already has the right
threading model. **Nothing below it should need to change**, and that is the test
of whether the layering was real.

---

## 10. Distributed sharding

**What it teaches.** Everything about distributed systems, at once.

**Why it is hard.** Routing, rebalancing, replication, consistency, partial
failure, and a recall definition that now spans machines.

**Honest advice.** Do 1–8 first. Most people who want this actually want
quantization and a bigger machine.

---

## How to choose

- **Want to understand ANN more deeply?** 1 (PQ) or 6 (IVF).
- **Want to understand storage engines?** 3 (mmap) or 4 (WAL).
- **Want to understand concurrency?** 5, after reading the TSan output.
- **Want the most useful contribution to this repository?** 2 — one honest
  comparison number is worth more than another feature.
