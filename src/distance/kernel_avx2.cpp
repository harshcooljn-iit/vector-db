// SPDX-License-Identifier: MIT
//
// x86-64 AVX2 + FMA distance kernels.
//
// ## Honesty note
//
// This code is compiled and reviewed but has **not been executed on real AVX2
// hardware** by this project's author — development and benchmarking happened
// on Apple Silicon. It is guarded by runtime detection so it can never run
// where it is unsupported, and it is validated against the scalar kernel by the
// same parameterised test suite every other kernel runs. But "compiles and is
// guarded" is not "measured", and `docs/benchmark-results.md` reports no AVX2
// numbers rather than estimated ones.
//
// Learning note: learnings/70-performance/05-avx2.md
#include "distance/cpu_features.hpp"

#if VECTORDB_ARCH_X86_64

#include <cstddef>

#include <vectordb/distance/kernel.hpp>

#include <immintrin.h>

namespace vectordb {
namespace {

// A 256-bit AVX register holds 8 floats. Four accumulators, 32 floats per
// iteration.
constexpr std::size_t kBlock = 32;

// The target attribute lets these functions use AVX2 and FMA without compiling
// the *whole translation unit* for AVX2 — which would be a disaster, because
// the runtime check that guards them would itself be compiled with AVX2 and
// would crash before it could decide anything.
#define VECTORDB_AVX2 __attribute__((target("avx2,fma")))

VECTORDB_AVX2 inline float horizontal_sum(__m256 value) noexcept {
    // Fold 256 bits to 128, then 128 to 64, then 64 to 32. A tree, not a linear
    // scan: three steps instead of seven.
    const __m128 low = _mm256_castps256_ps128(value);
    const __m128 high = _mm256_extractf128_ps(value, 1);
    __m128 sum = _mm_add_ps(low, high);
    sum = _mm_add_ps(sum, _mm_movehl_ps(sum, sum));
    sum = _mm_add_ss(sum, _mm_shuffle_ps(sum, sum, 0x55));
    return _mm_cvtss_f32(sum);
}

VECTORDB_AVX2 float avx2_l2_squared(const float* a,
                                    const float* b,
                                    std::size_t count) noexcept {
    __m256 sum0 = _mm256_setzero_ps();
    __m256 sum1 = _mm256_setzero_ps();
    __m256 sum2 = _mm256_setzero_ps();
    __m256 sum3 = _mm256_setzero_ps();

    std::size_t i = 0;
    for (; i + kBlock <= count; i += kBlock) {
        // loadu, not load: the aligned form faults on a misaligned address, and
        // stride == dimension means rows are only 4-byte aligned for odd
        // dimensions. On every microarchitecture that supports AVX2, an
        // unaligned load that does not straddle a cache line costs the same as
        // an aligned one.
        const __m256 d0 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 0), _mm256_loadu_ps(b + i + 0));
        const __m256 d1 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8));
        const __m256 d2 =
            _mm256_sub_ps(_mm256_loadu_ps(a + i + 16), _mm256_loadu_ps(b + i + 16));
        const __m256 d3 =
            _mm256_sub_ps(_mm256_loadu_ps(a + i + 24), _mm256_loadu_ps(b + i + 24));

        sum0 = _mm256_fmadd_ps(d0, d0, sum0);
        sum1 = _mm256_fmadd_ps(d1, d1, sum1);
        sum2 = _mm256_fmadd_ps(d2, d2, sum2);
        sum3 = _mm256_fmadd_ps(d3, d3, sum3);
    }

    for (; i + 8 <= count; i += 8) {
        const __m256 d = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
        sum0 = _mm256_fmadd_ps(d, d, sum0);
    }

    float total =
        horizontal_sum(_mm256_add_ps(_mm256_add_ps(sum0, sum1), _mm256_add_ps(sum2, sum3)));

    for (; i < count; ++i) {
        const float d = a[i] - b[i];
        total += d * d;
    }
    return total;
}

VECTORDB_AVX2 float avx2_dot(const float* a, const float* b, std::size_t count) noexcept {
    __m256 sum0 = _mm256_setzero_ps();
    __m256 sum1 = _mm256_setzero_ps();
    __m256 sum2 = _mm256_setzero_ps();
    __m256 sum3 = _mm256_setzero_ps();

    std::size_t i = 0;
    for (; i + kBlock <= count; i += kBlock) {
        sum0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 0), _mm256_loadu_ps(b + i + 0), sum0);
        sum1 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8), sum1);
        sum2 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 16), _mm256_loadu_ps(b + i + 16), sum2);
        sum3 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 24), _mm256_loadu_ps(b + i + 24), sum3);
    }

    for (; i + 8 <= count; i += 8) {
        sum0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), sum0);
    }

    float total =
        horizontal_sum(_mm256_add_ps(_mm256_add_ps(sum0, sum1), _mm256_add_ps(sum2, sum3)));

    for (; i < count; ++i) {
        total += a[i] * b[i];
    }
    return total;
}

VECTORDB_AVX2 float avx2_norm_squared(const float* values, std::size_t count) noexcept {
    return avx2_dot(values, values, count);
}

#undef VECTORDB_AVX2

constexpr DistanceKernel kAvx2Kernel{
    .l2_squared = &avx2_l2_squared,
    .dot = &avx2_dot,
    .norm_squared = &avx2_norm_squared,
    .name = "avx2",
};

}  // namespace

const DistanceKernel& avx2_kernel() noexcept {
    return kAvx2Kernel;
}

}  // namespace vectordb

#endif  // VECTORDB_ARCH_X86_64
