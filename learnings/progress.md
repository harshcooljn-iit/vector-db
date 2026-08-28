# Progress

What is **actually implemented**, updated when a milestone genuinely lands.
Nothing is ticked here on the strength of a plan.

Last updated: phase 2 complete.

## Foundations

- [x] Repository structure
- [x] CMake project with Ninja
- [x] vcpkg manifest with pinned baseline
- [x] Build configurations (debug / release / relwithdebinfo / asan / tsan)
- [x] GoogleTest harness wired to CTest
- [x] Error hierarchy
- [x] Id model (`VectorId` / `LocalId`) and metric vocabulary
- [x] clang-format / clang-tidy scripts
- [x] CI (linux + macos, debug + release, format, asan)

## Vector model and math

- [x] Contiguous vector storage (in memory)
- [x] Vector validation (dimension, NaN/Inf)
- [ ] Scalar distance kernels (L2², cosine, inner product)
- [x] Normalization
- [ ] Top-k selection
- [ ] SIMD kernels (NEON)
- [ ] SIMD kernels (AVX2)
- [ ] Runtime CPU dispatch

## Indexes

- [ ] Index interface
- [ ] Brute-force index
- [ ] HNSW graph construction
- [ ] HNSW search
- [ ] Neighbour selection / pruning
- [ ] Recall cross-check against brute force

## Storage and persistence

- [ ] Binary serialization primitives
- [ ] Vector file format (versioned)
- [ ] Memory-mapped reads
- [ ] SQLite metadata store
- [ ] Database configuration persistence
- [ ] HNSW index persistence
- [ ] Atomic file replacement
- [ ] Corruption detection / `vectordb check`

## Database engineering

- [ ] Database lifecycle (create / open / close)
- [ ] Insert, get, delete
- [ ] Tombstones
- [ ] Index rebuild
- [ ] Compaction
- [ ] Crash-safety reasoning documented

## Concurrency

- [ ] Reader/writer contract documented
- [ ] Concurrent search
- [ ] Thread pool
- [ ] Batch search

## Query features

- [ ] Top-k search API
- [ ] Metadata filtering
- [ ] Filter + ANN semantics documented

## CLI

- [x] `version`, `help`
- [ ] `create`, `info`, `stats`
- [ ] `insert`, `get`, `delete`
- [ ] `search`, `batch-search`
- [ ] `rebuild-index`, `compact`, `check`
- [ ] `--json` machine-readable output

## Benchmarks

- [ ] Deterministic dataset generator
- [ ] Distance kernel benchmark
- [ ] Brute-force latency
- [ ] HNSW latency
- [ ] HNSW recall vs efSearch
- [ ] Insertion throughput
- [ ] Batch search throughput
- [ ] Index build time
- [ ] Load time
- [ ] Measured results recorded in `docs/benchmark-results.md`

## Documentation

- [x] `learnings/00-start-here.md`
- [x] `learnings/01-project-overview.md`
- [x] `learnings/02-how-to-navigate-the-codebase.md`
- [x] `learnings/20-build-and-dependencies/` (CMake, vcpkg, build configs)
- [x] `learnings/10-cpp-systems-foundations/03-memory-layout.md`
- [x] `learnings/30-vector-database/01`, `02` (what/why, representations)
- [x] `README.md`
- [ ] `docs/architecture.md`
- [ ] remaining `docs/` pages
- [x] ADRs 0001-0009 (decisions made so far)
- [ ] Exercises
- [ ] Final report and capstone
