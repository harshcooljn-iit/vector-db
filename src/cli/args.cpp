// SPDX-License-Identifier: MIT
#include "cli/args.hpp"

#include <algorithm>
#include <charconv>

#include <vectordb/core/error.hpp>

namespace vectordb::cli {
namespace {

/// Options that may appear more than once and are collected as key=value pairs.
bool is_repeatable(std::string_view name) {
    return name == "meta";
}

/// Short aliases. Kept tiny on purpose: a CLI with thirty single-letter flags
/// is a CLI nobody can read six months later.
std::string expand_short(std::string_view name) {
    if (name == "k")
        return "k";
    if (name == "d")
        return "dimension";
    if (name == "m")
        return "metric";
    if (name == "v")
        return "verbose";
    if (name == "q")
        return "query";
    if (name == "o")
        return "output";
    if (name == "i")
        return "input";
    return std::string(name);
}

}  // namespace

Args Args::parse(const std::vector<std::string_view>& argv) {
    Args args;

    for (std::size_t i = 0; i < argv.size(); ++i) {
        std::string_view token = argv[i];

        if (token.size() < 2 || token[0] != '-') {
            args.positionals_.emplace_back(token);
            continue;
        }

        // A lone "-" is a positional (conventionally stdin), not an option.
        if (token == "-") {
            args.positionals_.emplace_back(token);
            continue;
        }

        const bool long_form = token.size() > 2 && token[1] == '-';
        std::string_view body = long_form ? token.substr(2) : token.substr(1);

        std::string name;
        std::optional<std::string> inline_value;
        if (const std::size_t equals = body.find('='); equals != std::string_view::npos) {
            name = std::string(body.substr(0, equals));
            inline_value = std::string(body.substr(equals + 1));
        } else {
            name = std::string(body);
        }
        if (!long_form) {
            name = expand_short(name);
        }

        if (name.empty()) {
            throw InvalidArgumentError("malformed option '" + std::string(token) + "'");
        }

        std::string value;
        if (inline_value.has_value()) {
            value = *inline_value;
        } else if (i + 1 < argv.size() && !argv[i + 1].empty() &&
                   (argv[i + 1][0] != '-' || argv[i + 1].size() == 1 ||
                    (std::isdigit(static_cast<unsigned char>(argv[i + 1][1])) != 0) ||
                    argv[i + 1][1] == '.')) {
            // The next token is consumed as a value unless it looks like another
            // option. A negative number is a value, which is why the digit and
            // '.' checks are there — otherwise "--threshold -0.5" would break.
            value = std::string(argv[++i]);
        }

        if (is_repeatable(name)) {
            const std::size_t equals = value.find('=');
            if (equals == std::string::npos) {
                throw InvalidArgumentError("--" + name + " expects key=value, got '" + value +
                                           "'");
            }
            args.repeated_[name].emplace_back(value.substr(0, equals),
                                              value.substr(equals + 1));
        } else {
            args.options_[name] = value;
        }
    }

    return args;
}

std::optional<std::string> Args::positional(std::size_t index) const {
    if (index >= positionals_.size()) {
        return std::nullopt;
    }
    return positionals_[index];
}

std::string Args::require_positional(std::size_t index, std::string_view what) const {
    const std::optional<std::string> value = positional(index);
    if (!value.has_value()) {
        throw InvalidArgumentError("missing " + std::string(what));
    }
    return *value;
}

bool Args::has(std::string_view name) const {
    touched_.emplace_back(name);
    return options_.find(std::string(name)) != options_.end();
}

std::optional<std::string> Args::value(std::string_view name) const {
    touched_.emplace_back(name);
    const auto it = options_.find(std::string(name));
    if (it == options_.end() || it->second.empty()) {
        return std::nullopt;
    }
    return it->second;
}

std::string Args::require(std::string_view name) const {
    const std::optional<std::string> found = value(name);
    if (!found.has_value()) {
        throw InvalidArgumentError("--" + std::string(name) + " is required");
    }
    return *found;
}

std::uint64_t Args::number(std::string_view name, std::uint64_t fallback) const {
    const std::optional<std::string> text = value(name);
    if (!text.has_value()) {
        return fallback;
    }
    std::uint64_t parsed = 0;
    const char* begin = text->data();
    const char* end = begin + text->size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end) {
        throw InvalidArgumentError("--" + std::string(name) +
                                   " expects a non-negative integer, got '" + *text + "'");
    }
    return parsed;
}

std::uint64_t Args::require_number(std::string_view name) const {
    static_cast<void>(require(name));
    return number(name, 0);
}

const std::vector<std::pair<std::string, std::string>>& Args::repeated(
    std::string_view name) const {
    static const std::vector<std::pair<std::string, std::string>> kEmpty;
    touched_.emplace_back(name);
    const auto it = repeated_.find(std::string(name));
    return it == repeated_.end() ? kEmpty : it->second;
}

std::vector<std::string> Args::unused() const {
    std::vector<std::string> leftovers;
    for (const auto& [name, value] : options_) {
        if (std::find(touched_.begin(), touched_.end(), name) == touched_.end()) {
            leftovers.push_back(name);
        }
    }
    for (const auto& [name, values] : repeated_) {
        if (std::find(touched_.begin(), touched_.end(), name) == touched_.end()) {
            leftovers.push_back(name);
        }
    }
    return leftovers;
}

}  // namespace vectordb::cli
