# Common failure modes

🟡 Intermediate · a symptom-to-cause index

## Build

**`find_package(spdlog)` fails**
`VCPKG_ROOT` is unset, or the preset was not used. `export VCPKG_ROOT=~/vcpkg`
and `cmake --preset debug`.

**Undefined reference to something that obviously exists**
Its module is missing an `add_subdirectory` line in `src/CMakeLists.txt` — or the
build directory is stale and still linking old objects. This happened here: the
persistence module was dropped from the source list and nothing failed until a
clean configure.

```sh
rm -rf build/debug && cmake --preset debug && cmake --build --preset debug
```

**`-Werror` on `-Wconversion` in new code**
Usually a real narrowing bug. Add an explicit `static_cast` only once you have
convinced yourself the value fits.

**Vexing parse: `VectorArray(kMaxDimension)` declares a variable**
Use braces: `VectorArray{kMaxDimension}`.

**`nodiscard` warning inside `EXPECT_THROW`**
Wrap it: `EXPECT_THROW(static_cast<void>(f()), E)`.

## Runtime errors

**`vector dimension mismatch: database dimension = 768, input dimension = 384`**
Exactly what it says. VectorDB never truncates or pads
([ADR-0002](../../docs/decisions/ADR-0002-fixed-dimension.md)).

**`vector rejected: component 5 is NaN`**
A bug upstream — a division by a zero norm, an overflow, a corrupt input.
NaN is refused at the door because it makes distance comparisons non-transitive
([ADR-0007](../../docs/decisions/ADR-0007-nan-policy.md)).

**`vector id 42 already exists (duplicate policy = reject)`**
Use `upsert` if replacement was intended.

**`hnsw index was built for metric 'cosine' but this database uses 'l2'`**
Run `vectordb rebuild-index`.

**`cannot parse filter at position 12`**
The message includes a caret. Note there is no `or`, and values need quoting to
be treated as text.

## Wrong answers

**Search returns nothing**
The database may be empty (`vectordb info`), everything may be tombstoned
(`stats`), or a filter may match nothing — remember a missing key never matches,
including for `!=`.

**Fewer than k results with a filter**
The post-filter ran out of candidates. Use `--exact`, or raise `--oversample`.
This is documented behaviour, not a bug — see
[`docs/metadata.md`](../../docs/metadata.md).

**HNSW disagrees with `--exact`**
Expected in small measure. Raise `--ef-search`; if it does not converge toward
the exact answer, the graph is broken — run the `HnswGraph.*` tests.

**Recall is terrible (< 0.5)**
**Check your data first.** Uniform random vectors in high dimensions are
pathological, and queries drawn from a different distribution than the data are
worse. This exact mistake produced recall 0.752 on a correct index here.

**Results changed after a reopen**
They should not — a loaded index is byte-identical. If they did, something is
non-deterministic; check whether the index was silently rebuilt (`--verbose`).

**Stale or nonsense distances after mutating the store**
The index's `VectorAccessor` was not refreshed. `Database` calls `set_accessor`
after every mutation; direct users of the index layer must too.

## Crashes

**Segfault in the search loop**
A dangling `VectorView` held across a reallocation, or a stale accessor. Run
under `asan`.

**`std::terminate` with no message**
An exception escaped a destructor, or a thread's entry function. Check that every
`packaged_task` boundary is intact.

**SIGBUS reading a mapped file**
The file was truncated while mapped. This is the accepted cost of `mmap` and
cannot portably become an exception.

**SIGILL on x86**
An AVX2 kernel ran on a CPU without it — meaning the runtime detection was
bypassed. It should be impossible; if it happens, `cpu_features.cpp` is wrong.

## Concurrency

**`bind integer failed: bad parameter or other API misuse`**
A SQLite statement used from two threads. This happened here: `MetadataStore` now
synchronises itself because prepared statements are stateful and a `const` read
is not read-only.

**A hang at exit**
`notify_one` where `notify_all` was needed — every worker must wake to see the
shutdown flag.

**A hang inside `parallel_for`**
Called from inside a task on the same pool. Every worker blocks waiting for work
only a worker can run. Documented, not guarded.

**Results differ between runs, only under threads**
Run under `tsan`. A race that does not manifest is still a race.

## Performance

**Slower than expected**
Check the build: `vectordb version` prints the build type. A Debug build is
10–50× slower on numeric loops, and not by a constant factor.

**SIMD did not help**
Expected for brute force — the scan is memory-bandwidth-bound, and SIMD speeds
up computation, not waiting. See
[70-performance/03](../70-performance/03-simd-introduction.md).

**Throughput falls as threads are added**
Something on the search path is serialising. Profile — do not guess. Here it was
a metadata lookup two layers away.

**Import is slow**
Check that `reserve` was called. Without it, growth reallocates and copies
logarithmically often.

## Files

**`check` passes but results are wrong**
Run `check --deep`. Payload checksums are not verified on open, by design.

**`is not a VectorDB database (no metadata.sqlite)`**
Wrong path, or the directory was created by something else.

**`.tmp` files left behind**
A crash during a write. They are harmless; the previous file is intact. Delete
them.

## When none of this helps

1. Write a **minimal failing test** in `tests/unit/`. Half the time this finds it.
2. `git bisect`. Commits are atomic and buildable, which is what makes it work.
3. `git log -S "<identifier>"` — every commit that touched it, with reasoning.
4. Check the ADRs. The behaviour may be a decision rather than a bug.

## Where this lives in the code

- `include/vectordb/core/error.hpp` — the error hierarchy
- `src/db/database.cpp` — `check`
- [01-how-to-debug-this-project.md](01-how-to-debug-this-project.md)
- `docs/decisions/` — when the behaviour is deliberate
