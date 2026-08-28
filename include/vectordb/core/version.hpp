// SPDX-License-Identifier: MIT
//
// Version identifiers.
//
// There are two independent kinds of version in this project and conflating
// them is a classic database bug:
//
//   * the *software* version   — which build of the `vectordb` binary this is
//   * the *format* versions    — what layout the bytes on disk are in
//
// A user may upgrade the binary without rewriting their data, so every
// persisted artefact carries its own format version and is validated on load.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace vectordb {

inline constexpr std::uint32_t kVersionMajor = 0;
inline constexpr std::uint32_t kVersionMinor = 1;
inline constexpr std::uint32_t kVersionPatch = 0;

/// e.g. "0.1.0"
[[nodiscard]] std::string_view version_string() noexcept;

/// A multi-line banner: version, compiler, build type, active SIMD kernel.
/// Printed by `vectordb version` and recorded in benchmark reports so that a
/// number can always be traced back to the build that produced it.
[[nodiscard]] std::string build_info();

}  // namespace vectordb
