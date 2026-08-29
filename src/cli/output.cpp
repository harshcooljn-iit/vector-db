// SPDX-License-Identifier: MIT
#include "cli/output.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <iomanip>
#include <sstream>

namespace vectordb::cli {
namespace {

/// Fixed-precision score rendering, so a column of results lines up.
std::string format_score(float score) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(4) << score;
    return out.str();
}

}  // namespace

std::string group_digits(std::uint64_t value) {
    const std::string digits = std::to_string(value);
    std::string grouped;
    grouped.reserve(digits.size() + digits.size() / 3);

    // Built right to left, because that is the direction the rule runs in.
    // Computing the position of the first separator from the left invites
    // exactly the unsigned-underflow bug this replaced: `(i - leading) % 3`
    // with size_t operands wraps instead of going negative, and produces
    // "2,0,000" for 20000.
    std::size_t since_separator = 0;
    for (auto digit = digits.rbegin(); digit != digits.rend(); ++digit) {
        if (since_separator == 3) {
            grouped.push_back(',');
            since_separator = 0;
        }
        grouped.push_back(*digit);
        ++since_separator;
    }
    std::reverse(grouped.begin(), grouped.end());
    return grouped;
}

std::string human_bytes(std::uint64_t bytes) {
    constexpr std::uint64_t kUnit = 1024;
    if (bytes < kUnit) {
        return std::to_string(bytes) + " B";
    }

    const char* suffixes[] = {"KB", "MB", "GB", "TB", "PB"};
    double value = static_cast<double>(bytes);
    int index = -1;
    while (value >= static_cast<double>(kUnit) && index < 4) {
        value /= static_cast<double>(kUnit);
        ++index;
    }

    std::ostringstream out;
    out << std::fixed << std::setprecision(value < 10.0 ? 2 : 1) << value << " "
        << suffixes[index];
    return out.str();
}

std::string json_string(std::string_view text) {
    std::string quoted;
    quoted.reserve(text.size() + 2);
    quoted.push_back('"');
    for (const char character : text) {
        switch (character) {
            case '"':
                quoted += "\\\"";
                break;
            case '\\':
                quoted += "\\\\";
                break;
            case '\n':
                quoted += "\\n";
                break;
            case '\r':
                quoted += "\\r";
                break;
            case '\t':
                quoted += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(character) < 0x20) {
                    std::array<char, 8> buffer{};
                    std::snprintf(buffer.data(),
                                  buffer.size(),
                                  "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(character)));
                    quoted += buffer.data();
                } else {
                    quoted.push_back(character);
                }
        }
    }
    quoted.push_back('"');
    return quoted;
}

std::string metadata_to_json(const Metadata& metadata) {
    std::string json = "{";
    bool first = true;
    for (const auto& [key, value] : metadata) {
        if (!first) {
            json += ",";
        }
        first = false;
        json += json_string(key);
        json += ":";
        json += to_json_string(value);
    }
    json += "}";
    return json;
}

void print_info(std::ostream& out, const Database& database, bool json) {
    const DatabaseConfig& config = database.config();
    const DatabaseStats stats = database.stats();

    if (json) {
        out << "{\n"
            << "  \"path\": " << json_string(database.directory().string()) << ",\n"
            << "  \"dimension\": " << config.dimension << ",\n"
            << "  \"metric\": " << json_string(metric_name(config.metric)) << ",\n"
            << "  \"index_type\": " << json_string(index_type_name(config.index_type)) << ",\n"
            << "  \"index\": " << json_string(stats.index_description) << ",\n"
            << "  \"normalize\": " << (config.normalize ? "true" : "false") << ",\n"
            << "  \"vectors\": " << stats.live_vectors << ",\n"
            << "  \"slots\": " << stats.slots << ",\n"
            << "  \"tombstones\": " << stats.tombstones << ",\n";
        if (config.index_type == IndexType::kHnsw) {
            out << "  \"hnsw\": {\n"
                << "    \"m\": " << config.hnsw.m << ",\n"
                << "    \"ef_construction\": " << config.hnsw.ef_construction << ",\n"
                << "    \"ef_search\": " << config.hnsw.ef_search << ",\n"
                << "    \"seed\": " << config.hnsw.seed << "\n"
                << "  },\n";
        }
        out << "  \"metadata_keys\": [";
        for (std::size_t i = 0; i < stats.metadata_keys.size(); ++i) {
            out << (i > 0 ? "," : "") << json_string(stats.metadata_keys[i]);
        }
        out << "]\n}\n";
        return;
    }

    out << "Database:  " << database.directory().string() << "\n"
        << "Dimension: " << config.dimension << "\n"
        << "Metric:    " << metric_name(config.metric)
        << (config.normalize ? " (vectors normalized on insert)" : "") << "\n"
        << "Index:     " << stats.index_description << "\n"
        << "Vectors:   " << group_digits(stats.live_vectors);
    if (stats.tombstones > 0) {
        out << "  (" << group_digits(stats.tombstones) << " deleted, not yet reclaimed)";
    }
    out << "\n";

    if (!stats.metadata_keys.empty()) {
        out << "Metadata:  ";
        for (std::size_t i = 0; i < stats.metadata_keys.size(); ++i) {
            out << (i > 0 ? ", " : "") << stats.metadata_keys[i];
        }
        out << "\n";
    }
}

void print_stats(std::ostream& out, const DatabaseStats& stats, bool json) {
    if (json) {
        out << "{\n"
            << "  \"dimension\": " << stats.dimension << ",\n"
            << "  \"metric\": " << json_string(metric_name(stats.metric)) << ",\n"
            << "  \"index\": " << json_string(stats.index_description) << ",\n"
            << "  \"live_vectors\": " << stats.live_vectors << ",\n"
            << "  \"slots\": " << stats.slots << ",\n"
            << "  \"tombstones\": " << stats.tombstones << ",\n"
            << "  \"tombstone_ratio\": " << stats.tombstone_ratio << ",\n"
            << "  \"metadata_vectors\": " << stats.metadata_vectors << ",\n"
            << "  \"metadata_rows\": " << stats.metadata_rows << ",\n"
            << "  \"memory\": {\n"
            << "    \"vectors\": " << stats.vector_bytes << ",\n"
            << "    \"index\": " << stats.index_bytes << "\n"
            << "  },\n"
            << "  \"files\": {\n"
            << "    \"vectors\": " << stats.vector_file_bytes << ",\n"
            << "    \"index\": " << stats.index_file_bytes << ",\n"
            << "    \"metadata\": " << stats.metadata_file_bytes << "\n"
            << "  },\n"
            << "  \"dirty\": " << (stats.dirty ? "true" : "false") << "\n}\n";
        return;
    }

    const auto row = [&out](std::string_view label, const std::string& value) {
        out << "  " << std::left << std::setw(26) << label << value << "\n";
    };

    out << "Contents\n";
    row("live vectors", group_digits(stats.live_vectors));
    row("slots (incl. deleted)", group_digits(stats.slots));
    row("tombstones", group_digits(stats.tombstones));
    {
        std::ostringstream ratio;
        ratio << std::fixed << std::setprecision(1) << (stats.tombstone_ratio * 100.0) << "%";
        row("tombstone ratio", ratio.str());
    }

    out << "\nMetadata\n";
    row("vectors with metadata", group_digits(stats.metadata_vectors));
    row("key-value rows", group_digits(stats.metadata_rows));

    out << "\nMemory (resident)\n";
    row("vectors", human_bytes(stats.vector_bytes));
    row("index", human_bytes(stats.index_bytes));
    if (stats.live_vectors > 0) {
        std::ostringstream per;
        per << (stats.index_bytes / stats.live_vectors) << " bytes/vector";
        row("index overhead", per.str());
    }

    out << "\nOn disk\n";
    row("vectors.bin", human_bytes(stats.vector_file_bytes));
    row("index.hnsw", human_bytes(stats.index_file_bytes));
    row("metadata.sqlite", human_bytes(stats.metadata_file_bytes));

    if (stats.dirty) {
        out << "\nThere are unflushed changes in memory.\n";
    }
}

void print_results(std::ostream& out,
                   const std::vector<QueryResult>& results,
                   Metric metric,
                   bool json,
                   bool show_metadata) {
    if (json) {
        out << "{\n  \"metric\": " << json_string(metric_name(metric))
            << ",\n  \"higher_is_better\": "
            << (higher_score_is_better(metric) ? "true" : "false") << ",\n  \"results\": [\n";
        for (std::size_t i = 0; i < results.size(); ++i) {
            out << "    {\"id\": " << results[i].id << ", \"score\": " << results[i].score;
            if (!results[i].metadata.empty()) {
                out << ", \"metadata\": " << metadata_to_json(results[i].metadata);
            }
            out << "}" << (i + 1 < results.size() ? "," : "") << "\n";
        }
        out << "  ]\n}\n";
        return;
    }

    if (results.empty()) {
        out << "No results.\n";
        return;
    }

    // The direction matters to a reader: a cosine score of 0.91 is good and an
    // L2 score of 0.91 might not be, so say which.
    out << "Results (" << metric_name(metric) << ", "
        << (higher_score_is_better(metric) ? "higher is better" : "lower is better")
        << "):\n\n";

    const std::size_t width = std::to_string(results.size()).size();
    for (std::size_t i = 0; i < results.size(); ++i) {
        out << "  " << std::setw(static_cast<int>(width)) << (i + 1) << ". id=" << std::left
            << std::setw(12) << results[i].id << std::right
            << " score=" << format_score(results[i].score);

        if (show_metadata && !results[i].metadata.empty()) {
            out << "  ";
            bool first = true;
            for (const auto& [key, value] : results[i].metadata) {
                out << (first ? "" : " ") << key << "=" << to_display_string(value);
                first = false;
            }
        }
        out << "\n";
    }
}

void print_batch_results(std::ostream& out,
                         const std::vector<std::vector<QueryResult>>& results,
                         Metric metric,
                         bool json) {
    if (json) {
        out << "{\n  \"metric\": " << json_string(metric_name(metric))
            << ",\n  \"queries\": [\n";
        for (std::size_t q = 0; q < results.size(); ++q) {
            out << "    [";
            for (std::size_t i = 0; i < results[q].size(); ++i) {
                out << (i > 0 ? ", " : "") << "{\"id\": " << results[q][i].id
                    << ", \"score\": " << results[q][i].score << "}";
            }
            out << "]" << (q + 1 < results.size() ? "," : "") << "\n";
        }
        out << "  ]\n}\n";
        return;
    }

    for (std::size_t q = 0; q < results.size(); ++q) {
        out << "Query " << q << ":\n";
        for (std::size_t i = 0; i < results[q].size(); ++i) {
            out << "  " << (i + 1) << ". id=" << results[q][i].id
                << " score=" << format_score(results[q][i].score) << "\n";
        }
        out << "\n";
    }
}

void print_stored_vector(std::ostream& out,
                         const StoredVector& stored,
                         bool json,
                         std::size_t preview_components) {
    if (json) {
        out << "{\n  \"id\": " << stored.id << ",\n  \"dimension\": " << stored.vector.size()
            << ",\n  \"vector\": [";
        for (std::size_t i = 0; i < stored.vector.size(); ++i) {
            out << (i > 0 ? "," : "") << stored.vector[i];
        }
        out << "],\n  \"metadata\": " << metadata_to_json(stored.metadata) << "\n}\n";
        return;
    }

    out << "id:        " << stored.id << "\n"
        << "dimension: " << stored.vector.size() << "\n"
        << "vector:    [";

    const std::size_t shown = std::min(preview_components, stored.vector.size());
    for (std::size_t i = 0; i < shown; ++i) {
        out << (i > 0 ? ", " : "") << std::setprecision(6) << stored.vector[i];
    }
    if (shown < stored.vector.size()) {
        out << ", ... (" << (stored.vector.size() - shown) << " more; use --json for all)";
    }
    out << "]\n";

    if (!stored.metadata.empty()) {
        out << "metadata:\n";
        for (const auto& [key, value] : stored.metadata) {
            out << "  " << std::left << std::setw(20) << key << to_display_string(value)
                << "\n";
        }
    }
}

void print_check(std::ostream& out, const CheckReport& report, bool json) {
    if (json) {
        out << "{\n  \"ok\": " << (report.ok() ? "true" : "false")
            << ",\n  \"errors\": " << report.error_count()
            << ",\n  \"warnings\": " << report.warning_count() << ",\n  \"issues\": [\n";
        for (std::size_t i = 0; i < report.issues.size(); ++i) {
            out << "    {\"severity\": " << json_string(report.issues[i].severity)
                << ", \"message\": " << json_string(report.issues[i].message) << "}"
                << (i + 1 < report.issues.size() ? "," : "") << "\n";
        }
        out << "  ]\n}\n";
        return;
    }

    if (report.ok()) {
        out << "OK — no problems found.\n";
        return;
    }

    for (const CheckIssue& issue : report.issues) {
        out << (issue.severity == "error" ? "error:   " : "warning: ") << issue.message << "\n";
    }
    out << "\n"
        << report.error_count() << " error(s), " << report.warning_count() << " warning(s).\n";
}

}  // namespace vectordb::cli
