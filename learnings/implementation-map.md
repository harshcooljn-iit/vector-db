# Implementation map

Concept → code → tests → documentation. Use this when you know *what* you want
to understand and need to find *where*.

## Core vocabulary

| | |
|---|---|
| **Concept** | Ids, metrics, errors, the ranking convention |
| **Headers** | `include/vectordb/core/{types,error,vector,search_result,top_k,logging,version}.hpp` |
| **Source** | `src/core/` |
| **Key types** | `VectorId`, `LocalId`, `Metric`, `RankKey`, `Candidate`, `SearchResult` |
| **Tests** | `tests/unit/core_*_test.cpp` |
| **Docs** | [ADR-0003](../docs/decisions/ADR-0003-id-model.md), [ADR-0009](../docs/decisions/ADR-0009-exceptions.md) |
| **Learn** | [30-vector-database/02](30-vector-database/02-vector-representations.md) |

## Vector storage in memory

| | |
|---|---|
| **Concept** | One contiguous block, stride = dimension |
| **Headers** | `include/vectordb/core/vector_array.hpp`, `storage/vector_store.hpp` |
| **Source** | `src/core/vector_array.cpp`, `src/storage/vector_store.cpp` |
| **Key functions** | `VectorArray::push_back`, `offset_of`, `VectorStore::insert/remove` |
| **Tests** | `core_vector_array_test.cpp`, `storage_vector_store_test.cpp` |
| **Docs** | [ADR-0004](../docs/decisions/ADR-0004-contiguous-storage.md), [memory-layout](../docs/memory-layout.md) |
| **Learn** | [10-cpp/03](10-cpp-systems-foundations/03-memory-layout.md) |

## Distance metrics and SIMD

| | |
|---|---|
| **Concept** | Three metrics over three swappable primitives |
| **Headers** | `include/vectordb/distance/{kernel,distance}.hpp` |
| **Source** | `src/distance/kernel_{scalar,neon,avx2}.cpp`, `kernel_registry.cpp`, `cpu_features.cpp`, `distance.cpp` |
| **Key functions** | `scalar_l2_squared`, `neon_l2_squared`, `detect_avx2`, `DistanceFunction::with_norms`, `cosine_rank_key` |
| **Tests** | `distance_kernel_test.cpp` (parameterised over every kernel), `distance_metric_test.cpp` |
| **Docs** | [distance-metrics](../docs/distance-metrics.md), [ADR-0005](../docs/decisions/ADR-0005-float32.md), [ADR-0007](../docs/decisions/ADR-0007-nan-policy.md) |
| **Learn** | [30-vector-database/03](30-vector-database/03-distance-metrics.md), [70-performance/03–05](70-performance/03-simd-introduction.md) |

## Top-k selection

| | |
|---|---|
| **Concept** | A bounded max-heap; smaller rank key is better |
| **Headers** | `include/vectordb/core/{top_k,search_result}.hpp` |
| **Source** | `src/core/top_k.cpp` |
| **Key functions** | `TopKCollector::offer`, `would_accept`, `merge_top_k`, `CandidateBetter` |
| **Tests** | `core_top_k_test.cpp` — order-independence, tie handling |
| **Learn** | [30-vector-database/04](30-vector-database/04-top-k-search.md) |

## Brute-force index

| | |
|---|---|
| **Concept** | Exact scan; the correctness oracle |
| **Headers** | `include/vectordb/index/{vector_index,vector_accessor,brute_force_index}.hpp` |
| **Source** | `src/index/brute_force/brute_force_index.cpp` |
| **Key functions** | `search`, `search_range` |
| **Tests** | `index_brute_force_test.cpp` — agreement with a full sort |
| **Docs** | [brute-force](../docs/brute-force.md), [ADR-0006](../docs/decisions/ADR-0006-brute-force-first.md) |
| **Learn** | [40-search-indexes/01](40-search-indexes/01-brute-force-index.md) |

## HNSW

| | |
|---|---|
| **Concept** | Layered proximity graph, greedy descent then wide search |
| **Headers** | `include/vectordb/index/{hnsw_config,hnsw_index,hnsw_file}.hpp` |
| **Source** | `src/index/hnsw/{hnsw_config,hnsw_index,hnsw_file}.cpp` |
| **Key functions** | `random_level`, `greedy_descend`, `search_layer`, `select_neighbours`, `link`, `add`, `VisitedSet` |
| **Tests** | `index_hnsw_test.cpp` (structure + recall), `index_hnsw_file_test.cpp` (persistence + corruption) |
| **Docs** | [hnsw](../docs/hnsw.md), [hnsw-format](../docs/hnsw-format.md), [ADR-0011](../docs/decisions/ADR-0011-tombstones.md) |
| **Learn** | [40-search-indexes/02–05](40-search-indexes/02-approximate-nearest-neighbour.md) |

## Binary formats and files

| | |
|---|---|
| **Concept** | Explicit serialization; validate before trusting |
| **Headers** | `include/vectordb/persistence/{byte_io,file_io}.hpp`, `storage/vector_file.hpp` |
| **Source** | `src/persistence/{byte_io,file_io}.cpp`, `src/storage/vector_file.cpp` |
| **Key functions** | `ByteReader::require`, `crc32`, `write_file_atomically`, `MappedFile`, `parse_vector_file_header` |
| **Tests** | `persistence_byte_io_test.cpp`, `storage_vector_file_test.cpp` |
| **Docs** | [vector-storage](../docs/vector-storage.md), [persistence](../docs/persistence.md), [ADR-0008](../docs/decisions/ADR-0008-validate-on-load.md) |
| **Learn** | [50-storage/02–03](50-storage/02-binary-file-formats.md), [10-cpp/06](10-cpp-systems-foundations/06-system-calls.md) |

## Metadata and filtering

| | |
|---|---|
| **Concept** | EAV in SQLite; a six-operator filter language |
| **Headers** | `include/vectordb/storage/{metadata,metadata_store,filter}.hpp` |
| **Source** | `src/storage/{metadata,metadata_store,filter}.cpp`, `src/storage/sqlite.{hpp,cpp}` (private) |
| **Key functions** | `MetadataStore::get/matching`, `Filter::parse`, `FilterCondition::matches` |
| **Tests** | `storage_metadata_store_test.cpp`, `storage_filter_test.cpp` |
| **Docs** | [metadata](../docs/metadata.md), [ADR-0010](../docs/decisions/ADR-0010-sqlite-for-metadata.md) |
| **Learn** | [50-storage/04](50-storage/04-sqlite.md) |

## Concurrency

| | |
|---|---|
| **Concept** | Reader-writer lock, thread pool, per-thread scratch |
| **Headers** | `include/vectordb/concurrency/thread_pool.hpp`, `db/concurrent_database.hpp` |
| **Source** | `src/concurrency/thread_pool.cpp`, `src/db/concurrent_database.cpp` |
| **Key functions** | `worker_loop`, `parallel_for`, `batch_search` |
| **Tests** | `concurrency_thread_pool_test.cpp`, `concurrent_database_test.cpp` (also run under `tsan`) |
| **Docs** | [concurrency](../docs/concurrency.md), [ADR-0013](../docs/decisions/ADR-0013-concurrency.md) |
| **Learn** | [60-concurrency/04–05](60-concurrency/04-thread-pools.md) |

## Database composition

| | |
|---|---|
| **Concept** | Three artefacts, one authoritative; recovery on open |
| **Headers** | `include/vectordb/db/database.hpp` |
| **Source** | `src/db/database.cpp` |
| **Key functions** | `create`, `open`, `flush`, `rebuild_index`, `compact`, `check`, `search` |
| **Tests** | `tests/integration/database_test.cpp`, `metadata_search_test.cpp` |
| **Docs** | [architecture](../docs/architecture.md), [persistence](../docs/persistence.md) |
| **Learn** | [80-database-engineering/](80-database-engineering/01-consistency.md) |

## CLI

| | |
|---|---|
| **Concept** | A thin shell; every behaviour lives below it |
| **Source** | `src/cli/{main,args,commands,output}.cpp` |
| **Key functions** | `reject_unknown_options`, `print_results`, the dispatch table in `main` |
| **Tests** | `tests/integration/cli_test.cpp` — drives the real binary |
| **Examples** | `examples/*.sh` |

## Benchmarks and datasets

| | |
|---|---|
| **Concept** | Deterministic data; assert before timing |
| **Headers** | `include/vectordb/util/{dataset,vector_io}.hpp` |
| **Source** | `src/util/{dataset,vector_io}.cpp`, `benchmarks/main.cpp` |
| **Key functions** | `generate_dataset`, `generate_queries`, `time_per_iteration` |
| **Tests** | `util_dataset_test.cpp` |
| **Docs** | [benchmarking](../docs/benchmarking.md), [benchmark-results](../docs/benchmark-results.md) |
| **Learn** | [70-performance/06–07](70-performance/06-profiling.md) |

## Finding things by question

| Question | Answer |
|---|---|
| Why is X the way it is? | `docs/decisions/`, or `git log --grep=X` |
| What does X guarantee? | its header comment, then its test file |
| How fast is X? | `docs/benchmark-results.md`, or run `vectordb-bench` |
| What breaks if I change X? | `grep -r X tests/` |
| How did X evolve? | `git log --oneline -- <path>` |
