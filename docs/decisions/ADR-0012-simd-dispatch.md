# ADR-0012 — A kernel table selected at runtime

**Status:** Accepted

## Decision

Distance arithmetic lives behind a small table of function pointers. One table
per instruction set; the fastest one the CPU can actually execute is selected
once, at runtime.

```cpp
struct DistanceKernel {
    BinaryReduceFn l2_squared;
    BinaryReduceFn dot;
    UnaryReduceFn  norm_squared;
    std::string_view name;
};
```

## Context

The inner loop of the whole system is a reduction over two float arrays. It is
worth optimising with SIMD, and SIMD is architecture-specific: NEON on ARM, AVX2
on x86-64, and neither on a target we have not thought about.

A binary must also run on a machine older than the one that compiled it.

## Options considered

### 1. Scalar only

Portable, simple, and gives up a measured 3.6–4.1×.

### 2. Compile-time selection

```cmake
target_compile_options(vectordb PRIVATE -mavx2)
```

The binary then requires AVX2 to *start*. Worse, it compiles the runtime check
itself with AVX2, so it would crash before it could decide anything. And
shipping per-architecture binaries pushes the problem onto packaging.

### 3. A virtual `DistanceKernel` interface

Works. Costs the same indirect call as a function pointer, plus a vtable
pointer, an object lifetime, and a class hierarchy to swap in a test.

### 4. Templates on the kernel type

Zero call overhead — and the kernel must be chosen from CPU features detected at
*runtime*. You end up with a runtime switch selecting between template
instantiations, which is a function-pointer table with more steps and much more
code.

### 5. A table of function pointers, selected at runtime (chosen)

## Chosen approach

Option 5. Kernels are registered in increasing order of preference, each guarded
twice: a compile-time check that the code is in this binary at all, and a runtime
check that this CPU can execute it.

AVX2 functions use `__attribute__((target("avx2,fma")))`, so only those functions
are compiled with AVX2 while the dispatch stays at baseline.

## Why

- **One indirect call per candidate is affordable given what it wraps.** At
  D = 768, `l2_squared` performs 768 multiply-adds and streams 6 KB; a few
  nanoseconds of dispatch is under 1%. (At D = 4 the ratio inverts, which is a
  real limitation and is stated as one.)
- **Adding an instruction set is adding a table**, not editing any caller. NEON
  was added without touching a single line of index or database code.
- **A kernel is swappable in a test with an assignment.** The test suite is
  parameterised over `available_kernels()`, so every kernel runs the same
  suite — every tail length from 1 to 17, a double-precision reference, and
  equality with scalar.
- **`VECTORDB_ENABLE_SIMD=OFF`** gives a scalar-only binary for comparison and
  for bisecting.

## Trade-offs

| Cost | Benefit |
|---|---|
| An indirect call per candidate | portable binary, no per-arch packaging |
| Blocks inlining of the kernel | the kernel is large; inlining would not help |
| Poor ratio at very small dimensions | irrelevant for embedding-sized vectors |
| Runtime detection code to maintain | a binary that starts on any supported CPU |

## Consequences

- **The scalar kernel is the oracle.** It is simple enough to be obviously
  correct, so an optimised kernel that disagrees is wrong — the same relationship
  brute force has with HNSW.
- **A kernel is never offered unless its instructions are supported.** For AVX2
  that means checking OSXSAVE, AVX, FMA, XCR0 *and* the AVX2 bit. The XCR0 check
  is the one people skip: a CPU can support AVX2 while the OS does not preserve
  YMM registers across a context switch.
- **The active kernel's name appears in `vectordb version` and in every benchmark
  result.** A timing without the kernel that produced it is not a measurement.
- Detection is cached in a function-local static: `cpuid` is a serialising
  instruction, so calling it per query would be a self-inflicted pipeline stall.

## Alternatives for future versions

- **A batched kernel API** (`l2_squared_many`) so one indirect call covers many
  candidates, removing dispatch from the per-candidate path entirely. The obvious
  next step if small dimensions ever matter.
- **AVX-512**, with the caveat that heavy use downclocks many Intel parts and can
  make the system slower overall.
- **ARM SVE**, once Apple cores expose it.
