// SPDX-License-Identifier: MIT
//
// The reader/writer contract, exercised under real contention.
//
// Run these under the `tsan` preset periodically: a data race that does not
// manifest today is still a race, and ThreadSanitizer finds it deterministically
// where a stress test finds it once every ten thousand runs.
#include <atomic>
#include <barrier>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <vectordb/db/concurrent_database.hpp>
#include <vectordb/util/dataset.hpp>

#include "support/temp_dir.hpp"

namespace vectordb {
namespace {

using testing::TempDir;

ConcurrentDatabase make_database(const TempDir& dir,
                                 const DatasetSpec& spec,
                                 std::size_t threads = 4) {
    DatabaseConfig config;
    config.dimension = spec.dimension;
    config.metric = Metric::kL2Squared;
    config.index_type = IndexType::kHnsw;
    config.hnsw = HnswConfig{.m = 8, .ef_construction = 60, .ef_search = 50};

    Database database = Database::create(dir.file("db"), config);
    const VectorArray data = generate_dataset(spec);
    database.reserve(data.size());
    for (LocalId i = 0; i < data.size(); ++i) {
        database.insert(static_cast<VectorId>(i), data[i]);
    }
    database.flush();

    return ConcurrentDatabase(std::move(database), threads);
}

TEST(ConcurrentDatabase, ConcurrentSearchesAgreeWithSequentialOnes) {
    const TempDir dir;
    const DatasetSpec spec{.dimension = 32, .count = 2000, .seed = 21};
    const ConcurrentDatabase database = make_database(dir, spec);
    const VectorArray queries = generate_queries(spec, 40);

    // Ground truth, computed with no concurrency at all.
    std::vector<std::vector<QueryResult>> expected;
    for (LocalId q = 0; q < queries.size(); ++q) {
        expected.push_back(database.search(queries[q], QueryOptions{.k = 10}));
    }

    constexpr int kThreads = 8;
    std::vector<std::thread> threads;
    std::atomic<int> mismatches{0};
    std::barrier start(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            // Release everybody at once, so the searches genuinely overlap
            // rather than being spread out by thread creation cost.
            start.arrive_and_wait();
            for (int repeat = 0; repeat < 5; ++repeat) {
                for (LocalId q = 0; q < queries.size(); ++q) {
                    const auto got = database.search(queries[q], QueryOptions{.k = 10});
                    if (got.size() != expected[q].size()) {
                        mismatches.fetch_add(1);
                        continue;
                    }
                    for (std::size_t i = 0; i < got.size(); ++i) {
                        if (got[i].id != expected[q][i].id) {
                            mismatches.fetch_add(1);
                        }
                    }
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(mismatches.load(), 0)
        << "concurrent searches must return exactly what a sequential one does";
}

TEST(ConcurrentDatabase, BatchSearchMatchesIndividualSearches) {
    const TempDir dir;
    const DatasetSpec spec{.dimension = 16, .count = 1000, .seed = 5};
    const ConcurrentDatabase database = make_database(dir, spec);
    const VectorArray queries = generate_queries(spec, 50);

    const auto batch = database.batch_search(queries, QueryOptions{.k = 5});
    ASSERT_EQ(batch.size(), queries.size());

    for (LocalId q = 0; q < queries.size(); ++q) {
        const auto single = database.search(queries[q], QueryOptions{.k = 5});
        ASSERT_EQ(batch[q].size(), single.size()) << "query " << q;
        for (std::size_t i = 0; i < single.size(); ++i) {
            EXPECT_EQ(batch[q][i].id, single[i].id) << "query " << q << " position " << i;
        }
    }
}

TEST(ConcurrentDatabase, BatchSearchHandlesAnEmptyQuerySet) {
    const TempDir dir;
    const DatasetSpec spec{.dimension = 8, .count = 100, .seed = 1};
    const ConcurrentDatabase database = make_database(dir, spec);
    EXPECT_TRUE(database.batch_search(VectorArray(8), QueryOptions{.k = 3}).empty());
}

// Readers and a writer running together. The assertion is not about ordering —
// a search may legitimately run before or after a given insert — but about the
// database remaining coherent throughout.
TEST(ConcurrentDatabase, StaysCoherentWithReadersAndAWriterRunningTogether) {
    const TempDir dir;
    const DatasetSpec spec{.dimension = 16, .count = 500, .seed = 9};
    ConcurrentDatabase database = make_database(dir, spec);
    const VectorArray queries = generate_queries(spec, 20);
    const VectorArray extra =
        generate_dataset({.dimension = 16, .count = 200, .seed = 9, .sample_seed = 777});

    std::atomic<bool> stop{false};
    std::atomic<int> searches{0};
    std::atomic<int> failures{0};

    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&] {
            while (!stop.load()) {
                for (LocalId q = 0; q < queries.size(); ++q) {
                    try {
                        const auto results = database.search(queries[q], QueryOptions{.k = 5});
                        if (results.size() > 5) {
                            failures.fetch_add(1);
                        }
                        for (const QueryResult& result : results) {
                            // Every returned id must resolve — a torn read
                            // would show up as an id that is not in the store.
                            if (!database.contains(result.id)) {
                                failures.fetch_add(1);
                            }
                        }
                        searches.fetch_add(1);
                    } catch (const std::exception&) {
                        failures.fetch_add(1);
                    }
                }
            }
        });
    }

    for (LocalId i = 0; i < extra.size(); ++i) {
        database.insert(static_cast<VectorId>(10000 + i), extra[i]);
    }

    stop.store(true);
    for (std::thread& reader : readers) {
        reader.join();
    }

    EXPECT_EQ(failures.load(), 0);
    EXPECT_GT(searches.load(), 0) << "the readers never actually ran";
    EXPECT_EQ(database.size(), 700U);
}

TEST(ConcurrentDatabase, WritesAreSerialisedSoNoInsertIsLost) {
    const TempDir dir;
    DatabaseConfig config;
    config.dimension = 4;
    config.metric = Metric::kL2Squared;
    config.index_type = IndexType::kBruteForce;

    ConcurrentDatabase database(Database::create(dir.file("db"), config), 4);

    constexpr int kThreads = 6;
    constexpr int kPerThread = 100;
    std::vector<std::thread> writers;
    std::barrier start(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        writers.emplace_back([&, t] {
            start.arrive_and_wait();
            for (int i = 0; i < kPerThread; ++i) {
                const auto id = static_cast<VectorId>(t * kPerThread + i);
                database.insert(id, std::vector<float>(4, static_cast<float>(id)));
            }
        });
    }
    for (std::thread& writer : writers) {
        writer.join();
    }

    EXPECT_EQ(database.size(), kThreads * kPerThread);
    for (VectorId id = 0; id < kThreads * kPerThread; ++id) {
        EXPECT_TRUE(database.contains(id)) << "lost insert " << id;
    }
}

TEST(ConcurrentDatabase, ExposesItsThreadCountAndConfiguration) {
    const TempDir dir;
    const DatasetSpec spec{.dimension = 8, .count = 50, .seed = 2};
    const ConcurrentDatabase database = make_database(dir, spec, 3);

    EXPECT_EQ(database.thread_count(), 3U);
    EXPECT_EQ(database.config().dimension, 8U);
    EXPECT_EQ(database.size(), 50U);
}

}  // namespace
}  // namespace vectordb
