// SPDX-License-Identifier: MIT
#include "cli/commands.hpp"

#include <chrono>
#include <iomanip>

#include <vectordb/core/error.hpp>
#include <vectordb/db/database.hpp>
#include <vectordb/util/dataset.hpp>
#include <vectordb/util/vector_io.hpp>

#include "cli/output.hpp"

namespace vectordb::cli {
namespace {

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitCheckFailed = 3;

/// Refuses to run if the command never looked at an option the user passed.
///
/// `--dimensions 768` is one keystroke from `--dimension 768`, and silently
/// ignoring it would create a database with the default dimension. Failing on
/// an unrecognised flag is much kinder than succeeding differently.
void reject_unknown_options(const Args& args) {
    const std::vector<std::string> leftovers = args.unused();
    if (leftovers.empty()) {
        return;
    }
    std::string message = "unrecognised option";
    message += leftovers.size() > 1 ? "s: " : ": ";
    for (std::size_t i = 0; i < leftovers.size(); ++i) {
        message += (i > 0 ? ", --" : "--") + leftovers[i];
    }
    message += "\nRun 'vectordb help' for the available options.";
    throw InvalidArgumentError(message);
}

Database open_database(const Args& args) {
    return Database::open(args.require_positional(0, "a database path"));
}

/// Reads a query vector from `--query "1,2,3"` or `--query-file path`.
std::vector<float> read_query(const Args& args) {
    const std::optional<std::string> literal = args.value("query");
    const std::optional<std::string> file = args.value("query-file");

    if (literal.has_value() && file.has_value()) {
        throw InvalidArgumentError("pass either --query or --query-file, not both");
    }
    if (literal.has_value()) {
        return parse_vector_literal(*literal);
    }
    if (file.has_value()) {
        const VectorBatch batch = read_vector_matrix(*file);
        if (batch.vectors.empty()) {
            throw InvalidArgumentError("'" + *file + "' contains no vectors");
        }
        const VectorView first = batch.vectors[0];
        return {first.begin(), first.end()};
    }
    throw InvalidArgumentError(
        "a query is required: --query \"0.1,0.2,0.3\" or --query-file query.vecs");
}

Metadata read_metadata(const Args& args) {
    Metadata metadata;
    for (const auto& [key, text] : args.repeated("meta")) {
        // Typed by shape: an integer stays an integer, so `year >= 2020` works
        // later. Quote it (`--meta year="2020"`) to force text — the same rule
        // the filter language uses, so the two cannot disagree.
        if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
            metadata[key] = text.substr(1, text.size() - 2);
            continue;
        }
        try {
            std::size_t consumed = 0;
            const long long integer = std::stoll(text, &consumed);
            if (consumed == text.size()) {
                metadata[key] = static_cast<std::int64_t>(integer);
                continue;
            }
        } catch (const std::exception&) {
        }
        try {
            std::size_t consumed = 0;
            const double real = std::stod(text, &consumed);
            if (consumed == text.size()) {
                metadata[key] = real;
                continue;
            }
        } catch (const std::exception&) {
        }
        metadata[key] = text;
    }
    return metadata;
}

QueryOptions read_query_options(const Args& args) {
    QueryOptions options;
    options.k = args.number("k", 10);
    options.ef_search = args.number("ef-search", 0);
    options.exact = args.has("exact");
    options.filter_oversample = args.number("oversample", 0);
    if (const std::optional<std::string> filter = args.value("filter")) {
        options.filter = Filter::parse(*filter);
    }
    return options;
}

double elapsed_ms(std::chrono::steady_clock::time_point start) {
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(now - start).count();
}

}  // namespace

// ---------------------------------------------------------------------------

int command_create(const Args& args, const GlobalOptions& global, std::ostream& out) {
    const std::string path = args.require_positional(0, "a database path");

    DatabaseConfig config;
    config.dimension = static_cast<Dimension>(args.require_number("dimension"));

    if (const std::optional<std::string> metric = args.value("metric")) {
        if (!parse_metric(*metric, config.metric)) {
            throw InvalidArgumentError("unknown metric '" + *metric +
                                       "'; expected l2, cosine or dot");
        }
    }
    if (const std::optional<std::string> index = args.value("index")) {
        if (!parse_index_type(*index, config.index_type)) {
            throw InvalidArgumentError("unknown index type '" + *index +
                                       "'; expected hnsw or brute_force");
        }
    }
    config.normalize = args.has("normalize");
    config.hnsw.m = args.number("m", config.hnsw.m);
    config.hnsw.ef_construction = args.number("ef-construction", config.hnsw.ef_construction);
    config.hnsw.ef_search = args.number("ef-search", config.hnsw.ef_search);
    config.hnsw.seed = args.number("seed", config.hnsw.seed);

    reject_unknown_options(args);

    const Database database = Database::create(path, config);

    if (global.json) {
        print_info(out, database, true);
    } else if (!global.quiet) {
        out << "Created database '" << path << "'\n";
        print_info(out, database, false);
    }
    return kExitOk;
}

int command_info(const Args& args, const GlobalOptions& global, std::ostream& out) {
    const Database database = open_database(args);
    reject_unknown_options(args);
    print_info(out, database, global.json);
    return kExitOk;
}

int command_stats(const Args& args, const GlobalOptions& global, std::ostream& out) {
    const Database database = open_database(args);
    reject_unknown_options(args);
    print_stats(out, database.stats(), global.json);
    return kExitOk;
}

int command_insert(const Args& args, const GlobalOptions& global, std::ostream& out) {
    Database database = open_database(args);

    const std::vector<float> vector = parse_vector_literal(args.require("vector"));
    const Metadata metadata = read_metadata(args);
    const bool has_id = args.has("id");
    const std::uint64_t id = args.number("id", 0);
    reject_unknown_options(args);

    VectorId assigned = 0;
    if (has_id) {
        assigned = id;
        database.insert(assigned, vector, metadata);
    } else {
        assigned = database.insert(vector, metadata);
    }
    database.flush();

    if (global.json) {
        out << "{\"inserted\": 1, \"id\": " << assigned << "}\n";
    } else if (!global.quiet) {
        out << "Inserted vector with id " << assigned << ".\n";
    }
    return kExitOk;
}

int command_get(const Args& args, const GlobalOptions& global, std::ostream& out) {
    const Database database = open_database(args);
    const std::string id_text = args.require_positional(1, "a vector id");
    const std::size_t preview = args.number("preview", 8);
    reject_unknown_options(args);

    VectorId id = 0;
    try {
        id = std::stoull(id_text);
    } catch (const std::exception&) {
        throw InvalidArgumentError("'" + id_text + "' is not a valid vector id");
    }

    const std::optional<StoredVector> stored = database.get(id);
    if (!stored.has_value()) {
        throw NotFoundError("no vector with id " + std::to_string(id));
    }

    print_stored_vector(out, *stored, global.json, preview);
    return kExitOk;
}

int command_delete(const Args& args, const GlobalOptions& global, std::ostream& out) {
    Database database = open_database(args);
    const std::string id_text = args.require_positional(1, "a vector id");
    reject_unknown_options(args);

    VectorId id = 0;
    try {
        id = std::stoull(id_text);
    } catch (const std::exception&) {
        throw InvalidArgumentError("'" + id_text + "' is not a valid vector id");
    }

    const bool removed = database.remove(id);
    database.flush();

    if (global.json) {
        out << "{\"deleted\": " << (removed ? 1 : 0) << ", \"id\": " << id << "}\n";
    } else if (!global.quiet) {
        out << (removed ? "Deleted vector " : "No vector with id ") << id << ".\n";
    }
    return removed ? kExitOk : kExitError;
}

int command_search(const Args& args, const GlobalOptions& global, std::ostream& out) {
    const Database database = open_database(args);
    const std::vector<float> query = read_query(args);
    QueryOptions options = read_query_options(args);
    const bool timing = args.has("time");
    reject_unknown_options(args);

    const auto start = std::chrono::steady_clock::now();
    const std::vector<QueryResult> results = database.search(query, options);
    const double duration = elapsed_ms(start);

    print_results(out,
                  results,
                  database.config().metric,
                  global.json,
                  /*show_metadata=*/true);

    if (timing && !global.json) {
        out << "\n" << std::fixed << std::setprecision(3) << duration << " ms\n";
    }
    return kExitOk;
}

int command_batch_search(const Args& args, const GlobalOptions& global, std::ostream& out) {
    const Database database = open_database(args);
    const std::string path = args.require("queries");
    QueryOptions options = read_query_options(args);
    const bool timing = args.has("time");
    reject_unknown_options(args);

    const VectorBatch batch = read_vector_matrix(path);
    const auto start = std::chrono::steady_clock::now();
    const auto results = database.batch_search(batch.vectors, options);
    const double duration = elapsed_ms(start);

    print_batch_results(out, results, database.config().metric, global.json);

    if (timing && !global.json) {
        out << std::fixed << std::setprecision(3) << duration << " ms for "
            << batch.vectors.size() << " queries ("
            << (duration / static_cast<double>(batch.vectors.size())) << " ms each)\n";
    }
    return kExitOk;
}

int command_import(const Args& args, const GlobalOptions& global, std::ostream& out) {
    Database database = open_database(args);
    const std::string path = args.require("input");
    const bool csv = args.has("csv");
    const bool csv_ids = args.has("csv-ids");
    const std::uint64_t start_id = args.number("start-id", 0);
    const bool has_start_id = args.has("start-id");
    reject_unknown_options(args);

    const VectorBatch batch = csv ? read_vector_csv(path, csv_ids) : read_vector_matrix(path);

    if (batch.vectors.dimension() != database.config().dimension) {
        throw DimensionMismatchError(database.config().dimension, batch.vectors.dimension());
    }

    // One allocation instead of a logarithmic number of copies, and one SQLite
    // transaction instead of a durability round trip per row.
    database.reserve(database.size() + batch.vectors.size());

    const auto start = std::chrono::steady_clock::now();
    for (LocalId i = 0; i < batch.vectors.size(); ++i) {
        if (batch.has_ids() && !has_start_id) {
            database.insert(batch.ids[i], batch.vectors[i]);
        } else if (has_start_id) {
            database.insert(start_id + i, batch.vectors[i]);
        } else {
            static_cast<void>(database.insert(batch.vectors[i]));
        }
    }
    database.flush();
    const double duration = elapsed_ms(start);

    if (global.json) {
        out << "{\"imported\": " << batch.vectors.size() << ", \"total\": " << database.size()
            << ", \"ms\": " << duration << "}\n";
    } else if (!global.quiet) {
        out << "Imported " << group_digits(batch.vectors.size()) << " vectors in " << std::fixed
            << std::setprecision(1) << duration << " ms ("
            << group_digits(static_cast<std::uint64_t>(
                   static_cast<double>(batch.vectors.size()) / (duration / 1000.0)))
            << " vectors/second).\n"
            << "Database now holds " << group_digits(database.size()) << " vectors.\n";
    }
    return kExitOk;
}

int command_export(const Args& args, const GlobalOptions& global, std::ostream& out) {
    const Database database = open_database(args);
    const std::string path = args.require("output");
    const bool csv = args.has("csv");
    reject_unknown_options(args);

    VectorArray vectors(database.config().dimension);
    std::vector<VectorId> ids;
    vectors.reserve(database.size());
    ids.reserve(database.size());

    // Live vectors only, in id order rather than slot order, so the output
    // carries no trace of tombstones or of our internal layout.
    for (const VectorId id : database.all_ids()) {
        if (const std::optional<StoredVector> stored = database.get(id)) {
            vectors.push_back(stored->vector);
            ids.push_back(id);
        }
    }

    if (csv) {
        write_vector_csv(path, vectors, ids);
    } else {
        write_vector_matrix(path, vectors, ids);
    }

    if (global.json) {
        out << "{\"exported\": " << vectors.size() << "}\n";
    } else if (!global.quiet) {
        out << "Exported " << group_digits(vectors.size()) << " vectors to '" << path << "'.\n";
    }
    return kExitOk;
}

int command_generate(const Args& args, const GlobalOptions& global, std::ostream& out) {
    DatasetSpec spec;
    spec.dimension = static_cast<Dimension>(args.require_number("dimension"));
    spec.count = args.require_number("count");
    spec.seed = args.number("seed", 42);
    spec.sample_seed = args.number("sample-seed", spec.seed);
    spec.cluster_count = args.number("clusters", 0);

    if (const std::optional<std::string> kind = args.value("kind")) {
        if (*kind == "uniform") {
            spec.kind = DatasetKind::kUniform;
        } else if (*kind == "gaussian") {
            spec.kind = DatasetKind::kGaussian;
        } else if (*kind == "clustered") {
            spec.kind = DatasetKind::kClustered;
        } else {
            throw InvalidArgumentError("unknown dataset kind '" + *kind +
                                       "'; expected clustered, gaussian or uniform");
        }
    }

    const std::string path = args.require("output");
    const bool csv = args.has("csv");
    reject_unknown_options(args);

    const VectorArray vectors = generate_dataset(spec);
    if (csv) {
        write_vector_csv(path, vectors);
    } else {
        write_vector_matrix(path, vectors);
    }

    if (global.json) {
        out << "{\"generated\": " << vectors.size() << ", \"dimension\": " << spec.dimension
            << ", \"seed\": " << spec.seed << "}\n";
    } else if (!global.quiet) {
        out << "Wrote " << group_digits(vectors.size()) << " vectors of dimension "
            << spec.dimension << " to '" << path << "' (seed " << spec.seed << ").\n";
    }
    return kExitOk;
}

int command_rebuild_index(const Args& args, const GlobalOptions& global, std::ostream& out) {
    Database database = open_database(args);
    reject_unknown_options(args);

    const auto start = std::chrono::steady_clock::now();
    database.rebuild_index();
    const double duration = elapsed_ms(start);

    if (global.json) {
        out << "{\"rebuilt\": true, \"vectors\": " << database.size()
            << ", \"ms\": " << duration << "}\n";
    } else if (!global.quiet) {
        out << "Rebuilt the index over " << group_digits(database.size()) << " vectors in "
            << std::fixed << std::setprecision(1) << duration << " ms.\n";
    }
    return kExitOk;
}

int command_compact(const Args& args, const GlobalOptions& global, std::ostream& out) {
    Database database = open_database(args);
    reject_unknown_options(args);

    const auto start = std::chrono::steady_clock::now();
    const std::size_t reclaimed = database.compact();
    const double duration = elapsed_ms(start);

    if (global.json) {
        out << "{\"reclaimed\": " << reclaimed << ", \"ms\": " << duration << "}\n";
    } else if (!global.quiet) {
        if (reclaimed == 0) {
            out << "Nothing to compact.\n";
        } else {
            out << "Reclaimed " << group_digits(reclaimed) << " deleted slots in " << std::fixed
                << std::setprecision(1) << duration << " ms.\n";
        }
    }
    return kExitOk;
}

int command_check(const Args& args, const GlobalOptions& global, std::ostream& out) {
    const Database database = open_database(args);
    const bool deep = args.has("deep");
    reject_unknown_options(args);

    const CheckReport report = database.check(deep);
    print_check(out, report, global.json);

    // A distinct exit code, so a script can tell "the check ran and found
    // problems" from "the command itself failed".
    return report.error_count() > 0 ? kExitCheckFailed : kExitOk;
}

}  // namespace vectordb::cli
