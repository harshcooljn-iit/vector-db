# Glossary

One line each, with a pointer to where the idea is developed properly.
Terms are added as the project introduces them.

| Term | Meaning |
|---|---|
| **ABI** | The binary contract for a compiled type: sizes, alignment, padding, calling convention. Why you cannot `fwrite` a struct and read it back elsewhere. → [50-storage/02](50-storage/02-binary-file-formats.md) |
| **Alignment** | The requirement that an object's address be a multiple of some number. Misaligned SIMD loads are slow or illegal. → [70-performance/03](70-performance/03-simd-introduction.md) |
| **ANN** | Approximate Nearest Neighbour. Trades a small chance of a wrong answer for a large speedup. → [40-search-indexes/02](40-search-indexes/02-approximate-nearest-neighbour.md) |
| **Atomic (C++)** | An operation other threads cannot observe half-finished. Not a substitute for a lock. → [60-concurrency/06](60-concurrency/06-atomics.md) |
| **Atomic replacement (files)** | Write to a temp file, fsync, then `rename()` over the target. Readers see the old or new file, never a half-written one. → [80-database-engineering/02](80-database-engineering/02-crash-safety.md) |
| **Candidate set** | The frontier of nodes HNSW still intends to explore. A min-heap by distance. → [40-search-indexes/04](40-search-indexes/04-hnsw-algorithm.md) |
| **Cache line** | The unit the CPU actually moves between RAM and cache, 64 or 128 bytes. Touching 4 bytes costs a whole line. → [70-performance/01](70-performance/01-cache-locality.md) |
| **Contention** | Threads queueing on the same lock. Shows up as cores idling while throughput flatlines. → [60-concurrency/05](60-concurrency/05-reader-writer-concurrency.md) |
| **Dimension** | Number of components per vector. Fixed per database in VectorDB. |
| **efConstruction** | HNSW build-time candidate-list width. Higher = better graph, slower build. → [40-search-indexes/04](40-search-indexes/04-hnsw-algorithm.md) |
| **efSearch** | HNSW query-time candidate-list width. The recall/latency dial you turn at runtime. |
| **Embedding** | A model's numeric representation of an item, where geometric closeness approximates semantic similarity. → [30-vector-database/01](30-vector-database/01-what-is-a-vector-database.md) |
| **HNSW** | Hierarchical Navigable Small World: a layered proximity graph searched greedily from the top. → [40-search-indexes/03](40-search-indexes/03-hnsw-intuition.md) |
| **Index** | A structure that answers queries faster than scanning. Here: brute force (exact) or HNSW (approximate). |
| **Latency** | Time for one operation. Report p50/p95/p99, never only the mean. → [70-performance/07](70-performance/07-benchmarking.md) |
| **LocalId** | Dense internal slot `[0, count)`; a direct index into contiguous storage. 32-bit to keep HNSW edges small. |
| **M** | HNSW's max neighbours per node per layer. The main memory/quality dial. |
| **Memory mapping (mmap)** | Making a file appear as addressable memory; the OS pages it in on demand. → [50-storage/03](50-storage/03-memory-mapped-files.md) |
| **Metadata** | Non-vector attributes attached to a vector. Stored in SQLite here. → [50-storage/04](50-storage/04-sqlite.md) |
| **NEON** | ARM's SIMD instruction set. What Apple Silicon uses. → [70-performance/04](70-performance/04-arm-neon.md) |
| **AVX2** | x86's 256-bit SIMD instruction set. Must be feature-detected at runtime. → [70-performance/05](70-performance/05-avx2.md) |
| **Page fault** | The trap when you touch a mapped page that is not resident; the OS fetches it. → [50-storage/03](50-storage/03-memory-mapped-files.md) |
| **Recall@k** | Fraction of the true top-k that an approximate search actually returned. The correctness measure for ANN. → [40-search-indexes/02](40-search-indexes/02-approximate-nearest-neighbour.md) |
| **Serialization** | Converting in-memory values to an explicit, documented byte layout. Field by field — never a struct dump. → [50-storage/02](50-storage/02-binary-file-formats.md) |
| **SIMD** | Single Instruction, Multiple Data: one instruction operating on several values at once. → [70-performance/03](70-performance/03-simd-introduction.md) |
| **Thread pool** | A fixed set of worker threads consuming a task queue, so threads are created once rather than per task. → [60-concurrency/04](60-concurrency/04-thread-pools.md) |
| **Throughput** | Operations per second in aggregate. Can improve while latency worsens. |
| **Tombstone** | A "deleted" marker left in place instead of physically removing data. → [80-database-engineering/05](80-database-engineering/05-tombstones.md) |
| **Top-k** | The k best results, maintained with a bounded heap rather than by sorting everything. → [30-vector-database/04](30-vector-database/04-top-k-search.md) |
| **VectorId** | Stable, user-facing 64-bit id. Never reused after deletion. |
| **WAL** | Write-Ahead Log: record intent durably before mutating, so recovery can replay or roll back. → [80-database-engineering/01](80-database-engineering/01-consistency.md) |
