// SPDX-License-Identifier: MIT
//
// The `vectordb` command-line entry point.
//
// Deliberately thin: parse argv, dispatch, translate exceptions into exit
// codes. Every behaviour lives behind `vectordb::Database`, so anything the CLI
// can do is also reachable — and testable — from C++.
#include <exception>
#include <iostream>
#include <string_view>
#include <vector>

#include <vectordb/core/error.hpp>
#include <vectordb/core/logging.hpp>
#include <vectordb/core/version.hpp>
#include <vectordb/distance/kernel.hpp>

#include "cli/args.hpp"
#include "cli/commands.hpp"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitUsage = 2;

void print_usage(std::ostream& out) {
    out << R"(vectordb — a local-first vector database

usage: vectordb <command> [arguments] [options]

Database lifecycle
  create <db> --dimension D [--metric M] [--index I] [--normalize]
                            [--m M] [--ef-construction N] [--ef-search N] [--seed S]
  info <db>                 configuration and contents
  stats <db>                detailed counters, memory and file sizes
  check <db> [--deep]       validate structure; --deep also verifies checksums

Vectors
  insert <db> --vector "0.1,0.2,..." [--id N] [--meta key=value ...]
  get <db> <id> [--preview N]
  delete <db> <id>
  import <db> --input FILE [--csv] [--csv-ids] [--start-id N]
  export <db> --output FILE [--csv]

Search
  search <db> --query "0.1,0.2,..." [--k N] [--ef-search N]
              [--filter EXPR] [--exact] [--time]
  search <db> --query-file FILE [...]
  batch-search <db> --queries FILE [--k N] [--time]

Maintenance
  rebuild-index <db>        rebuild the ANN index from the stored vectors
  compact <db>              reclaim slots left by deleted vectors

Tools
  generate --output FILE --dimension D --count N [--kind K] [--seed S] [--csv]
  version                   version and build information
  help                      this message

Global options
  --json                    machine-readable output
  --quiet                   suppress progress messages
  --verbose                 log at info level (repeat as --log-level debug)
  --log-level LEVEL         trace | debug | info | warn | error | off
  --log-file PATH           also write logs to a file

Metrics:      l2 (default alias euclidean), cosine, dot
Index types:  hnsw (default), brute_force
Filters:      "category == \"finance\" and year >= 2020"   (== != < <= > >=, and only)

Exit codes:   0 success   1 error   2 usage   3 check found errors

Examples
  vectordb create demo.vdb --dimension 4 --metric cosine
  vectordb insert demo.vdb --vector "1,0,0,0" --meta name=first --meta year=2026
  vectordb search demo.vdb --query "1,0,0,0" --k 5 --time
  vectordb generate --output data.vecs --dimension 128 --count 10000
  vectordb import demo128.vdb --input data.vecs
)";
}

/// Applies the global options that must be set before anything else runs.
vectordb::cli::GlobalOptions configure(const vectordb::cli::Args& args) {
    vectordb::cli::GlobalOptions global;
    global.json = args.has("json");
    global.quiet = args.has("quiet");

    vectordb::LogLevel level = vectordb::LogLevel::kWarn;
    if (args.has("verbose")) {
        level = vectordb::LogLevel::kInfo;
    }
    if (const std::optional<std::string> named = args.value("log-level")) {
        if (!vectordb::parse_log_level(*named, level)) {
            throw vectordb::InvalidArgumentError(
                "unknown log level '" + *named +
                "'; expected trace, debug, info, warn, error or off");
        }
    }
    vectordb::set_log_level(level);

    if (const std::optional<std::string> path = args.value("log-file")) {
        vectordb::add_log_file(*path);
    }
    return global;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> raw(argv + 1, argv + argc);

    if (raw.empty()) {
        print_usage(std::cout);
        return kExitUsage;
    }

    const std::string_view command = raw.front();

    if (command == "help" || command == "--help" || command == "-h") {
        print_usage(std::cout);
        return kExitOk;
    }
    if (command == "version" || command == "--version" || command == "-V") {
        std::cout << vectordb::build_info()
                  << "  kernel:     " << vectordb::active_kernel().name << "\n";
        return kExitOk;
    }

    try {
        const std::vector<std::string_view> rest(raw.begin() + 1, raw.end());
        const vectordb::cli::Args args = vectordb::cli::Args::parse(rest);
        const vectordb::cli::GlobalOptions global = configure(args);

        using Handler = int (*)(
            const vectordb::cli::Args&, const vectordb::cli::GlobalOptions&, std::ostream&);
        const std::pair<std::string_view, Handler> table[] = {
            {"create", vectordb::cli::command_create},
            {"info", vectordb::cli::command_info},
            {"stats", vectordb::cli::command_stats},
            {"insert", vectordb::cli::command_insert},
            {"get", vectordb::cli::command_get},
            {"delete", vectordb::cli::command_delete},
            {"search", vectordb::cli::command_search},
            {"batch-search", vectordb::cli::command_batch_search},
            {"import", vectordb::cli::command_import},
            {"export", vectordb::cli::command_export},
            {"generate", vectordb::cli::command_generate},
            {"rebuild-index", vectordb::cli::command_rebuild_index},
            {"compact", vectordb::cli::command_compact},
            {"check", vectordb::cli::command_check},
        };

        for (const auto& [name, handler] : table) {
            if (name == command) {
                return handler(args, global, std::cout);
            }
        }

        std::cerr << "vectordb: unknown command '" << command << "'\n"
                  << "Run 'vectordb help' for the available commands.\n";
        return kExitUsage;

    } catch (const vectordb::InvalidArgumentError& error) {
        // A usage mistake, not a failure: distinct exit code so a script can
        // tell "I called it wrong" from "it went wrong".
        std::cerr << "vectordb: " << error.what() << "\n";
        return kExitUsage;
    } catch (const vectordb::Error& error) {
        std::cerr << "vectordb: " << error.what() << "\n";
        return kExitError;
    } catch (const std::exception& error) {
        std::cerr << "vectordb: unexpected failure: " << error.what() << "\n";
        return kExitError;
    }
}
