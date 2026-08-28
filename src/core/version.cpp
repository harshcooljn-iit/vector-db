// SPDX-License-Identifier: MIT
#include <string>

#include <vectordb/core/version.hpp>

namespace vectordb {
namespace {

constexpr std::string_view kVersionString = "0.1.0";

constexpr std::string_view compiler_name() noexcept {
#if defined(__clang__) && defined(__apple_build_version__)
    return "AppleClang";
#elif defined(__clang__)
    return "Clang";
#elif defined(__GNUC__)
    return "GCC";
#elif defined(_MSC_VER)
    return "MSVC";
#else
    return "unknown";
#endif
}

constexpr std::string_view compiler_version() noexcept {
#if defined(__clang_version__)
    return __clang_version__;
#elif defined(__VERSION__)
    return __VERSION__;
#else
    return "unknown";
#endif
}

constexpr std::string_view build_type() noexcept {
#if defined(NDEBUG)
    return "Release (NDEBUG defined, assertions off)";
#else
    return "Debug (assertions on)";
#endif
}

constexpr std::string_view target_arch() noexcept {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#else
    return "unknown";
#endif
}

}  // namespace

std::string_view version_string() noexcept {
    return kVersionString;
}

std::string build_info() {
    std::string out;
    out += "VectorDB ";
    out += kVersionString;
    out += "\n  compiler:   ";
    out += compiler_name();
    out += " ";
    out += compiler_version();
    out += "\n  build type: ";
    out += build_type();
    out += "\n  target:     ";
    out += target_arch();
    out += "\n  C++:        ";
    out += std::to_string(__cplusplus);
    out += "\n";
    return out;
}

}  // namespace vectordb
