# Build configurations — and why Debug numbers are lies

🟢 Foundational · 🔴 consequences are advanced

## The five presets

| Preset | Flags | Use it for |
|---|---|---|
| `debug` | `-O0 -g`, assertions on, `-Werror` | writing code, debugging |
| `release` | `-O3 -DNDEBUG` | **the only build you may benchmark** |
| `relwithdebinfo` | `-O2 -g -DNDEBUG` | profiling — optimised *and* has symbols |
| `asan` | `-O0 -g -fsanitize=address,undefined` | memory bugs |
| `tsan` | `-O0 -g -fsanitize=thread` | data races |

## Why the difference is enormous, not marginal

`-O0` compiles each statement more or less literally. Every local variable
lives on the stack and is reloaded on every use; nothing inlines. Consider our
inner loop shape:

```cpp
float sum = 0.0F;
for (Dimension i = 0; i < dim; ++i) {
    const float diff = a[i] - b[i];
    sum += diff * diff;
}
```

At `-O0`: `sum` round-trips to memory each iteration, `dim` is reloaded, the
bounds arithmetic is recomputed, and nothing is vectorised.

At `-O3`: `sum` stays in a register, the loop is unrolled, and on arm64 the
compiler emits NEON instructions that do four lanes at once — often with
several independent accumulators so the FMA pipeline stays full.

The ratio is routinely **10–50×** on numeric kernels. It is not a constant
factor you can mentally correct for either, because it varies per loop. That is
why "Debug is about 10x slower" is not a usable excuse for benchmarking Debug.

There is a second, sneakier reason. `-DNDEBUG` disables `assert()`. If a hot
loop contains a bounds assertion, the Debug build is measuring your assertions.

**Rule enforced in this repo:** every number in `docs/benchmark-results.md`
records its build type, and `build_info()` reports it at runtime so a report
cannot silently be produced from the wrong build. There is a unit test that
`build_info()` names the build type — see `tests/unit/core_version_test.cpp`.

## Why `-ffast-math` is not enabled

`-ffast-math` lets the compiler pretend floating-point addition is associative.
It is not: `(a + b) + c != a + (b + c)` in IEEE-754. The compiler can then
reassociate a reduction into parallel accumulators, which is a real speedup.

We do not enable it, for two reasons:

1. Our SIMD kernels are validated against the scalar ones within a tolerance.
   If the compiler is silently reassociating the scalar version too, the test
   is comparing two moving targets and cannot tell you whether *your* SIMD code
   is correct.
2. `-ffast-math` also implies `-ffinite-math-only`, which tells the compiler NaN
   and infinity cannot occur. We have an explicit NaN/Inf rejection policy, and
   code that checks for a value the compiler has been told cannot exist is code
   the compiler is allowed to delete.

When we want multiple accumulators, we write them ourselves. Explicit beats
implicit in a kernel whose correctness is being asserted.

## Sanitizers

**AddressSanitizer** instruments every memory access and catches
use-after-free, heap and stack overflow, and leaks. Costs ~2× runtime and ~3×
memory. Run the whole suite under it periodically:

```sh
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
```

**ThreadSanitizer** catches data races — including races that did not
*manifest* on this run. This matters enormously for us: a race in HNSW
insertion might corrupt one neighbour list once every 10,000 runs and show up
as an inexplicable recall drop months later. TSan finds it deterministically.

ASan and TSan cannot be combined. They get separate presets.

## Experiments

1. Build both `debug` and `release`, run `vectordb-bench` under each, and write
   down the ratio. Then re-read the claim "Debug is 10× slower" and decide
   whether you believe it as a general rule.
2. Add `-ffast-math` to the release preset and re-run the distance tests once
   they exist. Note which tolerance starts failing.
3. Write a deliberate off-by-one in a test, run it under `debug` (probably
   passes silently) and under `asan` (immediate, with a stack trace).

## Where this lives in the code

- `CMakePresets.json` — all five configurations
- `CMakeLists.txt` — the `-O3` generator expression and the comment explaining
  the absence of `-ffast-math`
- `src/core/version.cpp` — `build_info()`, which reports the build type

---

Next: [../30-vector-database/01-what-is-a-vector-database.md](../30-vector-database/01-what-is-a-vector-database.md)
