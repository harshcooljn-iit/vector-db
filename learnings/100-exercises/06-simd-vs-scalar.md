# Exercise 6 — SIMD vs scalar

🔴 Advanced · [SIMD intro](../70-performance/03-simd-introduction.md)

## Goal

Measure where SIMD helps, where it does not, and be able to explain the
difference.

## Background

Measured on an M1: NEON is **3.6–4.1×** faster than scalar on the distance
kernel. And the brute-force scan, which is nothing but that kernel in a loop,
barely improves.

Both facts are true. Understanding why is the exercise.

## Part A: the kernel

```sh
./build/release/bin/vectordb-bench distance
```

**Questions.** Is your speedup near 4×? Why is D = 128 slightly worse than
D = 384? What fixed cost is being amortised?

## Part B: the scan

```sh
./build/release/bin/vectordb-bench brute-force
```

Then rebuild with SIMD off and compare:

```sh
cmake --preset release -DVECTORDB_ENABLE_SIMD=OFF
cmake --build --preset release
./build/release/bin/vectordb-bench brute-force
```

**Predict the ratio first.** Then measure.

**Questions.** How much did the 4× kernel speedup become at the whole-query
level? Look at the GB/s column — what is it plateauing at, and what is that
number physically?

## Part C: name the bound

Compute, for D = 768, N = 100,000:

- bytes read per query
- floating-point operations per query
- your machine's memory bandwidth (`sysctl hw.memsize` will not tell you; a
  simple `memcpy` benchmark will)
- your machine's FLOP/s (from Part A's kernel numbers)

**Questions.** Which resource runs out first? By what margin? Now explain the
Part B result in one sentence.

## Part D: where SIMD *does* help

```sh
./build/release/bin/vectordb-bench hnsw     # with and without SIMD
```

HNSW touches a few thousand vectors, many repeatedly, so more of its working set
is cache-resident.

**Questions.** Is the SIMD benefit larger here than for brute force? Why? Does
that match the claim that SIMD helps computation and not waiting?

## Part E: accumulators

In `kernel_neon.cpp`, use one accumulator instead of four (`kBlock = 4`).

**Questions.** How much do you lose? That gap is the *pipeline* effect, separate
from the lane effect. Now do the same to the scalar kernel and compare the two
gaps.

## Part F: what the compiler did anyway

```sh
clang++ -O3 -std=c++20 -Rpass=loop-vectorize -c src/distance/kernel_scalar.cpp \
    -I include -I build/release/vcpkg_installed/arm64-osx/include
```

**Questions.** Did clang vectorise the scalar kernel by itself? If so, what are
your hand-written intrinsics actually buying? Would you still write them?

## Part G: fast-math

Add `-ffast-math` to the release build and re-run the distance benchmark and the
kernel tests.

**Questions.** How much faster is the scalar kernel? Which test tolerance starts
failing, and why? Read the reasoning in
[03-build-configurations](../20-build-and-dependencies/03-build-configurations.md)
— do you agree with the decision?

## Where to look

- `src/distance/kernel_scalar.cpp`, `kernel_neon.cpp`
- `docs/benchmark-results.md` — sections 1 and 2, and the contrast between them
- `benchmarks/main.cpp` — note that the kernel benchmark is cache-resident by
  construction and the scan is not
