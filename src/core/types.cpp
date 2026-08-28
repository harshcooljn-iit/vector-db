// SPDX-License-Identifier: MIT
#include <vectordb/core/types.hpp>

#include <array>
#include <utility>

namespace vectordb {
namespace {

// Keeping the spellings in one table means the parser and the printer can
// never drift apart — a surprisingly common source of "why does `info` print
// a metric my config file cannot express?" bugs.
constexpr std::array<std::pair<std::string_view, Metric>, 5> kMetricNames{{
    {"l2", Metric::kL2Squared},
    {"l2_squared", Metric::kL2Squared},
    {"euclidean", Metric::kL2Squared},
    {"cosine", Metric::kCosine},
    {"dot", Metric::kInnerProduct},
}};

constexpr std::array<std::pair<std::string_view, IndexType>, 4> kIndexNames{{
    {"brute_force", IndexType::kBruteForce},
    {"bruteforce", IndexType::kBruteForce},
    {"flat", IndexType::kBruteForce},
    {"hnsw", IndexType::kHnsw},
}};

}  // namespace

std::string_view metric_name(Metric metric) noexcept {
    switch (metric) {
        case Metric::kL2Squared:    return "l2";
        case Metric::kCosine:       return "cosine";
        case Metric::kInnerProduct: return "dot";
    }
    return "unknown";
}

bool parse_metric(std::string_view name, Metric& out) noexcept {
    for (const auto& [text, value] : kMetricNames) {
        if (text == name) {
            out = value;
            return true;
        }
    }
    return false;
}

std::string_view index_type_name(IndexType type) noexcept {
    switch (type) {
        case IndexType::kBruteForce: return "brute_force";
        case IndexType::kHnsw:       return "hnsw";
    }
    return "unknown";
}

bool parse_index_type(std::string_view name, IndexType& out) noexcept {
    for (const auto& [text, value] : kIndexNames) {
        if (text == name) {
            out = value;
            return true;
        }
    }
    return false;
}

}  // namespace vectordb
