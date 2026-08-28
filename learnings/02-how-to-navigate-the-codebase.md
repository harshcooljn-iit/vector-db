# 02 — How to Navigate the Codebase

🟢 Foundational

## The two-tree layout

```
include/vectordb/   PUBLIC headers — the API surface. If it is here, something
                    outside its own module is allowed to use it.
src/                Implementation, plus private headers that nothing outside
                    the module may include.
```

This split is not decoration. It is how "the query engine must not know about
the CLI" is *enforced* rather than merely requested: `src/cli/` may include
`<vectordb/db/database.hpp>`, but nothing under `src/index/` can include
anything from `src/cli/` because the CLI exports no public header at all.

When you add a type, ask: **does anything outside this directory need it?**
If no, it belongs in `src/`, not `include/`.

## "If you want to understand X, open Y"

| You want to understand… | Start at | Then read |
|---|---|---|
| the vocabulary of the whole system | `include/vectordb/core/types.hpp` | [30-vector-database/02](30-vector-database/02-vector-representations.md) |
| how errors are reported | `include/vectordb/core/error.hpp` | [90-debugging/02](90-debugging/02-common-failure-modes.md) |
| how vectors are laid out in memory | `src/core/vector_store.*` | [10-cpp-systems-foundations/03](10-cpp-systems-foundations/03-memory-layout.md) |
| distance maths | `src/distance/` | [30-vector-database/03](30-vector-database/03-distance-metrics.md) |
| SIMD | `src/distance/kernel_neon.cpp` | [70-performance/03](70-performance/03-simd-introduction.md) |
| exact search | `src/index/brute_force/` | [40-search-indexes/01](40-search-indexes/01-brute-force-index.md) |
| approximate search | `src/index/hnsw/` | [40-search-indexes/03](40-search-indexes/03-hnsw-intuition.md) |
| how bytes reach disk | `src/persistence/` | [50-storage/02](50-storage/02-binary-file-formats.md) |
| the vector file format | `src/storage/vector_store_file.*` | [50-storage/01](50-storage/01-persistent-storage.md) |
| metadata and filtering | `src/storage/metadata_store.*` | [50-storage/04](50-storage/04-sqlite.md) |
| threads | `src/concurrency/` | [60-concurrency/04](60-concurrency/04-thread-pools.md) |
| how it all composes | `src/db/database.*` | [01-project-overview.md](01-project-overview.md) |
| the command line | `src/cli/` | — |
| benchmark methodology | `benchmarks/` | [70-performance/07](70-performance/07-benchmarking.md) |

Some of those files do not exist yet — see [progress.md](progress.md) for what
has actually landed. The table is the destination, not a claim.

## Reading order for a first pass

If you want to read the code once, in an order where nothing depends on
something you have not seen:

1. `include/vectordb/core/types.hpp` — the vocabulary.
2. `include/vectordb/core/error.hpp` — how failure is expressed.
3. `src/distance/` — pure functions, no state. The easiest real code here.
4. `src/index/brute_force/` — the whole search idea, with none of the tricks.
5. `src/storage/` — where the floats actually live.
6. `src/index/hnsw/` — the hard part, and the reason for everything above it.
7. `src/db/` — composition.
8. `src/cli/` — a thin shell. Read last; it teaches you the least.

## Where the tests are

```
tests/unit/          one file per module, fast, no filesystem where avoidable
tests/integration/   real databases in a temp directory, full round trips
tests/performance/   functional performance tests, no wall-clock assertions
```

Tests are the second-best documentation in the repository (the first is the
commit history). If you want to know what a class actually guarantees, its test
file is usually a faster read than its header.

## Naming conventions

| Kind | Style | Example |
|---|---|---|
| types | `CamelCase` | `HnswIndex`, `SearchResult` |
| functions, variables | `lower_case` | `search_layer`, `entry_point` |
| private members | trailing underscore | `dimension_`, `nodes_` |
| constants | `k` + `CamelCase` | `kMaxDimension`, `kInvalidLocalId` |
| enum values | `k` + `CamelCase` | `Metric::kCosine` |
| files | `snake_case.{hpp,cpp}` | `brute_force_index.cpp` |

Enforced by `.clang-tidy`; formatting by `.clang-format`. Run
`./scripts/format.sh` before committing and neither will ever come up in review.

---

Next: [10-cpp-systems-foundations/01-value-vs-reference-semantics.md](10-cpp-systems-foundations/01-value-vs-reference-semantics.md)
