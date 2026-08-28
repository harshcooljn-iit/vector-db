// SPDX-License-Identifier: MIT
//
// The `vectordb` command-line entry point.
//
// This file stays deliberately thin: it parses argv, dispatches to a command
// handler, and translates exceptions into exit codes. All real behaviour lives
// behind the application services in src/db/, so that everything the CLI can
// do is also reachable (and testable) from C++.
#include <cstdio>
#include <exception>
#include <string_view>
#include <vector>

#include <vectordb/core/version.hpp>

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitError = 1;

void print_usage() {
    std::fputs(
        "vectordb — a local-first vector database\n"
        "\n"
        "usage: vectordb <command> [options]\n"
        "\n"
        "commands:\n"
        "  version              print version and build information\n"
        "  help                 show this message\n"
        "\n",
        stdout);
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args(argv + 1, argv + argc);

    if (args.empty()) {
        print_usage();
        return kExitUsage;
    }

    const std::string_view command = args.front();

    try {
        if (command == "version" || command == "--version" || command == "-V") {
            std::fputs(vectordb::build_info().c_str(), stdout);
            return kExitOk;
        }
        if (command == "help" || command == "--help" || command == "-h") {
            print_usage();
            return kExitOk;
        }

        std::fprintf(stderr, "vectordb: unknown command '%.*s'\n",
                     static_cast<int>(command.size()), command.data());
        print_usage();
        return kExitUsage;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "vectordb: unexpected failure: %s\n", error.what());
        return kExitError;
    }
}
