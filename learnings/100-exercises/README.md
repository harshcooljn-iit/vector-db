# Exercises

Hands-on changes to this codebase. Each has a goal, something to change,
something to measure, and questions that are **deliberately not answered** —
they are reachable by running the code, which is the point.

Work in a branch:

```sh
git switch -c experiment/whatever
# ... break things ...
git switch - && git branch -D experiment/whatever
```

## Before you start

```sh
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset release && cmake --build --preset release
ctest --preset release                       # 327 tests should pass
./build/release/bin/vectordb-bench            # your baseline numbers
```

**Write your baseline down.** Every exercise below is a comparison, and "it felt
faster" is not a result.

## The exercises

| # | Exercise | Level | Teaches |
|---|---|---|---|
| 1 | [Memory layout](01-memory-layout.md) | 🟡 | why contiguous storage matters, measured |
| 2 | [Brute force vs HNSW](02-brute-force-vs-hnsw.md) | 🟢 | where the crossover actually is |
| 3 | [The efSearch curve](03-efsearch-recall-curve.md) | 🟡 | the recall/latency dial |
| 4 | [Tuning M](04-tuning-m.md) | 🟡 | memory against quality |
| 5 | [Break the heuristic](05-break-the-heuristic.md) | 🔴 | why neighbour diversity matters |
| 6 | [SIMD vs scalar](06-simd-vs-scalar.md) | 🔴 | arithmetic vs memory bound |
| 7 | [Thread scaling](07-thread-scaling.md) | 🟡 | where parallelism stops helping |
| 8 | [Delete and rebuild](08-deletion-and-rebuild.md) | 🟡 | what tombstones cost |
| 9 | [Corrupt a file](09-corrupt-a-file.md) | 🟡 | validation, and what it catches |
| 10 | [Bad benchmark data](10-bad-benchmark-data.md) | 🔴 | the curse of dimensionality, felt |

## Mini challenges

Open-ended, no instructions. Each is a real contribution.

| Challenge | Where to start |
|---|---|
| **Add a distance metric** (Manhattan / L1) | `Metric` in `core/types.hpp`, then the kernels, then `distance.cpp`. Note what the ordering convention demands. |
| **Add `vectordb count`** with a filter | `src/cli/commands.cpp`, plus `Database::filter_ids` |
| **Add a `key exists` filter operator** | `FilterOp`, the parser, the evaluator, and think hard about the missing-key rule |
| **Add float16 storage** | `VectorArray`, the file format version, a conversion in the kernels. The largest of these. |
| **Make `search_range` use the thread pool** | `BruteForceIndex` already has the sliced form; `merge_top_k` already merges |
| **Add a `--csv` option to `export`** | Already there — instead, add streaming import so a file larger than RAM can be loaded |
| **Per-page checksums** | `docs/vector-storage.md` names this as the gap in validate-on-load |
| **An hnswlib backend, purely as a benchmark baseline** | The most useful missing number in `benchmark-results.md` |

## How to answer an exercise well

1. **Predict before you measure.** Write the number down. Being wrong is the
   informative outcome.
2. **Change one thing.**
3. **Measure the same way each time**, same data, same build.
4. **Explain the mechanism**, not just the direction. "Slower because more cache
   misses" is an answer; "slower" is not.
5. **Check whether the tests still pass.** An optimisation that breaks a test is
   a bug with better timing.
