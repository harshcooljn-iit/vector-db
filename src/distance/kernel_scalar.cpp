// SPDX-License-Identifier: MIT
//
// The portable reference kernel.
//
// Written to be obviously correct first and reasonably fast second. It is the
// oracle every optimised kernel is validated against, so cleverness here would
// undermine its entire purpose.
#include <cstddef>

#include <vectordb/distance/kernel.hpp>

namespace vectordb {
namespace {

// ---------------------------------------------------------------------------
// On the accumulator strategy
// ---------------------------------------------------------------------------
// A single `float sum` is the obvious loop. It has two problems, and they pull
// in the same direction:
//
//   Accuracy. Each += rounds. Once `sum` is large relative to the next term,
//   small terms round away entirely. Over 1536 dimensions this is visible.
//
//   Speed. Every += depends on the previous one, so the loop is a single
//   serial dependency chain. A float add has ~3-4 cycles of latency but the FPU
//   can start a new one every cycle, so a serial chain leaves ~75% of the
//   pipeline idle.
//
// Four independent accumulators fix both at once: four chains that can be in
// flight simultaneously, each summing a quarter of the terms so each stays
// smaller and rounds less. This is the same restructuring `-ffast-math` would
// let the compiler perform on its own — we do it explicitly instead, because a
// kernel whose results are asserted against another kernel must not have its
// arithmetic silently rearranged. See ADR-0005 and the note in CMakeLists.txt.
//
// Four is chosen to match the lane count of the NEON kernel that will follow,
// so scalar and SIMD sum in the same groupings and their results agree to a
// much tighter tolerance than they otherwise would.
constexpr std::size_t kUnroll = 4;

float scalar_l2_squared(const float* a, const float* b, std::size_t count) noexcept {
    float sum0 = 0.0F;
    float sum1 = 0.0F;
    float sum2 = 0.0F;
    float sum3 = 0.0F;

    std::size_t i = 0;
    const std::size_t limit = count - (count % kUnroll);
    for (; i < limit; i += kUnroll) {
        const float d0 = a[i + 0] - b[i + 0];
        const float d1 = a[i + 1] - b[i + 1];
        const float d2 = a[i + 2] - b[i + 2];
        const float d3 = a[i + 3] - b[i + 3];
        sum0 += d0 * d0;
        sum1 += d1 * d1;
        sum2 += d2 * d2;
        sum3 += d3 * d3;
    }

    // Tail. Dimensions are usually multiples of 4 (128, 384, 768, 1536) so this
    // runs zero times in the common case, but it must be correct for D = 5.
    float tail = 0.0F;
    for (; i < count; ++i) {
        const float d = a[i] - b[i];
        tail += d * d;
    }

    return ((sum0 + sum1) + (sum2 + sum3)) + tail;
}

float scalar_dot(const float* a, const float* b, std::size_t count) noexcept {
    float sum0 = 0.0F;
    float sum1 = 0.0F;
    float sum2 = 0.0F;
    float sum3 = 0.0F;

    std::size_t i = 0;
    const std::size_t limit = count - (count % kUnroll);
    for (; i < limit; i += kUnroll) {
        sum0 += a[i + 0] * b[i + 0];
        sum1 += a[i + 1] * b[i + 1];
        sum2 += a[i + 2] * b[i + 2];
        sum3 += a[i + 3] * b[i + 3];
    }

    float tail = 0.0F;
    for (; i < count; ++i) {
        tail += a[i] * b[i];
    }

    return ((sum0 + sum1) + (sum2 + sum3)) + tail;
}

float scalar_norm_squared(const float* values, std::size_t count) noexcept {
    return scalar_dot(values, values, count);
}

constexpr DistanceKernel kScalarKernel{
    .l2_squared = &scalar_l2_squared,
    .dot = &scalar_dot,
    .norm_squared = &scalar_norm_squared,
    .name = "scalar",
};

}  // namespace

const DistanceKernel& scalar_kernel() noexcept {
    return kScalarKernel;
}

}  // namespace vectordb
