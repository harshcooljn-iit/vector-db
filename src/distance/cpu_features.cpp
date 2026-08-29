// SPDX-License-Identifier: MIT
#include "distance/cpu_features.hpp"

#if VECTORDB_ARCH_X86_64
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#include <cstdint>
#endif

namespace vectordb {
namespace {

#if VECTORDB_ARCH_X86_64

struct CpuidResult {
    std::uint32_t eax = 0;
    std::uint32_t ebx = 0;
    std::uint32_t ecx = 0;
    std::uint32_t edx = 0;
};

CpuidResult cpuid(std::uint32_t leaf, std::uint32_t subleaf) noexcept {
    CpuidResult result;
#if defined(_MSC_VER)
    int registers[4] = {0, 0, 0, 0};
    __cpuidex(registers, static_cast<int>(leaf), static_cast<int>(subleaf));
    result.eax = static_cast<std::uint32_t>(registers[0]);
    result.ebx = static_cast<std::uint32_t>(registers[1]);
    result.ecx = static_cast<std::uint32_t>(registers[2]);
    result.edx = static_cast<std::uint32_t>(registers[3]);
#else
    __cpuid_count(leaf, subleaf, result.eax, result.ebx, result.ecx, result.edx);
#endif
    return result;
}

/// Reads the extended control register, which says whether the *operating
/// system* has enabled AVX state saving.
///
/// This check is the one people forget, and skipping it is a real crash. A CPU
/// can support AVX2 while the OS does not save YMM registers on a context
/// switch; using AVX2 there corrupts state or faults. CPUID alone is not enough.
std::uint64_t read_xcr0() noexcept {
#if defined(_MSC_VER)
    return _xgetbv(0);
#else
    std::uint32_t low = 0;
    std::uint32_t high = 0;
    __asm__ volatile(".byte 0x0f, 0x01, 0xd0" : "=a"(low), "=d"(high) : "c"(0));
    return (static_cast<std::uint64_t>(high) << 32) | low;
#endif
}

bool detect_avx2() noexcept {
    const CpuidResult leaf0 = cpuid(0, 0);
    if (leaf0.eax < 7) {
        return false;  // the CPU is too old to even report extended features
    }

    constexpr std::uint32_t kOsXsaveBit = 1U << 27;
    constexpr std::uint32_t kAvxBit = 1U << 28;
    constexpr std::uint32_t kFmaBit = 1U << 12;

    const CpuidResult leaf1 = cpuid(1, 0);
    const bool os_xsave = (leaf1.ecx & kOsXsaveBit) != 0;
    const bool avx = (leaf1.ecx & kAvxBit) != 0;
    const bool fma = (leaf1.ecx & kFmaBit) != 0;
    if (!os_xsave || !avx || !fma) {
        return false;
    }

    // Bits 1 (SSE) and 2 (AVX) of XCR0 must both be set, or the OS is not
    // preserving the registers we are about to use.
    constexpr std::uint64_t kXmmYmmState = 0x6;
    if ((read_xcr0() & kXmmYmmState) != kXmmYmmState) {
        return false;
    }

    constexpr std::uint32_t kAvx2Bit = 1U << 5;
    return (cpuid(7, 0).ebx & kAvx2Bit) != 0;
}

#endif  // VECTORDB_ARCH_X86_64

}  // namespace

bool cpu_supports_avx2() noexcept {
#if VECTORDB_ARCH_X86_64
    // Detected once. CPU features do not change while a process runs, and the
    // cpuid instruction is serialising, so calling it repeatedly would be a
    // needless pipeline flush.
    static const bool supported = detect_avx2();
    return supported;
#else
    return false;
#endif
}

bool cpu_supports_neon() noexcept {
#if VECTORDB_ARCH_ARM64
    // Mandatory on arm64; nothing to detect.
    return true;
#elif defined(__ARM_NEON)
    return true;
#else
    return false;
#endif
}

}  // namespace vectordb
