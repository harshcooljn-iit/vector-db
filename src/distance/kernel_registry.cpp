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

namespace vectordb {
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

    // SIMD kernels are appended here as they are implemented, each guarded by
    // both a compile-time check (is it in this binary?) and a runtime check
    // (can this CPU run it?). See phase 15.

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
