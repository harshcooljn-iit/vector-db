# Progress

What is **actually implemented**. Nothing is ticked here on the strength of a
plan.

Last updated: v1 complete. 327 tests passing; measured results in
[`docs/benchmark-results.md`](../docs/benchmark-results.md).

## Foundations

- [x] Repository structure
- [x] CMake project with Ninja
- [x] vcpkg manifest with a pinned baseline
- [x] Build configurations (debug / release / relwithdebinfo / asan / tsan)
- [x] GoogleTest harness wired to CTest
- [x] Error hierarchy
- [x] Id model (`VectorId` / `LocalId`) and metric vocabulary
- [x] clang-format / clang-tidy configuration and scripts
- [x] CI: linux + macos, debug + release, format, links, AddressSanitizer

## Vector model and math

- [x] Contiguous vector storage
- [x] Vector validation (dimension, NaN/Inf)
- [x] Scalar distance kernels (L2², cosine, inner product)
- [x] Normalization
- [x] Top-k selection with a deterministic total order
- [x] SIMD kernels (NEON) — measured 3.6–4.1× over scalar
- [x] SIMD kernels (AVX2) — implemented and guarded, **never run on x86 hardware**
- [x] Runtime CPU dispatch, including the XCR0 check

## Indexes

- [x] Index interface
- [x] Brute-force index (the oracle)
- [x] HNSW graph construction
- [x] HNSW search
- [x] Neighbour selection heuristic with re-pruning
- [x] Recall cross-check against brute force — 0.999 at efSearch 10

## Storage and persistence

- [x] Binary serialization primitives with bounds-checked reads
- [x] Vector file format (versioned, CRC, nine validation steps)
- [x] Memory-mapped reads on the load path
- [ ] Searching directly over a live mapping without materialising the store
- [x] SQLite metadata store
- [x] Database configuration persistence
- [x] HNSW index persistence with full reference validation
- [x] Atomic file replacement (incl. directory fsync and `F_FULLFSYNC`)
- [x] Corruption detection and `vectordb check --deep`

## Database engineering

- [x] Database lifecycle (create / open / close / flush)
- [x] Insert, upsert, get, delete
- [x] Tombstones
- [x] Index rebuild, including automatic recovery on open
- [x] Compaction
- [x] Crash-safety reasoning documented and its limits stated
- [ ] Write-ahead log

## Concurrency

- [x] Reader/writer contract documented and enforced
- [x] Concurrent search — measured 4.19× on 8 cores
- [x] Thread pool
- [x] Batch search
- [x] Clean under ThreadSanitizer
- [ ] Concurrent writers (explicitly not supported)

## Query features

- [x] Top-k search API
- [x] Metadata filter language and evaluator
- [x] Filter integrated with search, with the post-filter limitation documented
- [x] `--exact` escape hatch for selective filters

## CLI

- [x] `version`, `help`
- [x] `create`, `info`, `stats`, `check`
- [x] `insert`, `get`, `delete`
- [x] `search`, `batch-search`
- [x] `import`, `export`, `generate`
- [x] `rebuild-index`, `compact`
- [x] `--json` machine-readable output
- [x] Unknown options rejected rather than ignored

## Benchmarks

- [x] Deterministic dataset generator
- [x] Distance kernel benchmark
- [x] Brute-force latency
- [x] HNSW latency and recall
- [x] Insertion / build throughput
- [x] Batch search throughput vs thread count
- [x] Index build and load time
- [x] Measured results in `docs/benchmark-results.md`
- [ ] N = 1,000,000 (8 GB machine; would measure swap at D=768)
- [ ] Comparison against FAISS or hnswlib

## Documentation

- [x] `README.md`
- [x] `docs/architecture.md`
- [x] `docs/distance-metrics.md`, `brute-force.md`, `hnsw.md`
- [x] `docs/vector-storage.md`, `hnsw-format.md`, `data-formats.md`
- [x] `docs/persistence.md`, `metadata.md`, `concurrency.md`, `memory-layout.md`
- [x] `docs/benchmarking.md`, `benchmark-results.md`
- [x] `docs/testing.md`, `development.md`
- [x] `docs/final-report.md`
- [x] 13 architecture decision records
- [x] 62 learning notes across 10 sections
- [x] 10 exercises and 8 mini challenges
- [x] 7 runnable examples
- [x] Final capstone and roadmap
