// SPDX-License-Identifier: MIT
//
// Runtime CPU capability detection.
//
// The rule this exists to enforce: a kernel is only ever offered if the CPU
// executing this process can run its instructions. Compiling an AVX2 kernel and
// calling it unconditionally works on the developer's machine and raises SIGILL
// on a user's older one — a crash with no diagnostic, from a binary that ran
// fine in testing.
//
// Learning note: learnings/70-performance/05-avx2.md
#pragma once

namespace vectordb {

/// What this binary was compiled for.
#if defined(__aarch64__) || defined(_M_ARM64)
#define VECTORDB_ARCH_ARM64 1
#else
#define VECTORDB_ARCH_ARM64 0
#endif

#if defined(__x86_64__) || defined(_M_X64)
#define VECTORDB_ARCH_X86_64 1
#else
#define VECTORDB_ARCH_X86_64 0
#endif

/// True when this CPU supports AVX2 *and* FMA.
///
/// Both are checked, because the kernel uses both. AVX2 without FMA exists in
/// principle; assuming otherwise is how a "detected" kernel still crashes.
///
/// Always false on non-x86 targets.
[[nodiscard]] bool cpu_supports_avx2() noexcept;

/// True when NEON is available.
///
/// On arm64 this is unconditionally true: NEON (Advanced SIMD) is part of the
/// mandatory base instruction set, so there is nothing to detect. It exists as
/// a function for symmetry, and because 32-bit ARM did make it optional.
[[nodiscard]] bool cpu_supports_neon() noexcept;

}  // namespace vectordb
