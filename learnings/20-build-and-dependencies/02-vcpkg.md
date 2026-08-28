# vcpkg — dependencies that are the same on every machine

🟢 Foundational

## The problem

C++ has no `pip install`. Historically you got a dependency by:

- installing a system package (`brew install spdlog`) — different version on
  every developer's machine, and none at all in CI;
- vendoring the source into your repo — now you own their build;
- `git submodule` + `add_subdirectory` — works, but their CMake options leak
  into yours and build times multiply.

The failure mode is always the same: **it builds on my machine.** Someone has
spdlog 1.12, CI has 1.10, and a header-only inline function behaves differently.

## What vcpkg does

vcpkg builds dependencies from source, for your exact compiler and flags, and
installs them into a directory it owns. In **manifest mode** (what we use) the
dependency list lives in the repo:

```json
{
  "name": "vectordb",
  "builtin-baseline": "ca18538bf88b72c41056b866695de13966ed6889",
  "dependencies": ["nlohmann-json", "spdlog", "sqlite3"],
  "features": {
    "tests": { "dependencies": ["gtest"] }
  }
}
```

Then a CMake toolchain file hooks the configure step: `find_package(spdlog
CONFIG REQUIRED)` resolves against vcpkg's tree instead of your system.

```sh
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset release      # vcpkg installs anything missing, then configures
```

Output lands in `build/<preset>/vcpkg_installed/` — inside the build directory,
which is gitignored. Delete the build directory and you delete the dependencies
with it.

## The three parts that matter

### `builtin-baseline`

A commit hash of the vcpkg registry. It pins *which versions of everything*
resolve. Without it, "spdlog" means "whatever spdlog is today", and a clean
checkout six months from now builds different code than yours did.

This is the single most important line in the manifest. It is what makes
`docs/benchmark-results.md` reproducible.

### Features

```json
"features": { "tests": { "dependencies": ["gtest"] } }
```

Optional dependency groups. Someone consuming VectorDB as a library should not
compile GoogleTest. Our presets opt in with
`"VCPKG_MANIFEST_FEATURES": "tests"`.

### Triplets

A triplet names the target ABI: `arm64-osx`, `x64-linux`, `x64-windows`. vcpkg
picks one from your compiler; it appears in every installed path. Two triplets
can coexist, which is how cross-compiling stays sane.

## Why not header-only everything?

Two of our four dependencies *are* header-only-capable (nlohmann/json always,
spdlog optionally). vcpkg's spdlog defaults to a compiled library
(`SPDLOG_COMPILED_LIB`), and that is the better choice here: spdlog pulls in a
lot of `fmt`, and compiling it once instead of in every translation unit is a
large build-time win. You can see the define on our compile lines.

SQLite is a single enormous `.c` file — genuinely worth building once.

## Things that will bite you

- **`VCPKG_ROOT` unset.** The preset references `$env{VCPKG_ROOT}`; without it
  the toolchain path is nonsense and `find_package` fails with a message that
  does not mention vcpkg at all. Export it in your shell profile.
- **First configure is slow.** vcpkg compiles from source. Ten minutes is
  normal; it is cached afterwards, including across build directories.
- **Target names rarely match package names.** `sqlite3` the package exports
  `unofficial::sqlite3::sqlite3`. vcpkg prints the correct incantation after
  installing — read that output rather than guessing.
- **Do not commit `vcpkg_installed/`.** It is hundreds of megabytes of
  machine-specific binaries. Our `.gitignore` covers it.

## Experiments

1. Delete `"builtin-baseline"` and reconfigure from a clean build dir. It still
   works — which is the danger. Now argue why you would not ship that.
2. Remove `"VCPKG_MANIFEST_FEATURES": "tests"` from the release preset and
   reconfigure. Watch `find_package(GTest)` fail, and notice the failure is at
   *configure* time rather than at link time. Earlier failure is better failure.
3. Run `./vcpkg list` in your vcpkg root, then look inside
   `build/release/vcpkg_installed/arm64-osx/`. Find the actual `libsqlite3.a`
   you are linking.

## Where this lives in the code

- `vcpkg.json` — the manifest
- `CMakePresets.json` — `CMAKE_TOOLCHAIN_FILE` and `VCPKG_MANIFEST_FEATURES`
- `CMakeLists.txt` — the three `find_package` calls

---

Next: [03-build-configurations.md](03-build-configurations.md)
