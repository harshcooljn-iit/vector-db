# Profiling — and why you should do it before theorising

🟡 Intermediate · practical

## A story from this repository

The batch-search benchmark showed throughput *falling* as threads were added:
1.79× at 2 threads, 1.16× at 4, 0.90× at 8. Meanwhile `HnswIndex::search` called
directly from raw threads scaled 4.3×.

So something in the `Database` wrapper was destroying it. Four hypotheses, each
plausible, each tested with an isolating benchmark:

| Hypothesis | Test | Result |
|---|---|---|
| `shared_mutex` cache-line contention | raw search + shared_lock per query | scales 4.31× — **not it** |
| Allocator contention on results | raw search + building a result vector | scales 4.25× — **not it** |
| `validate_vector` on the query | timed in isolation | 123 ns — **not it** |
| `ThreadPool::parallel_for` overhead | own threads instead of the pool | same collapse — **not it** |

Four rounds of careful reasoning, four wrong answers.

Then:

```sh
sample <pid> 4
```

Four seconds, and the answer was immediate:

```
2705 vectordb::Database::search(...)
 2401   vectordb::MetadataStore::get(...) + 64      ← 89%, at the mutex acquire
  177   vectordb::MetadataStore::get(...) + 160
          173 sqlite3_step
```

`Database::search` was fetching metadata for every result — including for a
database that had none and a caller that never read it. Each lookup took a mutex
and ran a SQLite statement, with WAL's `fcntl` locking underneath.

Fixing it took single-thread throughput from 28,357 to 51,368 queries/s and
restored scaling from 0.90× to 4.19×.

> **Profile before you theorise.** The bottleneck was two layers away from the
> code being optimised, and no amount of reasoning about the code being
> optimised was going to find it.

## The tools

### macOS

```sh
# Sample a running process — the quickest useful thing you can do
sample <pid> 5 -mayDie

# Full time profiler
xctrace record --template 'Time Profiler' --launch ./build/relwithdebinfo/bin/vectordb-bench

# Where the memory went
leaks --atExit -- ./build/release/bin/vectordb-bench
```

`sample` is underrated. It needs no setup, attaches to a running process, and
produces a call tree with sample counts. It found the bug above.

### Linux

```sh
perf record -g ./build/relwithdebinfo/bin/vectordb-bench
perf report

# Hardware counters — where the *real* answers are for memory-bound code
perf stat -e cache-misses,cache-references,instructions,cycles,stalled-cycles-backend \
    ./build/relwithdebinfo/bin/vectordb-bench

# Which lines cause cache-line contention between cores
perf c2c record ./build/relwithdebinfo/bin/vectordb-bench
```

### Both

```sh
valgrind --tool=cachegrind ./build/relwithdebinfo/bin/vectordb_tests   # slow, exact
```

## Use `relwithdebinfo`

```sh
cmake --preset relwithdebinfo && cmake --build --preset relwithdebinfo
```

**Not `debug`** — profiling a debug build tells you about the debug build, where
nothing is inlined and every local round-trips to memory. The hot spots are
different.

**Not plain `release`** either — without symbols the profile is a list of
addresses. `relwithdebinfo` is `-O2 -g`: real optimisation, real symbols.

## Reading a profile

**Self time vs total time.** Total includes callees. A function with high total
and low self time is a *router*, not a bottleneck — look at its children.

**Inlining scrambles attribution.** `-O2` inlines aggressively, so time may be
attributed to the caller. If a hot function seems to have vanished, it was
inlined into its caller.

**Look for what should not be there at all.** The most valuable finding is
usually not "this function is 5% slower than expected" but "why is SQLite on the
search path?"

That is the shape of most real performance bugs: not a slow function, but a fast
function called in a place nobody intended.

## Counters, for memory-bound code

A time profile tells you *where*. Hardware counters tell you *why*.

| Counter | Says |
|---|---|
| `cache-misses` / `cache-references` | miss rate — the memory story |
| `stalled-cycles-backend` | cycles waiting, usually for memory |
| `instructions` / `cycles` | IPC; below ~1 usually means stalling |

For VectorDB's brute-force scan, IPC is low and backend stalls are high — the
loop is waiting on DRAM. That is measurable evidence for the claim made
throughout these notes, rather than an assertion.

## Poor man's profiling

No tools, or a hang rather than a slowdown? Attach a debugger and interrupt it a
few times:

```sh
lldb -p <pid>
(lldb) thread backtrace all
(lldb) continue
```

If the same stack appears three times out of five, that is where the time goes.
Crude, and often enough. For a *hang* it is strictly better than a sampler,
because it shows exactly what every thread is blocked on.

## Method

1. **Measure first.** Establish a baseline number you can compare against.
2. **Profile, do not guess.** See the story at the top of this page.
3. **Form one hypothesis** and an experiment that would falsify it.
4. **Change one thing.**
5. **Measure again**, the same way, on the same data.
6. **Write down what changed and by how much.** "It felt faster" is not a result.

Step 5 catches the case where your optimisation made things worse, which happens
more often than anyone admits.

## Experiments

1. Run `vectordb-bench brute-force` under `perf stat` (or Instruments' Counters
   template). Compute IPC and the cache miss rate. Explain the GB/s plateau.
2. Profile `vectordb-bench hnsw`. Where does time go — distance computation, or
   pointer chasing through the graph? Predict first.
3. Deliberately reintroduce the metadata lookup on the search path, then find it
   with a profiler without reading this page.
4. Profile a `debug` build and a `relwithdebinfo` build of the same benchmark and
   compare the hot lists. Note how the ranking changes.
5. Use `perf c2c` (Linux) on the concurrent benchmark to find which cache lines
   bounce between cores.

## Where this lives in the code

- `docs/benchmarking.md` — the methodology, including this story
- `docs/benchmark-results.md` — the measured before/after
- `benchmarks/main.cpp` — the driver, and its correctness assertions

---

Next: [07-benchmarking.md](07-benchmarking.md)
