// SPDX-License-Identifier: MIT
//
// ARM NEON distance kernels.
//
// Only compiled on arm64. NEON is part of the mandatory base instruction set
// there, so no runtime guard is needed — unlike AVX2 on x86.
//
// Learning note: learnings/70-performance/04-arm-neon.md
#include "distance/cpu_features.hpp"

#if VECTORDB_ARCH_ARM64

#include <cstddef>

#include <vectordb/distance/kernel.hpp>

#include <arm_neon.h>

namespace vectordb {
namespace {

// Four vector accumulators of four lanes each: 16 floats per iteration.
//
// Four, for the same reason the scalar kernel uses four scalar accumulators —
// a floating-point add has several cycles of latency but can be issued every
// cycle, so a single accumulator leaves most of the pipeline idle waiting on
// its own previous result. Four independent chains keep it fed.
//
// The lane count is what SIMD adds on top: each of those four adds now handles
// four values instead of one.
constexpr std::size_t kBlock = 16;

/// Sums the four lanes of a NEON register into one float.
///
/// `vaddvq_f32` is an arm64 instruction; 32-bit ARM needs a pairwise-add
/// sequence instead. Since this file only compiles on arm64, the single
/// instruction is available.
inline float horizontal_sum(float32x4_t value) noexcept {
    return vaddvq_f32(value);
}

float neon_l2_squared(const float* a, const float* b, std::size_t count) noexcept {
    float32x4_t sum0 = vdupq_n_f32(0.0F);
    float32x4_t sum1 = vdupq_n_f32(0.0F);
    float32x4_t sum2 = vdupq_n_f32(0.0F);
    float32x4_t sum3 = vdupq_n_f32(0.0F);

    std::size_t i = 0;
    for (; i + kBlock <= count; i += kBlock) {
        const float32x4_t d0 = vsubq_f32(vld1q_f32(a + i + 0), vld1q_f32(b + i + 0));
        const float32x4_t d1 = vsubq_f32(vld1q_f32(a + i + 4), vld1q_f32(b + i + 4));
        const float32x4_t d2 = vsubq_f32(vld1q_f32(a + i + 8), vld1q_f32(b + i + 8));
        const float32x4_t d3 = vsubq_f32(vld1q_f32(a + i + 12), vld1q_f32(b + i + 12));

        // Fused multiply-add: one instruction, and one rounding step instead of
        // two. The result is slightly *more* accurate than a separate multiply
        // and add, not less — which is worth knowing, because the intuition
        // usually runs the other way.
        sum0 = vfmaq_f32(sum0, d0, d0);
        sum1 = vfmaq_f32(sum1, d1, d1);
        sum2 = vfmaq_f32(sum2, d2, d2);
        sum3 = vfmaq_f32(sum3, d3, d3);
    }

    // Whole vectors that did not fill a block.
    for (; i + 4 <= count; i += 4) {
        const float32x4_t d = vsubq_f32(vld1q_f32(a + i), vld1q_f32(b + i));
        sum0 = vfmaq_f32(sum0, d, d);
    }

    float total = horizontal_sum(vaddq_f32(vaddq_f32(sum0, sum1), vaddq_f32(sum2, sum3)));

    // Scalar tail. Zero iterations for every dimension anyone actually uses
    // (128, 384, 768, 1536 are all multiples of 16), but it must be right for
    // D = 5.
    for (; i < count; ++i) {
        const float d = a[i] - b[i];
        total += d * d;
    }
    return total;
}

float neon_dot(const float* a, const float* b, std::size_t count) noexcept {
    float32x4_t sum0 = vdupq_n_f32(0.0F);
    float32x4_t sum1 = vdupq_n_f32(0.0F);
    float32x4_t sum2 = vdupq_n_f32(0.0F);
    float32x4_t sum3 = vdupq_n_f32(0.0F);

    std::size_t i = 0;
    for (; i + kBlock <= count; i += kBlock) {
        sum0 = vfmaq_f32(sum0, vld1q_f32(a + i + 0), vld1q_f32(b + i + 0));
        sum1 = vfmaq_f32(sum1, vld1q_f32(a + i + 4), vld1q_f32(b + i + 4));
        sum2 = vfmaq_f32(sum2, vld1q_f32(a + i + 8), vld1q_f32(b + i + 8));
        sum3 = vfmaq_f32(sum3, vld1q_f32(a + i + 12), vld1q_f32(b + i + 12));
    }

    for (; i + 4 <= count; i += 4) {
        sum0 = vfmaq_f32(sum0, vld1q_f32(a + i), vld1q_f32(b + i));
    }

    float total = horizontal_sum(vaddq_f32(vaddq_f32(sum0, sum1), vaddq_f32(sum2, sum3)));

    for (; i < count; ++i) {
        total += a[i] * b[i];
    }
    return total;
}

float neon_norm_squared(const float* values, std::size_t count) noexcept {
    return neon_dot(values, values, count);
}

constexpr DistanceKernel kNeonKernel{
    .l2_squared = &neon_l2_squared,
    .dot = &neon_dot,
    .norm_squared = &neon_norm_squared,
    .name = "neon",
};

}  // namespace

const DistanceKernel& neon_kernel() noexcept {
    return kNeonKernel;
}

}  // namespace vectordb

#endif  // VECTORDB_ARCH_ARM64
