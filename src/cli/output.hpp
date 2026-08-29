// SPDX-License-Identifier: MIT
//
// Output formatting.
//
// Two independent renderings of the same data, and the JSON one never depends
// on the human one. A caller piping `--json` into `jq` must not be affected by
// a change to column alignment, and a machine-readable format that is produced
// by string-munging a table is a machine-readable format in name only.
#pragma once

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include <vectordb/db/database.hpp>

namespace vectordb::cli {

/// Groups digits: 1250000 -> "1,250,000". Applied to human output only; JSON
/// gets bare numbers.
[[nodiscard]] std::string group_digits(std::uint64_t value);

/// 3.07 GB, 155.2 MB, 5.0 KB, 512 B.
[[nodiscard]] std::string human_bytes(std::uint64_t bytes);

/// Escapes and quotes a JSON string.
[[nodiscard]] std::string json_string(std::string_view text);

/// Renders metadata as a JSON object.
[[nodiscard]] std::string metadata_to_json(const Metadata& metadata);

void print_info(std::ostream& out, const Database& database, bool json);
void print_stats(std::ostream& out, const DatabaseStats& stats, bool json);
void print_results(std::ostream& out,
                   const std::vector<QueryResult>& results,
                   Metric metric,
                   bool json,
                   bool show_metadata);
void print_batch_results(std::ostream& out,
                         const std::vector<std::vector<QueryResult>>& results,
                         Metric metric,
                         bool json);
void print_stored_vector(std::ostream& out,
                         const StoredVector& stored,
                         bool json,
                         std::size_t preview_components);
void print_check(std::ostream& out, const CheckReport& report, bool json);

}  // namespace vectordb::cli
