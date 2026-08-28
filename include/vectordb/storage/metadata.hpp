// SPDX-License-Identifier: MIT
//
// The value model for metadata attached to a vector.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <variant>

namespace vectordb {

/// One metadata value.
///
/// The four types are exactly SQLite's storage classes minus BLOB, which is
/// deliberate: metadata is for the attributes you filter and display, and large
/// binary payloads belong in the vector store or outside the database
/// altogether. A `std::monostate` means the key exists with a null value.
using MetadataValue = std::variant<std::monostate, std::int64_t, double, std::string>;

/// A vector's metadata: an ordered map from key to value.
///
/// `std::map` rather than `unordered_map` so that JSON output, `vectordb get`
/// and every test expectation are in a stable key order. Metadata objects are
/// small — a handful of keys — so the tree's overhead is irrelevant and the
/// determinism is worth having.
using Metadata = std::map<std::string, MetadataValue>;

/// Type tag as persisted, so that a value's type survives a round trip through
/// SQLite's dynamic typing rather than being guessed on read.
enum class MetadataType : std::uint8_t {
    kNull = 0,
    kInteger = 1,
    kReal = 2,
    kText = 3,
};

[[nodiscard]] MetadataType type_of(const MetadataValue& value) noexcept;

/// Human-readable rendering, used by the CLI's table output.
[[nodiscard]] std::string to_display_string(const MetadataValue& value);

/// JSON rendering: strings quoted and escaped, numbers bare, null as `null`.
[[nodiscard]] std::string to_json_string(const MetadataValue& value);

}  // namespace vectordb
