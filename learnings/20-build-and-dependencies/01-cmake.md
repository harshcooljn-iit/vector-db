# CMake — enough to actually own the build

🟢 Foundational · prerequisite for everything else

You have probably used CMake as a magic incantation. Here is the model that
makes it stop being magic.

## 1. CMake is not a build system

CMake is a **build system generator**. It reads `CMakeLists.txt` and writes
build files for something else — here, Ninja.

```
CMakeLists.txt --[cmake configure]--> build.ninja --[ninja]--> binaries
                    "configure step"                "build step"
```

Two consequences that explain most CMake confusion:

- Changing a `CMakeLists.txt` requires re-running configure. Ninja does this
  automatically, which is why the first build after an edit is slower.
- Anything you write in `CMakeLists.txt` runs at *configure* time, on your
  machine, in CMake's own language — not at compile time.

## 2. Targets, not variables

Old CMake set global variables (`CMAKE_CXX_FLAGS`, `include_directories()`).
Those leak: a flag needed by one library ends up on every file in the project.

Modern CMake attaches everything to a **target** and gives each property a
visibility:

| Keyword | Applies to the target | Propagates to consumers |
|---|---|---|
| `PRIVATE` | ✅ | ❌ |
| `PUBLIC` | ✅ | ✅ |
| `INTERFACE` | ❌ | ✅ |

Look at our library:

```cmake
target_include_directories(vectordb
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
```

`include/` is PUBLIC — anyone linking `vectordb` gets it, because that is the
API. `src/` is PRIVATE — our own `.cpp` files can include private headers, but
a consumer cannot reach into our internals. **The visibility keyword is the
encapsulation boundary.** That is the whole idea.

Same story for libraries:

```cmake
target_link_libraries(vectordb
    PUBLIC  spdlog::spdlog                     # appears in our public headers
    PRIVATE unofficial::sqlite3::sqlite3       # an implementation detail
            nlohmann_json::nlohmann_json)
```

SQLite is PRIVATE, and that is the build system *enforcing* the architectural
rule "HNSW must not know about SQLite". Nothing that links `vectordb` even sees
SQLite's headers.

### The `INTERFACE` trick we use for warnings

```cmake
add_library(vectordb_warnings INTERFACE)
target_compile_options(vectordb_warnings INTERFACE -Wall -Wextra ...)
```

A library with no sources whose only job is to carry flags. Every target *we*
own links it; vcpkg's headers never see it. Without this, `-Wconversion` would
fire thousands of times inside third-party headers and you would turn it off —
losing exactly the warning that catches silent narrowing in a file format.

## 3. Why `target_sources()` and not `file(GLOB)`

Globbing is tempting:

```cmake
file(GLOB SOURCES "src/*.cpp")   # DON'T
```

Two failures. First, the glob is evaluated at *configure* time — add a file and
the build does not notice until you reconfigure. Second, a stray file (an
editor backup, a half-finished experiment) silently joins your library.

We instead list sources from each module's own `CMakeLists.txt`:

```cmake
# src/core/CMakeLists.txt
target_sources(vectordb PRIVATE error.cpp types.cpp version.cpp)
```

Adding a file touches exactly one line in one module. As a bonus, the diff of
"which files does this library contain" is reviewable.

## 4. Generator expressions

`$<...>` is evaluated at *generate* time, when the build type is finally known:

```cmake
$<$<CONFIG:Release>:-O3>            # only in Release
$<$<CXX_COMPILER_ID:MSVC>:/W4>      # only under MSVC
```

You need these because one configure can generate multiple configurations. A
plain `if(CMAKE_BUILD_TYPE STREQUAL "Release")` is evaluated once, too early.

## 5. Presets

`CMakePresets.json` names configurations so nobody has to remember a
twelve-flag command line:

```sh
cmake --preset release            # configure
cmake --build --preset release    # build
ctest --preset release            # test
```

Our presets: `debug` (assertions on, `-Werror`), `release` (`-O3`,
**the only one you may benchmark**), `relwithdebinfo` (optimised with symbols,
for profiling), `asan` and `tsan` (sanitizers).

Each writes to its own `build/<preset>/` directory, so switching build types
never triggers a full rebuild.

## Things that will bite you

- **`CMAKE_CXX_STANDARD 20` is a request, not a guarantee** unless you also set
  `CMAKE_CXX_STANDARD_REQUIRED ON`. Without it, CMake will happily fall back to
  an older standard and you get a wall of confusing errors instead of one clear
  one. We set both, plus `CMAKE_CXX_EXTENSIONS OFF` for portable `-std=c++20`
  rather than `-std=gnu++20`.
- **Stale cache.** A change to a toolchain file or preset sometimes needs
  `rm -rf build/<preset>`. If an error mentions a path you deleted a week ago,
  that is the cause.
- **`compile_commands.json`.** `CMAKE_EXPORT_COMPILE_COMMANDS ON` makes clangd
  and clang-tidy work. Symlink it to the repo root for your editor:
  `ln -sf build/debug/compile_commands.json .`

## Experiments

1. Change `PRIVATE` to `PUBLIC` on the SQLite link. Now `#include <sqlite3.h>`
   from `tests/unit/core_types_test.cpp`. It compiles. That is the leak the
   keyword was preventing — put it back.
2. Add a `.cpp` file to `src/core/` without touching `CMakeLists.txt`. Confirm
   it is not compiled. Now imagine that file was a half-finished rewrite.
3. Configure with `-DVECTORDB_WERROR=ON` on the release preset and add an
   unused variable. Read what the warning target did for you.

## Where this lives in the code

- `CMakeLists.txt` — targets, options, warning interface, dependency wiring
- `CMakePresets.json` — the five build configurations
- `src/*/CMakeLists.txt` — per-module `target_sources()`
- `tests/CMakeLists.txt` — GoogleTest wiring and `gtest_discover_tests`

---

Next: [02-vcpkg.md](02-vcpkg.md)
