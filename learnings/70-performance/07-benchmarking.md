# Benchmarking honestly

🟡 Intermediate · pairs with `benchmarks/main.cpp`

## The number that was three orders of magnitude wrong

The first run of this project's batch benchmark reported:

```
threads   queries/s
1         13159267
```

**13 million queries per second.** Each query searches 50,000 vectors of
dimension 128 — that would be 76 nanoseconds to do work that takes ~20
microseconds.

Nothing failed. The benchmark ran, formatted a table, and printed a number that
was wrong by a factor of about 600 **in the flattering direction**.

The cause: the setup inserted 50,000 vectors and never called `flush()` before
reopening the directory. Every reopened handle read the empty database that
`create()` had written. It was searching nothing, very quickly.

> **A benchmark with no correctness assertion measures whatever it happens to be
> doing — and a broken configuration is usually much faster than a working one.**

The fix:

```cpp
if (concurrent.size() != spec.count) { /* fail loudly */ }
const auto probe = concurrent.batch_search(queries, options);
if (probe.empty() || probe.front().size() != 10) { /* fail loudly */ }
```

If you take one thing from this page, take that.

## The rules this project follows

1. **Never report a number that was not measured.** No estimates, no
   extrapolation. A missing figure is documented as missing.
2. **Release builds only.** The driver prints a loud warning otherwise, and
   `build_info()` records the build type so a report is traceable.
3. **Record provenance** — compiler, build type, architecture, kernel, thread
   count. A timing without the build that produced it is not a measurement.
4. **Deterministic data.** Fixed seeds, a fully specified PRNG, so the same spec
   gives byte-identical data on any machine.
5. **Assert correctness before timing.**
6. **Percentiles, not just means.** p50 and p95. A mean hides the tail users
   actually feel.

## Traps

### Dead code elimination

```cpp
for (int i = 0; i < N; ++i) distance(a, b);       // result unused
```

The compiler proves the results are unused and deletes the loop. You benchmark
an empty loop and report a spectacular number.

```cpp
volatile float sink = 0.0F;
float total = 0.0F;
for (int i = 0; i < N; ++i) total += kernel->l2_squared(a, b, dimension);
sink = total;                                     // now it must happen
```

### Timing the clock

`Clock::now()` costs tens of nanoseconds. Timing a 10 ns kernel call one at a
time measures the clock. Batch 1000 calls per timed region and divide.

### No warm-up

The first iteration pays for cold caches, lazily-faulted pages and one-time
initialisation. Run the body once before starting the timer.

### Fixed iteration counts

Too few on a slow configuration is noise; too many on a fast one wastes minutes.
Run for a fixed *duration* and let the iteration count fall out.

### Benchmarking the wrong thing

The distance-kernel benchmark reuses two cache-resident vectors and reports
99–126 GB/s — which exceeds this machine's DRAM bandwidth by 3×. That is not an
error; it measures **arithmetic throughput**.

The brute-force benchmark streams a whole dataset and plateaus at 38–48 GB/s,
which is the real memory bandwidth.

Both are correct and they answer different questions. Reporting either alone as
"the speed of VectorDB" would mislead, so both appear with an explanation.

### Unrepresentative data

An earlier version of the dataset generator drew queries from a *different*
cluster structure than the data. Every query landed in the empty space between
clusters, where a query's 10th nearest neighbour was 3% further than its 1st —
the concentration regime where there is barely a nearest neighbour to find.

HNSW measured recall@10 = 0.752 at parameters that should be near-exact. The
index was fine; the benchmark was measuring tie-breaking among near-equidistant
points.

**Uniform random data in high dimensions is a pathological case, not a neutral
one.** If your ANN benchmark shows terrible recall, suspect the data first.

### Comparing across machines

Different CPU, different memory, different compiler, different thermal state.
Our numbers say "Apple M1, 8 GB, AppleClang 17, Release" precisely so nobody
compares them against a Xeon and draws a conclusion.

## Reporting

For each result: date, CPU, memory, OS, compiler, build type, kernel, dataset
parameters (N, D, kind, seed), query count, k, and index configuration.

And the negative space. `docs/benchmark-results.md` has a **Not measured**
section listing AVX2, N = 1,000,000, filtered-search latency, and the comparison
against FAISS or hnswlib that would be the single most useful number here.

An absent number is more honest than an estimated one, and a section that says
so is more useful than silence.

## What we deliberately do not do

**No performance assertions in CI.** Hardware-dependent thresholds make CI
flaky, and a flaky test gets ignored — which loses you the signal *and* the
tests around it. CI checks functional properties (recall exceeds a threshold, a
sliced scan equals a whole one) and leaves timing to the benchmark driver.

**No CPU pinning or frequency locking.** This is a laptop and the numbers say so.
Run-to-run variance is a few percent, which is stated rather than hidden by
running until a lucky number appears.

## Experiments

1. Remove the `volatile` sink from the distance benchmark and re-run. Note the
   impossible number, then look at the assembly to confirm the loop is gone.
2. Delete the warm-up call and run the fastest benchmark repeatedly. Measure the
   variance you introduced.
3. Run the whole suite three times and record the spread. That is your noise
   floor, and any "improvement" smaller than it is not one.
4. Run the suite in a `debug` build and compare with `release`. Compute the
   ratio per benchmark — it is not a constant, which is why "Debug is about 10×
   slower" is not a usable correction.
5. Add a benchmark for filtered search at 1%, 10% and 50% selectivity. Then
   decide whether a single "filtered search latency" number could ever be
   meaningful.

## Where this lives in the code

- `benchmarks/main.cpp` — the driver, `time_per_iteration`, the assertions
- `docs/benchmarking.md` — the methodology
- `docs/benchmark-results.md` — measured numbers, and what is missing
- `src/util/dataset.cpp` — deterministic generation, and the query-distribution
  fix

---

Next: [../80-database-engineering/01-consistency.md](../80-database-engineering/01-consistency.md)
