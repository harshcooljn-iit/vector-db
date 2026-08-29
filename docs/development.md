# Development

Getting set up, and the everyday loop.

## Requirements

| | |
|---|---|
| Compiler | C++20 — AppleClang 15+, Clang 15+, GCC 12+ |
| CMake | 3.24+ |
| Ninja | any recent |
| vcpkg | any recent |
| Python | 3.8+ (helper scripts only) |

Tested on macOS (arm64) and Linux (x86-64). Windows is not tested and is not
claimed.

## Setup

```sh
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=~/vcpkg          # put this in your shell profile

cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

The first configure compiles spdlog, fmt, SQLite and GoogleTest from source —
several minutes. Cached afterwards, including across build directories.

## The loop

```sh
cmake --build --preset debug && ctest --preset debug
```

Or:

```sh
./scripts/build-and-test.sh debug
```

Run one test:

```sh
./build/debug/bin/vectordb_tests --gtest_filter='Hnsw*'
./build/debug/bin/vectordb_tests --gtest_filter='*Recall*' --gtest_brief=1
```

## Presets

| Preset | For |
|---|---|
| `debug` | development — assertions, `-Werror` |
| `release` | **the only build whose timings mean anything** |
| `relwithdebinfo` | profiling — optimised, with symbols |
| `asan` | AddressSanitizer + UBSan |
| `tsan` | ThreadSanitizer |

Each writes to its own `build/<preset>/`, so switching never triggers a rebuild.

## Editor setup

```sh
ln -sf build/debug/compile_commands.json .
```

clangd and clang-tidy then work with no further configuration.

## Formatting

```sh
./scripts/format.sh            # format in place
./scripts/format.sh --check    # what CI runs
```

Run it before committing and formatting never comes up in review. The script
hunts for `clang-format` in the places macOS hides it rather than assuming PATH.

## Adding a source file

1. Create it in the right module under `src/`.
2. Add it to that module's `CMakeLists.txt` — sources are listed explicitly, not
   globbed, so adding a file touches exactly one line.
3. Public header in `include/vectordb/<module>/`, private header next to the
   `.cpp`. The `include`/`src` split is what enforces the layering.
4. Add a test file to `tests/unit/` and to its `CMakeLists.txt`.

### If it does not link

Check that the module has an `add_subdirectory` line in `src/CMakeLists.txt` —
and then do a **clean** configure. A missing subdirectory once went unnoticed
because the stale build directory still held the object files:

```sh
rm -rf build/debug && cmake --preset debug && cmake --build --preset debug
```

## Adding a CLI command

1. Declare it in `src/cli/commands.hpp`.
2. Implement it in `src/cli/commands.cpp` — it must call
   `reject_unknown_options(args)`, or a mistyped flag would be silently ignored.
3. Register it in `main`'s dispatch table.
4. Add it to `print_usage`.
5. Add an integration test to `tests/integration/cli_test.cpp` asserting on exit
   codes and `--json`, never on layout.

Commands must contain **no engine logic** — everything lives behind
`vectordb::Database`, so anything the CLI can do is also testable from C++.

## Debugging

```sh
lldb ./build/debug/bin/vectordb_tests -- --gtest_filter='Hnsw*'
gdb --args ./build/debug/bin/vectordb_tests --gtest_filter='Hnsw*'
```

For memory bugs, `asan`. For races, `tsan`. For "why is this slow",
[`benchmarking.md`](benchmarking.md#profiling) — and profile before you theorise.

Verbose logging:

```sh
./build/debug/bin/vectordb search mydb --query "..." --verbose
./build/debug/bin/vectordb search mydb --query "..." --log-level debug
```

## Commits

Conventional Commits: `feat:`, `fix:`, `perf:`, `docs:`, `test:`, `build:`,
`chore:`, `style:`, with an optional scope (`feat(index):`).

Before each:

```sh
git status && git diff
./scripts/format.sh --check
cmake --build --preset debug && ctest --preset debug
```

**The message should explain *why*.** The diff already says what. When a commit
records a measurement or a bug, put the numbers in — several commits here are the
best documentation of a decision, and `git log --grep` is a first-class way to
find things.

Keep commits atomic and buildable. A commit that leaves the tree broken makes
`git bisect` useless exactly when you need it.

## CI

Every push runs: `{ubuntu, macos} × {debug, release}` with a CLI smoke test, a
clang-format check, and the full suite under AddressSanitizer.

Reproduce a CI failure locally with the same preset.

## Before opening a pull request

```sh
rm -rf build
cmake --preset release && cmake --build --preset release && ctest --preset release
cmake --preset debug   && cmake --build --preset debug   && ctest --preset debug
./scripts/format.sh --check
./examples/01-quickstart.sh
```

If you touched anything on a hot path, run `vectordb-bench` before and after and
put the numbers in the commit message.
