# How to debug this project

🟡 Intermediate · practical

## First, reproduce it

```sh
cmake --preset debug && cmake --build --preset debug
./build/debug/bin/vectordb_tests --gtest_filter='*Hnsw*'
```

Everything random here is **seeded**, so a failure is reproducible. If it is not,
that itself is the bug — something is depending on iteration order, timing, or
uninitialised memory.

Turn on logging:

```sh
./build/debug/bin/vectordb search mydb --query "..." --verbose
./build/debug/bin/vectordb search mydb --query "..." --log-level debug
```

## Pick the right tool for the symptom

| Symptom | Tool |
|---|---|
| Wrong answer | a failing test, then a debugger |
| Crash | `asan` |
| Crash only sometimes | `asan`, then `tsan` |
| Wrong answer only under threads | `tsan` |
| Hang | `lldb`, then `thread backtrace all` |
| Too slow | `relwithdebinfo` + a profiler — [see below](#slow) |
| Corrupt file | `xxd`, and `vectordb check --deep` |
| It works in debug, not release | uninitialised memory, or UB. `asan`, then `-fsanitize=undefined` |

That last row deserves emphasis: "works in debug, fails in release" almost always
means undefined behaviour that the optimiser is now exploiting.

## Sanitizers

```sh
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
```

**ASan** catches use-after-free, buffer overflow, and leaks — with a stack trace
for both the bad access and the original allocation. ~2× slower.

**TSan** catches data races *that did not manifest*. It instruments memory
accesses and tracks happens-before, so it reports a race the first time the two
accesses occur, even if the timing never actually interleaved badly.

They cannot be combined.

## Debugging a wrong search result

Work down the stack. Each step eliminates a layer:

1. **Is the vector stored correctly?**
   `vectordb get mydb <id> --json` — compare against what you inserted.
2. **Is the distance right?**
   Compute it by hand for two small vectors and compare with
   `compute_rank_key`.
3. **Is brute force right?**
   `vectordb search mydb --query ... --exact`. If exact is wrong, the bug is in
   distance, top-k or the store — not in HNSW.
4. **Is HNSW's answer merely approximate?**
   Compare with `--exact`. A small difference is expected; raise `--ef-search`
   and watch it converge. If it does not converge, the graph is broken.
5. **Is the graph well-formed?**
   Run the structural tests: `--gtest_filter='HnswGraph.*'`.

**The oracle is the debugging tool.** Brute force exists so that "is this right?"
is answerable, and it is the first thing to reach for.

## Debugging a corrupt file

```sh
vectordb check mydb --deep
xxd mydb/vectors.bin | head -5
xxd -s 64 -l 32 mydb/vectors.bin        # the first eight floats
```

Compare the header against [`docs/vector-storage.md`](../../docs/vector-storage.md).
The magic is readable ASCII precisely so a hex dump is useful at 2 a.m.

Error messages name the artefact, the invariant, and the values — start by
reading it carefully rather than skipping to the code.

## Debugging a hang

```sh
lldb -p <pid>
(lldb) thread backtrace all
```

For a *hang*, this is strictly better than a profiler: it shows exactly what
every thread is blocked on.

Common causes here: `parallel_for` called from inside a pool task (documented,
not guarded); a mutex held across something that blocks; `notify_one` where
`notify_all` was needed at shutdown.

## <a name="slow"></a>Debugging slowness

```sh
cmake --preset relwithdebinfo && cmake --build --preset relwithdebinfo
sample <pid> 5 -mayDie                                  # macOS
perf record -g ./build/relwithdebinfo/bin/vectordb-bench   # Linux
```

**Profile before you theorise.** This project spent four rounds of careful
reasoning on a scaling collapse — allocator contention, cache-line ping-pong,
virtual dispatch, `validate_vector` — each tested with an isolating benchmark and
each wrong. A four-second `sample` found it immediately: 89% of the time was in
`MetadataStore::get`, two layers away.

The story is in [70-performance/06](../70-performance/06-profiling.md).

## Using the history

The commit history is a debugging tool:

```sh
git log --oneline --reverse            # the project in build order
git log --grep="cluster structure"     # why a decision was made
git log -S "select_neighbours"         # every commit touching that identifier
git bisect start && git bisect bad && git bisect good <sha>
```

Commit messages here record measurements and reasoning, so `git log --grep` often
answers "why is this like this?" faster than reading the code.

`git bisect` works because commits are atomic and each leaves the tree buildable.
That is what the discipline was for.

## Adding a temporary probe

```cpp
#include <spdlog/spdlog.h>
spdlog::debug("search_layer: layer={} ef={} visited={}", layer, ef, visited_count);
```

Then run with `--log-level debug`. Remove it before committing — the logging
policy is that hot loops do not log.

## Common mistakes when debugging *this* codebase

**Forgetting `set_accessor`.** Mutating the store invalidates the index's view.
Symptom: stale or nonsense distances.

**Comparing HNSW against HNSW.** Always compare against brute force.

**Benchmarking a debug build.** The driver warns, loudly.

**Trusting a stale build directory.** A missing `add_subdirectory` once went
unnoticed for exactly this reason. When something is inexplicable:
`rm -rf build/debug`.

**Assuming a recall drop is an index bug.** Check the data first —
[Exercise 10](../100-exercises/10-bad-benchmark-data.md) exists because that
assumption cost real time here.

## Where this lives in the code

- `CMakePresets.json` — the sanitizer presets
- `include/vectordb/core/logging.hpp` — levels; the library defaults to `warn`
- `src/db/database.cpp` — `check`
- [02-common-failure-modes.md](02-common-failure-modes.md)

---

Next: [02-common-failure-modes.md](02-common-failure-modes.md)
