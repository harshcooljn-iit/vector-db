# Benchmarking methodology

The rules this project follows so that its numbers mean something. Results:
[benchmark-results.md](benchmark-results.md). Driver: `benchmarks/main.cpp`.

## The rules

1. **Never report a number that was not measured.** No estimates, no
   extrapolation, no "roughly 2× based on the instruction count". A missing
   figure is documented as missing.
2. **Release builds only.** The driver prints a loud warning under `NDEBUG`-less
   builds, and `build_info()` records the build type so a report can always be
   traced back.
3. **Record the provenance.** Every run prints compiler, build type,
   architecture, active kernel and thread count. A timing without the build that
   produced it is not a measurement.
4. **Deterministic data.** Fixed seeds and a fully specified PRNG, so the same
   spec produces byte-identical data on any platform.
5. **Assert correctness before timing.** See below — this is not optional.
6. **Report percentiles, not just the mean.** p50 and p95. A mean hides the tail
   that users actually notice.

## Assert before you time

The batch benchmark once reported **13 million queries per second**. It was
searching an empty database: the setup inserted 50,000 vectors and never flushed
before reopening the directory.

Nothing failed. The benchmark ran, produced a beautifully formatted table, and
was wrong by three orders of magnitude in the flattering direction.

```cpp
if (concurrent.size() != spec.count) { /* fail loudly */ }
const auto probe = concurrent.batch_search(queries, options);
if (probe.empty() || probe.front().size() != 10) { /* fail loudly */ }
```

**A benchmark with no correctness assertion measures whatever it happens to be
doing**, and a broken configuration is usually much faster than a working one.
This is the single most valuable rule on the page.

## Warm-up

The first iteration pays for cold caches, lazily-faulted pages and any one-time
initialisation. None of that is what we are measuring, so `time_per_iteration`
runs the body once before starting the clock.

## Fixed duration, not fixed iterations

```cpp
body();                                  // warm up
const auto start = Clock::now();
while (seconds_since(start) < min_seconds) { body(); ++iterations; }
return seconds_since(start) / iterations;
```

A fixed iteration count either wastes time on a fast configuration or produces
noise on a slow one. A fixed duration adapts, and the iteration count becomes an
output rather than a guess.

## Defeating the optimiser

```cpp
volatile float sink = 0.0F;
float total = 0.0F;
for (int i = 0; i < kBatch; ++i) total += kernel->l2_squared(a, b, dimension);
sink = total;
```

Without the `volatile` sink, the compiler can prove the results are unused and
delete the entire loop. You then measure an empty loop and report a spectacular
number.

The batching (1000 calls per timed iteration) is separate: it amortises the
clock read, which is itself tens of nanoseconds and would otherwise dominate a
10 ns kernel call.

## Cache-resident versus streaming

The distance-kernel benchmark reuses two vectors, so both stay in L1. Its GB/s
figures exceed DRAM bandwidth by 3×, and that is not a mistake — it measures
**arithmetic throughput**.

The brute-force benchmark streams a whole dataset and plateaus at 38–48 GB/s,
which is the machine's actual memory bandwidth.

Both are useful and they answer different questions. Reporting either as "the"
speed of the system would be misleading, so both appear with an explanation of
what each measures.

## Recall

Measured against brute force on identical queries, as set intersection:

```
recall@k = |returned ∩ true top-k| / k
```

Not position-by-position agreement — two results at nearly equal distance are
equally good answers, and demanding an exact ordering from an approximate
algorithm produces a metric that punishes the wrong thing.

### Query distribution is part of the methodology

Queries are drawn from the **same cluster structure** as the data, with a
different sample seed.

This is not a detail. An earlier generator drew queries from a different
structure, putting every query in the empty space between clusters where
everything is roughly equidistant. HNSW measured recall@10 = 0.752 at parameters
that should be near-exact — the index was fine, the benchmark was measuring
tie-breaking.

**Uniform random data in high dimensions is a pathological case**, not a neutral
one. `DatasetKind::kUniform` exists as an honest worst case; `kClustered` is the
default because it is the closest cheap approximation to real embeddings.

## What we do not do

- **No CPU pinning, no disabled frequency scaling, no isolated cores.** This is a
  laptop, and the numbers are honest about being laptop numbers. Run-to-run
  variance is a few percent.
- **No comparison against FAISS or hnswlib.** It would be the most useful number
  here, and its absence is recorded in
  [benchmark-results.md](benchmark-results.md#not-measured) rather than papered
  over.
- **No performance assertions in CI.** Hardware-dependent thresholds make CI
  flaky, and a flaky test gets ignored. The test suite checks *functional*
  properties — that recall exceeds a threshold, that a sliced scan equals a whole
  one — and leaves timing to the benchmark driver.

## Profiling

macOS:

```sh
cmake --preset relwithdebinfo && cmake --build --preset relwithdebinfo
# Sample a running process:
sample <pid> 5 -mayDie
# Or the full instrument:
xctrace record --template 'Time Profiler' --launch ./build/relwithdebinfo/bin/vectordb-bench
```

Linux:

```sh
perf record -g ./build/relwithdebinfo/bin/vectordb-bench
perf report
perf stat -e cache-misses,cache-references,instructions,cycles ./build/relwithdebinfo/bin/vectordb-bench
```

Use `relwithdebinfo`, not `debug`: you want optimised code *with* symbols.
Profiling a debug build tells you about the debug build.

`sample` is what found the metadata bottleneck described in
[benchmark-results.md](benchmark-results.md#the-bug-this-benchmark-found) — after
four rounds of reasoning about allocator contention, cache-line ping-pong and
virtual dispatch had all pointed at the wrong thing. **Profile before you
theorise.**

## Adding a benchmark

1. Add a function to `benchmarks/main.cpp`.
2. Assert correctness before timing.
3. Register it in `main`'s `wants(...)` dispatch so it can be run alone.
4. Run it, and paste the **actual output** into `benchmark-results.md` with the
   machine description.
5. If the number surprises you, profile before you explain it.
