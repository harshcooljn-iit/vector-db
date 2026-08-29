# Exercise 1 — Memory layout

🟡 Intermediate · [ADR-0004](../../docs/decisions/ADR-0004-contiguous-storage.md), [memory-layout note](../10-cpp-systems-foundations/03-memory-layout.md)

## Goal

Turn "contiguous storage is faster" from a claim you were told into a number you
measured.

## Background

`VectorArray` stores N vectors in **one** allocation, vector `i` at
`data() + i * dimension`. The obvious alternative is
`std::vector<std::vector<float>>`.

The claimed reasons: allocation count, per-object overhead, and — the one said
to dominate — cache locality and prefetching.

## Part A: measure the layouts

Write a standalone program that sums every float in 100,000 vectors of dimension
768, once with each layout. Same data, same arithmetic.

```cpp
// A
std::vector<std::vector<float>> scattered;
// B
std::vector<float> contiguous;   // N * D
```

Build with `-O3 -DNDEBUG`.

**Predict the ratio before you run it.** Write it down.

Then measure both time and peak RSS (`/usr/bin/time -l` on macOS, `-v` on Linux).

## Part B: isolate the prefetcher

Using the contiguous layout only, visit slots in a random permutation instead of
in order.

Same data, same total bytes, same arithmetic — only the *order* changes.

**Questions.** How much of A-versus-B was allocation overhead, and how much was
locality? Does this measurement change your answer?

## Part C: find your cache

Time the contiguous scan at N = 100, 1,000, 10,000, 100,000, 1,000,000 with
D = 768. Plot **nanoseconds per vector** against total working-set size.

The curve should be flat and then step up.

**Questions.** Where are the steps? Compare with your machine's L1/L2/L3 sizes
(`sysctl hw.l1dcachesize hw.l2cachesize` on macOS, `lscpu` on Linux). You have
just measured your cache hierarchy with a stopwatch.

## Part D: the dangling view

```cpp
VectorView v = array[5];
array.push_back(something);   // may reallocate
float x = v[0];               // ?
```

Run it under `release`, then under the `asan` preset.

**Questions.** What happened in each? Which failure would you rather debug? What
does this tell you about when to run the sanitizers?

## Part E: was padding the right call?

ADR-0004 rejects padding the row stride to a SIMD boundary, on the grounds that
it would fork the memory layout from the file layout and forfeit direct mmap.

Add padding to `VectorArray` (round the stride up to a multiple of 16 floats,
zero-filled). Note that the kernels can then drop their tail handling entirely,
since `(0-0)² = 0`.

Measure at D = 100 (12% waste) and D = 768 (no waste).

**Questions.** Does the tail removal pay for the wasted bandwidth? At which
dimensions? What would you now have to change in `vector_file.cpp`, and would
`read_vector_file` still be able to use the mapped floats in place? Would you
reverse the ADR?

## Where to look

- `include/vectordb/core/vector_array.hpp` — the class and its reasoning
- `src/index/brute_force/brute_force_index.cpp` — the scan
- `tests/unit/core_vector_array_test.cpp` — `StoresVectorsBackToBackInOneContiguousBlock`
