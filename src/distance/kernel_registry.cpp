// SPDX-License-Identifier: MIT
//
// Which kernel runs, and how that is decided.
//
// The rule this file exists to enforce: a kernel is only ever offered if the
// CPU executing this process can actually run its instructions. Compiling an
// AVX2 kernel and calling it unconditionally works on the developer's machine
// and raises SIGILL on a user's older one.
//
// Learning note: learnings/70-performance/05-avx2.md
#include <array>
#include <vector>

#include <vectordb/distance/kernel.hpp>

#include "distance/cpu_features.hpp"

namespace vectordb {

// Declared here rather than in the public header: which SIMD kernels exist is
// an implementation detail, and a caller asks for one by name.
#if VECTORDB_ARCH_ARM64
const DistanceKernel& neon_kernel() noexcept;
#endif
#if VECTORDB_ARCH_X86_64
const DistanceKernel& avx2_kernel() noexcept;
#endif

namespace {

/// Builds the list of kernels runnable on *this* machine, cheapest to detect
/// first. Called once; the result is cached in a function-local static, which
/// C++11 onwards guarantees is initialised exactly once even under concurrent
/// first calls.
std::vector<const DistanceKernel*> detect_kernels() {
    std::vector<const DistanceKernel*> kernels;
    kernels.reserve(2);

    // Always present, on every platform, by construction.
    kernels.push_back(&scalar_kernel());

    // Appended in increasing order of preference, each guarded twice: a
    // compile-time check that the code is in this binary at all, and a runtime
    // check that this CPU can execute it.
    //
    // VECTORDB_ENABLE_SIMD lets a build opt out entirely, which is how the
    // benchmark suite compares against a scalar-only binary and how a bisect
    // can rule SIMD in or out in one flag.
#if defined(VECTORDB_ENABLE_SIMD) && VECTORDB_ENABLE_SIMD
#if VECTORDB_ARCH_ARM64
    if (cpu_supports_neon()) {
        kernels.push_back(&neon_kernel());
    }
#endif
#if VECTORDB_ARCH_X86_64
    if (cpu_supports_avx2()) {
        kernels.push_back(&avx2_kernel());
    }
#endif
#endif

    return kernels;
}

const std::vector<const DistanceKernel*>& kernel_list() noexcept {
    static const std::vector<const DistanceKernel*> kernels = detect_kernels();
    return kernels;
}

}  // namespace

const DistanceKernel& active_kernel() noexcept {
    // detect_kernels() appends in increasing order of preference, so the last
    // entry is the best one this machine can run.
    return *kernel_list().back();
}

std::span<const DistanceKernel* const> available_kernels() noexcept {
    return {kernel_list().data(), kernel_list().size()};
}

const DistanceKernel* kernel_by_name(std::string_view name) noexcept {
    for (const DistanceKernel* kernel : kernel_list()) {
        if (kernel->name == name) {
            return kernel;
        }
    }
    return nullptr;
}

}  // namespace vectordb
