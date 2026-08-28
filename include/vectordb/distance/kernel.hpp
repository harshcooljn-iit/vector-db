// SPDX-License-Identifier: MIT
//
// The swappable arithmetic layer.
//
// A DistanceKernel is a small table of raw primitives over contiguous float
// runs. Everything above it — metrics, indexes, the query engine — is written
// against this table and never against a specific instruction set. Adding NEON
// or AVX2 later means adding a table, not editing any caller.
//
// Learning note: learnings/70-performance/03-simd-introduction.md
#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace vectordb {

/// Signature of a two-input reduction over `count` floats.
///
/// `noexcept` is part of the type on purpose: these run in the innermost loop
/// of the system, and nothing there may throw.
using BinaryReduceFn = float (*)(const float* a, const float* b, std::size_t count) noexcept;

/// Signature of a one-input reduction over `count` floats.
using UnaryReduceFn = float (*)(const float* values, std::size_t count) noexcept;

/// One implementation of the numeric primitives every metric is built from.
///
/// ## Why a table of function pointers rather than a virtual interface?
///
/// Both cost one indirect call. The table wins on three smaller counts: no
/// vtable pointer and no object to keep alive, a kernel is a constexpr-friendly
/// aggregate, and swapping one in a test or a benchmark is an assignment rather
/// than a class hierarchy.
///
/// ## Why is one indirect call per candidate acceptable at all?
///
/// Because of what it wraps. For dimension 768, `l2_squared` performs 768
/// multiply-adds and streams 6 KB; the indirect call is a few nanoseconds
/// against hundreds. At dimension 4 the ratio inverts and the call dominates —
/// which is exactly why brute force calls the kernel *per candidate* rather
/// than per component, and why very small dimensions are not this system's
/// design target.
struct DistanceKernel {
    /// `sum((a[i] - b[i])^2)`. No square root: it is monotonic, so it cannot
    /// change any ranking, and it costs a `sqrt` per candidate.
    BinaryReduceFn l2_squared;

    /// `sum(a[i] * b[i])`.
    BinaryReduceFn dot;

    /// `sum(v[i]^2)`.
    UnaryReduceFn norm_squared;

    /// Stable identifier: "scalar", "neon", "avx2". Printed by
    /// `vectordb version` and recorded in every benchmark result, because a
    /// timing without the kernel that produced it is not a measurement.
    std::string_view name;
};

/// The portable reference implementation. Always available, on every platform.
///
/// This is the correctness oracle for the SIMD kernels in exactly the way
/// brute force is the oracle for HNSW: it is simple enough to be obviously
/// right, so an optimised kernel that disagrees with it is wrong.
[[nodiscard]] const DistanceKernel& scalar_kernel() noexcept;

/// The fastest kernel this binary can safely run on this machine.
///
/// Selected once, on first call, from the kernels compiled into the binary and
/// the CPU features actually detected at runtime. Never returns a kernel whose
/// instructions the current CPU does not support — compiling an AVX2 kernel and
/// executing it unconditionally is a SIGILL waiting for a user with an older
/// machine.
[[nodiscard]] const DistanceKernel& active_kernel() noexcept;

/// Every kernel compiled into this binary and runnable on this machine,
/// including the scalar one. Used by `vectordb benchmark-distance` to time them
/// against each other, and by tests to check each against scalar.
[[nodiscard]] std::span<const DistanceKernel* const> available_kernels() noexcept;

/// Looks a kernel up by `name`. Returns nullptr if it is not compiled in or not
/// runnable here — a caller asking for "avx2" on Apple Silicon gets nullptr,
/// not a crash.
[[nodiscard]] const DistanceKernel* kernel_by_name(std::string_view name) noexcept;

}  // namespace vectordb
