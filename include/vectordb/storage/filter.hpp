// SPDX-License-Identifier: MIT
//
// A deliberately small filter language over metadata.
//
// Learning note: learnings/50-storage/04-sqlite.md
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <vectordb/storage/metadata.hpp>

namespace vectordb {

/// Comparison operators. Six, and no more.
///
/// This is not a SQL parser and is not becoming one. `docs/decisions/` records
/// the reasoning: the useful 95% of vector-search filtering is equality and
/// range predicates ANDed together, and every operator beyond that adds a
/// parser case, an evaluator case, an index consideration and a class of bug.
enum class FilterOp : std::uint8_t {
    kEqual,
    kNotEqual,
    kLess,
    kLessEqual,
    kGreater,
    kGreaterEqual,
};

[[nodiscard]] std::string_view filter_op_symbol(FilterOp op) noexcept;

/// A single `key <op> value` predicate.
struct FilterCondition {
    std::string key;
    FilterOp op = FilterOp::kEqual;
    MetadataValue value;

    /// Evaluates against one vector's metadata.
    ///
    /// A missing key never matches, including for `!=`. That is a real decision
    /// and the opposite one is defensible — SQL's three-valued logic would say
    /// `missing != "x"` is unknown, and a "does not have this key" filter is
    /// sometimes wanted. Never-match is chosen because it makes a filter's
    /// meaning independent of which keys happen to exist, which is far less
    /// surprising when metadata is sparse.
    [[nodiscard]] bool matches(const Metadata& metadata) const;
};

/// Conditions combined with AND.
///
/// No OR, no nesting, no negation of groups. `a AND b AND c` covers the cases
/// people actually write, and an empty filter matches everything so callers
/// need no null check.
class Filter {
public:
    Filter() = default;

    /// Parses a filter expression.
    ///
    /// Grammar, in full:
    ///
    /// ```
    /// filter     := condition ( "and" condition )*
    /// condition  := key op value
    /// op         := "==" | "!=" | ">=" | "<=" | ">" | "<" | "="
    /// value      := number | quoted-string | bare-word
    /// ```
    ///
    /// Values that parse as an integer or a float become numbers; anything else
    /// is text. Quote a value to force it to be text — `year == "2026"` matches
    /// the string, `year == 2026` matches the number.
    ///
    /// Example: `category == "finance" and year >= 2020 and score < 0.5`
    ///
    /// @throws InvalidArgumentError with the offending position on a syntax error
    [[nodiscard]] static Filter parse(std::string_view expression);

    void add(FilterCondition condition) { conditions_.push_back(std::move(condition)); }

    /// True when every condition holds. An empty filter matches everything.
    [[nodiscard]] bool matches(const Metadata& metadata) const;

    [[nodiscard]] bool empty() const noexcept { return conditions_.empty(); }

    [[nodiscard]] std::size_t size() const noexcept { return conditions_.size(); }

    [[nodiscard]] const std::vector<FilterCondition>& conditions() const noexcept {
        return conditions_;
    }

    /// Round-trippable rendering, used by `vectordb info` and error messages.
    [[nodiscard]] std::string to_string() const;

private:
    std::vector<FilterCondition> conditions_;
};

}  // namespace vectordb
