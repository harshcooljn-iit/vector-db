# Testing

327 tests across unit, integration and concurrency suites. What they check, and
the choices behind them.

```sh
ctest --preset debug          # includes -Werror
ctest --preset release
ctest --preset asan           # AddressSanitizer + UBSan
ctest --preset tsan           # ThreadSanitizer
```

## Layout

```
tests/
├── support/       shared helpers: TempDir, recall measurement
├── unit/          one file per module, fast, filesystem only where necessary
└── integration/   real databases, the real CLI binary, real threads
```

One executable, suites separated by directory and gtest suite name. Keeps link
time down and `ctest` output readable.

## What each kind checks

### Unit tests

Contracts, edge cases, and the specific ways a component can be silently wrong.

Test names are sentences, so a failure reads as a statement of what broke:

```
VectorArray.StoresVectorsBackToBackInOneContiguousBlock
TopKCollector.ResultIsIndependentOfOfferOrder
ByteReader.RefusesToReadPastTheEnd
CosineMetric.TreatsAZeroVectorAsUnrelatedRatherThanReturningNaN
HnswGraph.LayerPopulationsThinGeometricallyByRoughlyM
```

Each of those is an invariant something else depends on.

### Integration tests

Full round trips against real files: create, insert, close, reopen, search. The
persistence tests genuinely close the database and reopen it, because that is the
property being claimed.

The CLI tests drive the **real binary** through `popen`, asserting on exit codes
and on `--json` output — never on column layout. A test that breaks when a column
widens is a test people delete.

### Concurrency tests

Eight threads searching while a writer inserts, asserting that concurrent results
are identical to sequential ones and that every returned id resolves.

Run under `tsan` periodically. The two find different things.

## Choices worth explaining

### Approximate results get thresholds, structure gets exact assertions

HNSW is approximate, so demanding an exact result set would be either trivially
weak or permanently flaky. Recall is measured as **set intersection** — two
results at nearly equal distance are equally good answers — and asserted against
a threshold.

But the *graph structure* is checked exactly: every node has a layer-0 edge, no
reference dangles or self-links, no node links on layer L to a node that only
reaches L−1, layer populations thin geometrically by roughly M.

Both are necessary. Structural tests catch a broken graph; recall catches a
graph that is well-formed and useless.

### Thresholds sit just below what was measured

`AchievesHighRecallOnClusteredData` asserts `> 0.99` against a measured 1.000,
not a comfortable `> 0.95`.

Because a 0.95 threshold would have passed the query-generation bug that produced
exactly **0.752** — and the whole point of the test is to catch that class of
problem.

### Fixed seeds everywhere

Every random input is seeded. A failure is reproducible, and a recall change can
be attributed to a code change rather than to luck.

### Tests assert on error *messages*, not just types

```cpp
EXPECT_NE(message.find("768"), std::string::npos);
EXPECT_NE(message.find("384"), std::string::npos);
```

A message that omits the offending values is a defect. The error hierarchy exists
so a user can act on what they read.

### One named test per corruption mode

```
VectorFileCorruption.RejectsAFileThatIsNotOurs
VectorFileCorruption.RejectsATruncatedFile
VectorFileCorruption.RejectsAFileWithTrailingData
HnswFileCorruption.RejectsANeighbourReferencePastTheNodeCount
```

Named individually so a failure says *which* validation regressed.

### No timing assertions

Hardware-dependent thresholds make CI flaky, and a flaky test gets ignored —
losing the signal and the tests around it. Timing lives in `vectordb-bench`.

Performance *properties* are still tested: that `reserve(1000)` makes 1000
`push_back`s allocation-free, that a sliced scan equals a whole one.

## Sanitizers

**ASan** catches use-after-free, overflow and leaks at ~2× runtime. CI runs the
whole suite under it on every push.

**TSan** catches data races that did not manifest. This matters here: a race in
HNSW insertion might corrupt one neighbour list once in ten thousand runs and
surface months later as an unexplained recall drop no test reproduces.

They cannot be combined, hence two presets.

## What the tests have actually caught

Not hypothetical:

- **A SQLite prepared statement shared across threads.** Eight readers, all
  holding a shared lock, all legitimate, corrupting each other. Found by the
  stress test; the message was `bind integer failed: bad parameter or other API
  misuse`.
- **A missing `add_subdirectory(persistence)`.** Nothing failed until a clean
  configure, because the stale build directory still had the objects.
- **The query-generation bug.** Recall 0.752 on a correct index.

## Adding a test

1. Name it as a sentence describing the property.
2. Assert on the *property*, not the current output.
3. Seed any randomness.
4. For an approximate result, use a threshold — set just below what you measured.
5. If it is a bug fix, make the test fail first.

## Coverage

Not measured with a tool. What is deliberately covered:

- every public API entry point
- every error path with a distinct type
- every corruption mode of every format
- every degenerate case: empty, single element, k > N, all deleted
- every metric, every kernel, every tail length from 1 to 17

What is not: `main`'s argument dispatch beyond the CLI integration tests, and the
AVX2 kernel on real hardware (it is unreachable here, and the suite would cover it
automatically on an x86 machine).
