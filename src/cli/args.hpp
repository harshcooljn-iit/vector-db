// SPDX-License-Identifier: MIT
//
// A small argument parser.
//
// Deliberately hand-written rather than a dependency. The CLI takes flags,
// values and one or two positionals — perhaps 60 lines of real logic — and a
// parsing library would be a larger dependency than the thing it parses. What
// it does buy us is control over error messages, which is where a CLI is
// actually judged.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vectordb::cli {

/// Parsed `argv`: positionals in order, plus `--name value` and `--flag` pairs.
///
/// Accepts `--name value`, `--name=value` and `-n value` for known short forms.
/// A bare `--flag` is stored with an empty value and read by `has()`.
class Args {
public:
    /// @throws InvalidArgumentError on a malformed option
    static Args parse(const std::vector<std::string_view>& argv);

    [[nodiscard]] const std::vector<std::string>& positionals() const noexcept {
        return positionals_;
    }

    /// Positional at `index`, or nullopt.
    [[nodiscard]] std::optional<std::string> positional(std::size_t index) const;

    /// Positional at `index`, or an error naming what was expected.
    /// @throws InvalidArgumentError
    [[nodiscard]] std::string require_positional(std::size_t index,
                                                 std::string_view what) const;

    [[nodiscard]] bool has(std::string_view name) const;

    [[nodiscard]] std::optional<std::string> value(std::string_view name) const;

    /// @throws InvalidArgumentError if absent
    [[nodiscard]] std::string require(std::string_view name) const;

    /// @throws InvalidArgumentError if present but not a number
    [[nodiscard]] std::uint64_t number(std::string_view name, std::uint64_t fallback) const;

    [[nodiscard]] std::uint64_t require_number(std::string_view name) const;

    /// Every `--meta key=value` occurrence, in order.
    [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& repeated(
        std::string_view name) const;

    /// Options the command never looked at.
    ///
    /// A typo like `--dimensions 768` would otherwise be silently ignored and
    /// the database created with the wrong dimension. Commands call this and
    /// refuse to run — failing on an unknown flag is much kinder than
    /// succeeding differently from what was asked.
    [[nodiscard]] std::vector<std::string> unused() const;

private:
    std::map<std::string, std::string> options_;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> repeated_;
    std::vector<std::string> positionals_;
    mutable std::vector<std::string> touched_;
};

}  // namespace vectordb::cli
