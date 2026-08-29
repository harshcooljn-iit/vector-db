// SPDX-License-Identifier: MIT
//
// The benchmark driver.
//
// Numbers produced here are the only ones allowed into docs/benchmark-results.md.
// Every run prints its provenance — build type, compiler, kernel, parameters —
// because a timing without the build that produced it is not a measurement.
//
// See docs/benchmarking.md for the methodology.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <unordered_set>
#include <vector>

#include <vectordb/concurrency/thread_pool.hpp>
#include <vectordb/core/version.hpp>
#include <vectordb/db/concurrent_database.hpp>
#include <vectordb/db/database.hpp>
#include <vectordb/index/brute_force_index.hpp>
#include <vectordb/index/hnsw_index.hpp>
#include <vectordb/storage/vector_store.hpp>
#include <vectordb/util/dataset.hpp>

namespace {

using vectordb::Candidate;
using vectordb::DatasetSpec;
using vectordb::Dimension;
using vectordb::LocalId;
using vectordb::Metric;
using vectordb::SearchParams;
using vectordb::VectorArray;
using vectordb::VectorStore;

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

/// Latency percentiles, in microseconds.
struct Latency {
    double p50 = 0;
    double p95 = 0;
    double p99 = 0;
    double mean = 0;

    static Latency from(std::vector<double> samples_us) {
        std::sort(samples_us.begin(), samples_us.end());
        Latency latency;
        if (samples_us.empty()) {
            return latency;
        }
        const auto at = [&](double q) {
            const auto index =
                static_cast<std::size_t>(q * static_cast<double>(samples_us.size() - 1));
            return samples_us[index];
        };
        latency.p50 = at(0.50);
        latency.p95 = at(0.95);
        latency.p99 = at(0.99);
        latency.mean = std::accumulate(samples_us.begin(), samples_us.end(), 0.0) /
                       static_cast<double>(samples_us.size());
        return latency;
    }
};

void print_header(std::string_view title) {
    std::cout << "\n" << title << "\n" << std::string(title.size(), '-') << "\n";
}

/// Runs `body` until at least `min_seconds` have elapsed, returning the
/// per-iteration time.
///
/// A fixed iteration count either wastes time on a fast case or produces noise
/// on a slow one; a fixed *duration* adapts.
double time_per_iteration(double min_seconds, const std::function<void()>& body) {
    // Warm up: the first iteration pays for cold caches, lazy page faults and
    // any one-time initialisation, none of which are what we are measuring.
    body();

    std::size_t iterations = 0;
    const auto start = Clock::now();
    while (seconds_since(start) < min_seconds) {
        body();
        ++iterations;
    }
    return seconds_since(start) / static_cast<double>(iterations);
}

// ---------------------------------------------------------------------------

void benchmark_distance_kernels() {
    print_header("Distance kernels");

    std::cout << std::left << std::setw(10) << "kernel" << std::setw(8) << "dim"
              << std::setw(14) << "ns/call" << std::setw(14) << "GB/s"
              << "speedup\n";

    for (const Dimension dimension : {128U, 384U, 768U, 1536U}) {
        const VectorArray data =
            vectordb::generate_dataset({.dimension = dimension, .count = 2, .seed = 1});
        const float* a = data[0].data();
        const float* b = data[1].data();

        double scalar_ns = 0;
        for (const vectordb::DistanceKernel* kernel : vectordb::available_kernels()) {
            // A volatile sink so the compiler cannot delete the whole loop as
            // dead code — the classic way to benchmark nothing at all.
            volatile float sink = 0.0F;
            constexpr int kBatch = 1000;

            const double per_batch = time_per_iteration(0.4, [&] {
                float total = 0.0F;
                for (int i = 0; i < kBatch; ++i) {
                    total += kernel->l2_squared(a, b, dimension);
                }
                sink = total;
            });
            static_cast<void>(sink);

            const double ns = per_batch * 1e9 / kBatch;
            if (kernel->name == "scalar") {
                scalar_ns = ns;
            }
            const double bytes = 2.0 * dimension * sizeof(float);

            std::cout << std::left << std::setw(10) << kernel->name << std::setw(8) << dimension
                      << std::setw(14) << std::fixed << std::setprecision(1) << ns
                      << std::setw(14) << std::setprecision(2) << (bytes / ns)
                      << std::setprecision(2) << (scalar_ns / ns) << "x\n";
        }
    }
}

void benchmark_brute_force(Dimension dimension, std::size_t count) {
    const DatasetSpec spec{.dimension = dimension, .count = count, .seed = 20260829};
    const VectorArray data = vectordb::generate_dataset(spec);
    const VectorArray queries = vectordb::generate_queries(spec, 100);

    VectorStore store(dimension);
    store.reserve(count);
    for (LocalId i = 0; i < data.size(); ++i) {
        store.insert(i, data[i]);
    }

    const vectordb::BruteForceIndex index(store.accessor(), Metric::kL2Squared);

    std::vector<double> samples;
    samples.reserve(queries.size());
    for (LocalId q = 0; q < queries.size(); ++q) {
        const auto start = Clock::now();
        const auto results = index.search(queries[q], SearchParams{.k = 10});
        samples.push_back(seconds_since(start) * 1e6);
        if (results.empty()) {
            std::cerr << "unexpected empty result\n";
        }
    }

    const Latency latency = Latency::from(samples);
    const double bytes = static_cast<double>(count) * dimension * sizeof(float);

    std::cout << std::left << std::setw(10) << dimension << std::setw(12) << count << std::fixed
              << std::setprecision(1) << std::setw(12) << latency.p50 << std::setw(12)
              << latency.p95 << std::setw(12) << std::setprecision(2)
              << (bytes / (latency.p50 * 1000.0)) << "\n";
}

void benchmark_hnsw(Dimension dimension, std::size_t count) {
    const DatasetSpec spec{.dimension = dimension, .count = count, .seed = 20260829};
    const VectorArray data = vectordb::generate_dataset(spec);
    const VectorArray queries = vectordb::generate_queries(spec, 200);

    VectorStore store(dimension);
    store.reserve(count);
    for (LocalId i = 0; i < data.size(); ++i) {
        store.insert(i, data[i]);
    }

    const vectordb::HnswConfig config{.m = 16, .ef_construction = 200};

    const auto build_start = Clock::now();
    vectordb::HnswIndex index(store.accessor(), Metric::kL2Squared, config);
    index.reserve(count);
    for (LocalId i = 0; i < data.size(); ++i) {
        index.add(i);
    }
    const double build_seconds = seconds_since(build_start);

    const vectordb::BruteForceIndex oracle(store.accessor(), Metric::kL2Squared);

    std::cout << "\n  N=" << count << " D=" << dimension << " M=" << config.m
              << " efConstruction=" << config.ef_construction << "\n"
              << "  build: " << std::fixed << std::setprecision(2) << build_seconds << " s ("
              << std::setprecision(0) << (static_cast<double>(count) / build_seconds)
              << " vectors/s), "
              << "index " << (index.index_bytes() / 1024 / 1024) << " MB ("
              << (index.index_bytes() / count) << " bytes/vector), " << (index.max_level() + 1)
              << " layers\n\n";

    std::cout << "  " << std::left << std::setw(12) << "efSearch" << std::setw(14) << "p50 (us)"
              << std::setw(14) << "p95 (us)" << std::setw(14) << "recall@10" << "speedup\n";

    // Brute force on the same data, for the speedup column.
    std::vector<double> exact_samples;
    for (LocalId q = 0; q < 50; ++q) {
        const auto start = Clock::now();
        static_cast<void>(oracle.search(queries[q], SearchParams{.k = 10}));
        exact_samples.push_back(seconds_since(start) * 1e6);
    }
    const double exact_p50 = Latency::from(exact_samples).p50;

    for (const std::size_t ef : {10U, 20U, 50U, 100U, 200U, 400U}) {
        std::vector<double> samples;
        double recall_total = 0.0;

        for (LocalId q = 0; q < queries.size(); ++q) {
            const auto start = Clock::now();
            const auto found = index.search(queries[q], SearchParams{.k = 10, .ef_search = ef});
            samples.push_back(seconds_since(start) * 1e6);

            const auto exact = oracle.search(queries[q], SearchParams{.k = 10});
            std::unordered_set<LocalId> truth;
            for (const Candidate& c : exact) {
                truth.insert(c.local_id);
            }
            std::size_t hits = 0;
            for (const Candidate& c : found) {
                hits += truth.count(c.local_id);
            }
            recall_total += static_cast<double>(hits) / 10.0;
        }

        const Latency latency = Latency::from(samples);
        std::cout << "  " << std::left << std::setw(12) << ef << std::fixed
                  << std::setprecision(1) << std::setw(14) << latency.p50 << std::setw(14)
                  << latency.p95 << std::setprecision(4) << std::setw(14)
                  << (recall_total / static_cast<double>(queries.size()))
                  << std::setprecision(1) << (exact_p50 / latency.p50) << "x\n";
    }
}

void benchmark_batch_throughput() {
    print_header("Batch search throughput vs thread count");

    const DatasetSpec spec{.dimension = 128, .count = 50000, .seed = 7};
    const VectorArray data = vectordb::generate_dataset(spec);
    const VectorArray queries = vectordb::generate_queries(spec, 2000);

    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "vectordb-bench-batch";
    std::filesystem::remove_all(directory);

    vectordb::DatabaseConfig config;
    config.dimension = spec.dimension;
    config.metric = Metric::kL2Squared;
    config.index_type = vectordb::IndexType::kHnsw;

    {
        vectordb::Database database = vectordb::Database::create(directory, config);
        database.reserve(data.size());
        for (LocalId i = 0; i < data.size(); ++i) {
            database.insert(static_cast<vectordb::VectorId>(i), data[i]);
        }
        // Flush, and close it, before anything reopens the directory. Without
        // the flush the files still hold the empty database that create()
        // wrote, every reopened handle searches nothing, and this benchmark
        // reports an impossible 13 million queries per second.
        database.flush();
    }

    std::cout << "  N=" << spec.count << " D=" << spec.dimension
              << " queries=" << queries.size() << " k=10 (metadata off)\n\n"
              << "  " << std::left << std::setw(10) << "threads" << std::setw(16) << "queries/s"
              << "scaling\n";

    double single = 0.0;
    for (const std::size_t threads : {1U, 2U, 4U, 8U}) {
        vectordb::ConcurrentDatabase concurrent(vectordb::Database::open(directory), threads);

        // Checked once, before timing: a benchmark that measures a broken
        // configuration reports a wonderful number and teaches you nothing.
        // This assertion is what turns "13 million queries per second" from a
        // result into a failure.
        if (concurrent.size() != spec.count) {
            std::cerr << "benchmark setup error: database holds " << concurrent.size()
                      << " vectors, expected " << spec.count << "\n";
            return;
        }
        const vectordb::QueryOptions options{.k = 10, .include_metadata = false};
        const auto probe = concurrent.batch_search(queries, options);
        if (probe.empty() || probe.front().size() != 10) {
            std::cerr << "benchmark setup error: search returned "
                      << (probe.empty() ? 0 : probe.front().size()) << " results\n";
            return;
        }

        const double duration = time_per_iteration(
            1.0, [&] { static_cast<void>(concurrent.batch_search(queries, options)); });
        const double per_second = static_cast<double>(queries.size()) / duration;
        if (threads == 1) {
            single = per_second;
        }

        std::cout << "  " << std::left << std::setw(10) << threads << std::fixed
                  << std::setprecision(0) << std::setw(16) << per_second << std::setprecision(2)
                  << (per_second / single) << "x\n";
    }

    std::filesystem::remove_all(directory);
}

void benchmark_persistence() {
    print_header("Persistence");

    const DatasetSpec spec{.dimension = 128, .count = 50000, .seed = 3};
    const VectorArray data = vectordb::generate_dataset(spec);

    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "vectordb-bench-persist";
    std::filesystem::remove_all(directory);

    vectordb::DatabaseConfig config;
    config.dimension = spec.dimension;
    config.metric = Metric::kL2Squared;
    config.index_type = vectordb::IndexType::kHnsw;

    double insert_seconds = 0;
    double flush_seconds = 0;
    {
        vectordb::Database database = vectordb::Database::create(directory, config);
        database.reserve(data.size());

        const auto insert_start = Clock::now();
        for (LocalId i = 0; i < data.size(); ++i) {
            database.insert(static_cast<vectordb::VectorId>(i), data[i]);
        }
        insert_seconds = seconds_since(insert_start);

        const auto flush_start = Clock::now();
        database.flush();
        flush_seconds = seconds_since(flush_start);
    }

    const auto open_start = Clock::now();
    const vectordb::Database reopened = vectordb::Database::open(directory);
    const double open_seconds = seconds_since(open_start);

    const vectordb::DatabaseStats stats = reopened.stats();
    std::cout << "  N=" << spec.count << " D=" << spec.dimension << "\n"
              << "  insert + index:  " << std::fixed << std::setprecision(2) << insert_seconds
              << " s (" << std::setprecision(0)
              << (static_cast<double>(spec.count) / insert_seconds) << " vectors/s)\n"
              << "  flush to disk:   " << std::setprecision(3) << flush_seconds << " s\n"
              << "  reopen + load:   " << open_seconds << " s\n"
              << "  vectors.bin:     " << (stats.vector_file_bytes / 1024 / 1024) << " MB\n"
              << "  index.hnsw:      " << (stats.index_file_bytes / 1024 / 1024) << " MB\n";

    std::filesystem::remove_all(directory);
}

void print_provenance() {
    std::cout << vectordb::build_info() << "  kernel:     " << vectordb::active_kernel().name
              << "\n"
              << "  kernels:    ";
    for (const vectordb::DistanceKernel* kernel : vectordb::available_kernels()) {
        std::cout << kernel->name << " ";
    }
    std::cout << "\n  threads:    " << vectordb::hardware_threads() << "\n";

#if !defined(NDEBUG)
    std::cout << "\n*** WARNING: this is a Debug build. These numbers are not\n"
                 "*** representative and must not be reported. Use --preset release.\n";
#endif
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    const auto wants = [&](std::string_view name) {
        return args.empty() || std::find(args.begin(), args.end(), name) != args.end();
    };

    print_provenance();

    if (wants("distance")) {
        benchmark_distance_kernels();
    }

    if (wants("brute-force")) {
        print_header("Brute-force search latency (k=10, l2)");
        std::cout << std::left << std::setw(10) << "dim" << std::setw(12) << "N"
                  << std::setw(12) << "p50 (us)" << std::setw(12) << "p95 (us)"
                  << "GB/s\n";
        for (const Dimension dimension : {128U, 768U}) {
            for (const std::size_t count : {10000U, 100000U}) {
                benchmark_brute_force(dimension, count);
            }
        }
    }

    if (wants("hnsw")) {
        print_header("HNSW: build, latency and recall (k=10, l2)");
        benchmark_hnsw(128, 50000);
        benchmark_hnsw(768, 20000);
    }

    if (wants("batch")) {
        benchmark_batch_throughput();
    }

    if (wants("persistence")) {
        benchmark_persistence();
    }

    std::cout << "\n";
    return 0;
}
