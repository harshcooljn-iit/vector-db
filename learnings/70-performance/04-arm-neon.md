# ARM NEON, concretely

🔴 Advanced · pairs with `src/distance/kernel_neon.cpp`

Read [03-simd-introduction](03-simd-introduction.md) first. This note is the
practical detail for the instruction set Apple Silicon actually runs.

## No runtime detection needed

NEON (Advanced SIMD) is **mandatory** in the arm64 base instruction set. If you
are on arm64, you have it.

```cpp
bool cpu_supports_neon() noexcept {
#if VECTORDB_ARCH_ARM64
    return true;      // nothing to detect
#endif
}
```

Contrast with AVX2, where a missing runtime check is a SIGILL on someone else's
machine. 32-bit ARM did make NEON optional, which is why the function exists at
all rather than being assumed.

## The types

```cpp
float32x4_t   // four floats  — what we use
float64x2_t   // two doubles
int32x4_t     // four int32
uint8x16_t    // sixteen bytes
```

The naming is regular: `<type><lanes>x<count>_t`.

## The instructions we use

| Intrinsic | Does |
|---|---|
| `vld1q_f32(ptr)` | load 4 floats |
| `vdupq_n_f32(0)` | broadcast a scalar to 4 lanes |
| `vsubq_f32(a, b)` | 4 subtractions |
| `vfmaq_f32(acc, a, b)` | `acc + a*b`, fused, 4 lanes |
| `vaddq_f32(a, b)` | 4 additions |
| `vaddvq_f32(v)` | sum the 4 lanes to one float |

The `q` means "quadword" — the 128-bit form. Without it you get the 64-bit
version with half the lanes.

`vaddvq_f32` is **arm64 only**. On 32-bit ARM you need a pairwise-add sequence.
Since the file only compiles on arm64, the single instruction is available.

## The kernel, annotated

```cpp
float neon_l2_squared(const float* a, const float* b, std::size_t count) {
    float32x4_t sum0 = vdupq_n_f32(0.0F);   // 4 accumulators
    float32x4_t sum1 = vdupq_n_f32(0.0F);   // × 4 lanes each
    float32x4_t sum2 = vdupq_n_f32(0.0F);   // = 16 floats in flight
    float32x4_t sum3 = vdupq_n_f32(0.0F);

    std::size_t i = 0;
    for (; i + 16 <= count; i += 16) {
        const float32x4_t d0 = vsubq_f32(vld1q_f32(a + i + 0), vld1q_f32(b + i + 0));
        const float32x4_t d1 = vsubq_f32(vld1q_f32(a + i + 4), vld1q_f32(b + i + 4));
        const float32x4_t d2 = vsubq_f32(vld1q_f32(a + i + 8), vld1q_f32(b + i + 8));
        const float32x4_t d3 = vsubq_f32(vld1q_f32(a + i + 12), vld1q_f32(b + i + 12));

        sum0 = vfmaq_f32(sum0, d0, d0);     // 4 independent chains
        sum1 = vfmaq_f32(sum1, d1, d1);
        sum2 = vfmaq_f32(sum2, d2, d2);
        sum3 = vfmaq_f32(sum3, d3, d3);
    }

    for (; i + 4 <= count; i += 4) { ... }  // whole vectors, no full block
    float total = vaddvq_f32(...);          // reduce once
    for (; i < count; ++i) { ... }          // scalar tail
    return total;
}
```

**16 floats per iteration.** At `D = 768` that is 48 iterations instead of 768.

**Why 16 and not 4?** Four independent FMA chains. The M1's FPU has ~4 cycles of
FMA latency and can issue one per cycle, so one chain would leave ~75% of the
pipeline idle. Four fills it. Eight would too, and would start to pressure the
register file for no further gain.

## The M1, specifically

Apple's cores are unusually wide, which changes the arithmetic:

- **Four 128-bit NEON units**, so up to four FMAs *per cycle*.
- ~3.2 GHz, so a theoretical ~100 GFLOP/s of FP32 FMA per core.
- 128-byte cache lines (twice the usual 64) — one line holds 32 floats.

Our measured 4.1× is against an already well-optimised scalar kernel. The
theoretical ceiling is higher, and we do not reach it because the loop is
issue-limited on loads rather than on arithmetic — two loads per FMA.

**This is why the 128-byte cache line matters more than it looks.** A row of 128
floats is 512 bytes = exactly 4 cache lines. Contiguous storage means the
prefetcher sees a perfectly regular stride.

### Apple's SVE situation

ARM's newer SVE/SVE2 offers variable-width vectors. As of the M1/M2/M3
generation, Apple cores do not expose it to user code, so NEON is what there is.
Code written against NEON continues to work regardless; SVE would be an addition,
not a replacement.

## Reading the generated assembly

```sh
clang++ -O3 -std=c++20 -S -o - src/distance/kernel_neon.cpp | grep -A20 neon_l2_squared
```

Look for `fmla v0.4s, v1.4s, v2.4s` — that is the 4-lane FMA. If you see `fmadd
s0, s1, s2` (no `.4s`), you are looking at scalar code and something has gone
wrong.

Worth doing once on the **scalar** kernel too, to see whether clang
auto-vectorised it. Sometimes it does, and then your hand-written intrinsics buy
much less than you think.

## Common mistakes

**Reducing inside the loop.** `vaddvq_f32` per iteration destroys the benefit —
it is a cross-lane operation with real latency.

**Using aligned loads.** `vld1q_f32` handles unaligned addresses; there is no
faulting variant to reach for by accident in NEON, but the habit carries over
badly to x86, where `_mm256_load_ps` does fault.

**Forgetting the tail.** `D = 5` will find it, and nothing else will.

**One accumulator.** Correct, and roughly 2× slower than four.

**Assuming SIMD fixes a memory-bound loop.** It does not. See the end of
[03-simd-introduction](03-simd-introduction.md).

## Experiments

1. Dump the assembly for both kernels. Count `fmla` instructions. Check whether
   the scalar one was auto-vectorised.
2. Vary the accumulator count: 1, 2, 4, 8. Plot ns/call. Find the knee and
   explain it in terms of FMA latency versus issue rate.
3. Process 8 floats per iteration instead of 16 and measure.
4. Write a NEON cosine kernel that computes the dot product and both norms in
   one pass over the data, instead of three. Validate against scalar first, then
   measure.
5. Time `l2_squared` at `D = 4` and `D = 4096`. Compute ns per component for
   each. The gap is the fixed overhead, and it tells you the dimension below
   which SIMD stops being worth the call.

## Where this lives in the code

- `src/distance/kernel_neon.cpp` — the kernels
- `src/distance/cpu_features.cpp` — `cpu_supports_neon`, and why it is trivial
- `tests/unit/distance_kernel_test.cpp` — validated against scalar, every tail
- `docs/benchmark-results.md` — the measured 3.6–4.1×

---

Next: [05-avx2.md](05-avx2.md)
