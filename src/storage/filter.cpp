// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cctype>
#include <charconv>

#include <vectordb/core/error.hpp>
#include <vectordb/storage/filter.hpp>

namespace vectordb {
namespace {

/// Compares two metadata values, returning a three-way result.
///
/// Cross-type comparison is numeric where both sides are numeric (so
/// `year >= 2020` works whether the stored value is an integer or a real), and
/// otherwise never matches. Comparing a number with a string has no defensible
/// answer, and inventing one — SQL sorts them into type classes — would make
/// filters behave in ways nobody predicts.
std::optional<int> compare_values(const MetadataValue& stored, const MetadataValue& wanted) {
    const bool stored_numeric =
        std::holds_alternative<std::int64_t>(stored) || std::holds_alternative<double>(stored);
    const bool wanted_numeric =
        std::holds_alternative<std::int64_t>(wanted) || std::holds_alternative<double>(wanted);

    if (stored_numeric && wanted_numeric) {
        const double a = std::holds_alternative<std::int64_t>(stored)
                             ? static_cast<double>(std::get<std::int64_t>(stored))
                             : std::get<double>(stored);
        const double b = std::holds_alternative<std::int64_t>(wanted)
                             ? static_cast<double>(std::get<std::int64_t>(wanted))
                             : std::get<double>(wanted);
        if (a < b) {
            return -1;
        }
        return a > b ? 1 : 0;
    }

    if (std::holds_alternative<std::string>(stored) &&
        std::holds_alternative<std::string>(wanted)) {
        const int result = std::get<std::string>(stored).compare(std::get<std::string>(wanted));
        return result < 0 ? -1 : (result > 0 ? 1 : 0);
    }

    if (std::holds_alternative<std::monostate>(stored) &&
        std::holds_alternative<std::monostate>(wanted)) {
        return 0;
    }

    return std::nullopt;  // incomparable types
}

bool is_space(char c) noexcept {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

/// Turns a token into a number if it looks like one, otherwise text.
MetadataValue parse_value(std::string_view token, bool was_quoted) {
    if (was_quoted) {
        return std::string(token);
    }

    std::int64_t integer = 0;
    const auto* end = token.data() + token.size();
    auto result = std::from_chars(token.data(), end, integer);
    if (result.ec == std::errc{} && result.ptr == end) {
        return integer;
    }

    // from_chars for floating point is not available in every standard library
    // shipped as of writing, so this uses strtod with an explicit full-consume
    // check rather than trusting a partial parse.
    const std::string text(token);
    char* stop = nullptr;
    const double real = std::strtod(text.c_str(), &stop);
    if (stop != nullptr && *stop == '\0' && !text.empty()) {
        return real;
    }

    return text;
}

}  // namespace

std::string_view filter_op_symbol(FilterOp op) noexcept {
    switch (op) {
        case FilterOp::kEqual:
            return "==";
        case FilterOp::kNotEqual:
            return "!=";
        case FilterOp::kLess:
            return "<";
        case FilterOp::kLessEqual:
            return "<=";
        case FilterOp::kGreater:
            return ">";
        case FilterOp::kGreaterEqual:
            return ">=";
    }
    return "?";
}

bool FilterCondition::matches(const Metadata& metadata) const {
    const auto it = metadata.find(key);
    if (it == metadata.end()) {
        // A missing key never matches, `!=` included. See the header for why.
        return false;
    }

    const std::optional<int> order = compare_values(it->second, value);
    if (!order.has_value()) {
        return false;  // incomparable types never match
    }

    switch (op) {
        case FilterOp::kEqual:
            return *order == 0;
        case FilterOp::kNotEqual:
            return *order != 0;
        case FilterOp::kLess:
            return *order < 0;
        case FilterOp::kLessEqual:
            return *order <= 0;
        case FilterOp::kGreater:
            return *order > 0;
        case FilterOp::kGreaterEqual:
            return *order >= 0;
    }
    return false;
}

bool Filter::matches(const Metadata& metadata) const {
    return std::all_of(conditions_.begin(), conditions_.end(), [&](const FilterCondition& c) {
        return c.matches(metadata);
    });
}

std::string Filter::to_string() const {
    std::string text;
    for (std::size_t i = 0; i < conditions_.size(); ++i) {
        if (i > 0) {
            text += " and ";
        }
        text += conditions_[i].key;
        text += ' ';
        text += filter_op_symbol(conditions_[i].op);
        text += ' ';
        if (std::holds_alternative<std::string>(conditions_[i].value)) {
            text += '"';
            text += std::get<std::string>(conditions_[i].value);
            text += '"';
        } else {
            text += to_display_string(conditions_[i].value);
        }
    }
    return text;
}

Filter Filter::parse(std::string_view expression) {
    Filter filter;
    std::size_t pos = 0;

    const auto skip_spaces = [&] {
        while (pos < expression.size() && is_space(expression[pos])) {
            ++pos;
        }
    };

    const auto fail = [&](std::string_view what) {
        throw InvalidArgumentError("cannot parse filter at position " + std::to_string(pos) +
                                   ": " + std::string(what) + "\n  " + std::string(expression) +
                                   "\n  " + std::string(pos, ' ') + "^");
    };

    skip_spaces();
    if (pos >= expression.size()) {
        return filter;  // an empty filter matches everything
    }

    while (true) {
        skip_spaces();

        // --- key ---
        const std::size_t key_start = pos;
        while (pos < expression.size() &&
               (std::isalnum(static_cast<unsigned char>(expression[pos])) ||
                expression[pos] == '_' || expression[pos] == '.' || expression[pos] == '-')) {
            ++pos;
        }
        if (pos == key_start) {
            fail("expected a metadata key");
        }
        const std::string key(expression.substr(key_start, pos - key_start));

        // --- operator ---
        skip_spaces();
        FilterOp op{};
        if (expression.compare(pos, 2, "==") == 0) {
            op = FilterOp::kEqual;
            pos += 2;
        } else if (expression.compare(pos, 2, "!=") == 0) {
            op = FilterOp::kNotEqual;
            pos += 2;
        } else if (expression.compare(pos, 2, ">=") == 0) {
            op = FilterOp::kGreaterEqual;
            pos += 2;
        } else if (expression.compare(pos, 2, "<=") == 0) {
            op = FilterOp::kLessEqual;
            pos += 2;
        } else if (pos < expression.size() && expression[pos] == '>') {
            op = FilterOp::kGreater;
            pos += 1;
        } else if (pos < expression.size() && expression[pos] == '<') {
            op = FilterOp::kLess;
            pos += 1;
        } else if (pos < expression.size() && expression[pos] == '=') {
            // A lone '=' is accepted as '=='. Rejecting it would be defensible,
            // but it is the single most common typo and its intent is obvious.
            op = FilterOp::kEqual;
            pos += 1;
        } else {
            fail("expected one of ==, !=, <, <=, >, >=");
        }

        // --- value ---
        skip_spaces();
        if (pos >= expression.size()) {
            fail("expected a value after the operator");
        }

        bool quoted = false;
        std::string token;
        if (expression[pos] == '"' || expression[pos] == '\'') {
            const char quote = expression[pos];
            quoted = true;
            ++pos;
            while (pos < expression.size() && expression[pos] != quote) {
                if (expression[pos] == '\\' && pos + 1 < expression.size()) {
                    ++pos;  // take the next character literally
                }
                token.push_back(expression[pos]);
                ++pos;
            }
            if (pos >= expression.size()) {
                fail("unterminated quoted value");
            }
            ++pos;  // closing quote
        } else {
            const std::size_t value_start = pos;
            while (pos < expression.size() && !is_space(expression[pos])) {
                ++pos;
            }
            token = std::string(expression.substr(value_start, pos - value_start));
            if (token.empty()) {
                fail("expected a value after the operator");
            }
        }

        filter.add(FilterCondition{key, op, parse_value(token, quoted)});

        // --- and / end ---
        skip_spaces();
        if (pos >= expression.size()) {
            break;
        }

        std::string keyword;
        const std::size_t keyword_start = pos;
        while (pos < expression.size() &&
               std::isalpha(static_cast<unsigned char>(expression[pos]))) {
            keyword.push_back(
                static_cast<char>(std::tolower(static_cast<unsigned char>(expression[pos]))));
            ++pos;
        }
        if (keyword != "and") {
            pos = keyword_start;
            fail(
                "expected 'and' or the end of the filter (there is no 'or' — see "
                "docs/metadata.md)");
        }
    }

    return filter;
}

}  // namespace vectordb
