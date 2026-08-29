// SPDX-License-Identifier: MIT
#include <array>
#include <memory>
#include <utility>
#include <vector>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <vectordb/core/error.hpp>
#include <vectordb/core/logging.hpp>

namespace vectordb {
namespace {

spdlog::level::level_enum to_spdlog(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::kTrace:
            return spdlog::level::trace;
        case LogLevel::kDebug:
            return spdlog::level::debug;
        case LogLevel::kInfo:
            return spdlog::level::info;
        case LogLevel::kWarn:
            return spdlog::level::warn;
        case LogLevel::kError:
            return spdlog::level::err;
        case LogLevel::kOff:
            return spdlog::level::off;
    }
    return spdlog::level::warn;
}

/// Applies the library's default the first time anything logs.
///
/// A function-local static, so it runs on first use rather than during static
/// initialisation — where the order relative to spdlog's own globals would be
/// unspecified.
struct DefaultLevel {
    DefaultLevel() { spdlog::set_level(spdlog::level::warn); }
};

LogLevel& current_level() noexcept {
    static DefaultLevel initialiser;
    static LogLevel level = LogLevel::kWarn;
    (void)initialiser;
    return level;
}

constexpr std::array<std::pair<std::string_view, LogLevel>, 7> kLevelNames{{
    {"trace", LogLevel::kTrace},
    {"debug", LogLevel::kDebug},
    {"info", LogLevel::kInfo},
    {"warn", LogLevel::kWarn},
    {"warning", LogLevel::kWarn},
    {"error", LogLevel::kError},
    {"off", LogLevel::kOff},
}};

}  // namespace

void set_log_level(LogLevel level) {
    current_level() = level;
    spdlog::set_level(to_spdlog(level));
}

LogLevel log_level() noexcept {
    return current_level();
}

bool parse_log_level(std::string_view name, LogLevel& out) noexcept {
    for (const auto& [text, value] : kLevelNames) {
        if (text == name) {
            out = value;
            return true;
        }
    }
    return false;
}

void add_log_file(std::string_view path) {
    try {
        auto file_sink =
            std::make_shared<spdlog::sinks::basic_file_sink_mt>(std::string(path), false);
        auto console_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
        auto logger = std::make_shared<spdlog::logger>(
            "vectordb", spdlog::sinks_init_list{console_sink, file_sink});
        logger->set_level(to_spdlog(current_level()));
        spdlog::set_default_logger(std::move(logger));
    } catch (const spdlog::spdlog_ex& error) {
        throw IoError("cannot open log file '" + std::string(path) + "': " + error.what());
    }
}

}  // namespace vectordb
