// SPDX-License-Identifier: MIT
//
// Logging policy.
//
// The library is a *library*: it must not print anything a caller did not ask
// for. So the default level is `warn` — problems are reported, routine work is
// silent — and an application raises it deliberately (`vectordb -v`).
//
// Learning note: learnings/90-debugging/01-how-to-debug-this-project.md
#pragma once

#include <string_view>

namespace vectordb {

enum class LogLevel {
    kTrace,
    kDebug,
    kInfo,
    kWarn,
    kError,
    kOff,
};

/// Sets the global level. Applies immediately to every subsequent log call.
void set_log_level(LogLevel level);

[[nodiscard]] LogLevel log_level() noexcept;

/// Parses "trace", "debug", "info", "warn", "error", "off".
[[nodiscard]] bool parse_log_level(std::string_view name, LogLevel& out) noexcept;

/// Sends log output to `path`, in addition to stderr.
/// @throws IoError if the file cannot be opened
void add_log_file(std::string_view path);

}  // namespace vectordb
