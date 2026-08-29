// SPDX-License-Identifier: MIT
//
// Full round trips through the public API, against real files on disk.
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <vectordb/core/error.hpp>
#include <vectordb/db/database.hpp>
#include <vectordb/util/dataset.hpp>

#include "support/temp_dir.hpp"

namespace vectordb {
namespace {

using testing::TempDir;

DatabaseConfig small_config(Dimension dimension = 8, IndexType type = IndexType::kBruteForce) {
    DatabaseConfig config;
    config.dimension = dimension;
    config.metric = Metric::kL2Squared;
    config.index_type = type;
    config.hnsw = HnswConfig{.m = 8, .ef_construction = 60, .ef_search = 50};
    return config;
}

std::vector<float> unit(Dimension dimension, Dimension hot) {
    std::vector<float> values(dimension, 0.0F);
    values[hot] = 1.0F;
    return values;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

TEST(Database, CreatesTheThreeArtefacts) {
    const TempDir dir;
    const auto path = dir.file("papers");

    const Database database = Database::create(path, small_config(16));

    EXPECT_TRUE(std::filesystem::exists(Database::vector_file_path(path)));
    EXPECT_TRUE(std::filesystem::exists(Database::metadata_file_path(path)));
    EXPECT_EQ(database.config().dimension, 16U);
    EXPECT_TRUE(database.empty());
}

TEST(Database, RefusesToOverwriteAnExistingDirectory) {
    const TempDir dir;
    const auto path = dir.file("papers");
    {
        const Database first = Database::create(path, small_config());
    }

    EXPECT_THROW(static_cast<void>(Database::create(path, small_config())),
                 InvalidArgumentError);
}

TEST(Database, RejectsADegenerateConfiguration) {
    const TempDir dir;
    DatabaseConfig config = small_config();
    config.dimension = 0;
    EXPECT_THROW(static_cast<void>(Database::create(dir.file("a"), config)),
                 InvalidArgumentError);

    config = small_config(8, IndexType::kHnsw);
    config.hnsw.m = 1;
    EXPECT_THROW(static_cast<void>(Database::create(dir.file("b"), config)),
                 InvalidArgumentError);
}

TEST(Database, OpeningSomethingThatIsNotADatabaseFails) {
    const TempDir dir;
    EXPECT_THROW(static_cast<void>(Database::open(dir.file("nowhere"))), NotFoundError);

    std::filesystem::create_directories(dir.file("empty"));
    EXPECT_THROW(static_cast<void>(Database::open(dir.file("empty"))), NotFoundError);
}

// The headline acceptance criterion: insert, close the process, reopen, search.
TEST(Database, SurvivesAFullCloseAndReopen) {
    const TempDir dir;
    const auto path = dir.file("papers");

    {
        Database database = Database::create(path, small_config(4));
        database.insert(100, unit(4, 0), Metadata{{"name", std::string("first")}});
        database.insert(200, unit(4, 1), Metadata{{"name", std::string("second")}});
        database.insert(300, unit(4, 2));
        database.flush();
    }

    Database reopened = Database::open(path);
    EXPECT_EQ(reopened.size(), 3U);
    EXPECT_EQ(reopened.config().dimension, 4U);
    EXPECT_EQ(reopened.config().metric, Metric::kL2Squared);

    const auto stored = reopened.get(200);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->vector, unit(4, 1));
    EXPECT_EQ(std::get<std::string>(stored->metadata.at("name")), "second");

    const auto results = reopened.search(unit(4, 0), QueryOptions{.k = 2});
    ASSERT_EQ(results.size(), 2U);
    EXPECT_EQ(results[0].id, 100U);
}

TEST(Database, PersistsWithoutAnExplicitFlushBecauseTheDestructorFlushes) {
    const TempDir dir;
    const auto path = dir.file("papers");
    {
        Database database = Database::create(path, small_config(4));
        database.insert(1, unit(4, 0));
    }

    EXPECT_EQ(Database::open(path).size(), 1U);
}

TEST(Database, PersistsTheHnswIndexAndReusesItOnOpen) {
    const TempDir dir;
    const auto path = dir.file("papers");
    const DatasetSpec spec{.dimension = 16, .count = 300, .seed = 12};
    const VectorArray data = generate_dataset(spec);

    {
        Database database = Database::create(path, small_config(16, IndexType::kHnsw));
        for (LocalId i = 0; i < data.size(); ++i) {
            database.insert(static_cast<VectorId>(i), data[i]);
        }
    }

    EXPECT_TRUE(std::filesystem::exists(Database::index_file_path(path)));

    Database reopened = Database::open(path);
    EXPECT_EQ(reopened.size(), 300U);
    EXPECT_EQ(reopened.stats().index_type, IndexType::kHnsw);

    const VectorArray queries = generate_queries(spec, 5);
    for (LocalId q = 0; q < queries.size(); ++q) {
        EXPECT_FALSE(reopened.search(queries[q], QueryOptions{.k = 5}).empty());
    }
}

// A corrupt index must never fail an open: the vector store is authoritative,
// so the correct response is to spend CPU rebuilding, not to refuse.
TEST(Database, RebuildsARuinedIndexOnOpenInsteadOfFailing) {
    const TempDir dir;
    const auto path = dir.file("papers");
    const VectorArray data = generate_dataset({.dimension = 8, .count = 200, .seed = 3});

    {
        Database database = Database::create(path, small_config(8, IndexType::kHnsw));
        for (LocalId i = 0; i < data.size(); ++i) {
            database.insert(static_cast<VectorId>(i), data[i]);
        }
    }

    {
        std::ofstream ruin(Database::index_file_path(path), std::ios::binary | std::ios::trunc);
        ruin << "this is not an index any more";
    }

    Database reopened = Database::open(path);
    EXPECT_EQ(reopened.size(), 200U);
    EXPECT_FALSE(reopened.search(data[0], QueryOptions{.k = 5}).empty())
        << "search must work after the rebuild";
}

TEST(Database, RebuildsWhenTheIndexFileIsSimplyMissing) {
    const TempDir dir;
    const auto path = dir.file("papers");
    {
        Database database = Database::create(path, small_config(4, IndexType::kHnsw));
        database.insert(1, unit(4, 0));
        database.insert(2, unit(4, 1));
    }
    std::filesystem::remove(Database::index_file_path(path));

    Database reopened = Database::open(path);
    EXPECT_EQ(reopened.search(unit(4, 0), QueryOptions{.k = 1}).at(0).id, 1U);
}

// ---------------------------------------------------------------------------
// Ids
// ---------------------------------------------------------------------------

TEST(Database, AssignsAutomaticIdsThatAreNeverReused) {
    const TempDir dir;
    const auto path = dir.file("papers");

    Database database = Database::create(path, small_config(4));
    EXPECT_EQ(database.insert(unit(4, 0)), 0U);
    EXPECT_EQ(database.insert(unit(4, 1)), 1U);

    ASSERT_TRUE(database.remove(1));
    // 1 must not come back: a stale reference to a deleted vector would
    // silently start resolving to a different one.
    EXPECT_EQ(database.insert(unit(4, 2)), 2U);
}

TEST(Database, AutomaticIdsSurviveAReopen) {
    const TempDir dir;
    const auto path = dir.file("papers");
    {
        Database database = Database::create(path, small_config(4));
        database.insert(unit(4, 0));
        database.insert(unit(4, 1));
    }

    Database reopened = Database::open(path);
    EXPECT_EQ(reopened.insert(unit(4, 2)), 2U);
}

TEST(Database, ExplicitIdsAdvanceTheAutomaticCounter) {
    const TempDir dir;
    Database database = Database::create(dir.file("papers"), small_config(4));
    database.insert(500, unit(4, 0));
    EXPECT_EQ(database.insert(unit(4, 1)), 501U);
}

TEST(Database, RejectsADuplicateId) {
    const TempDir dir;
    Database database = Database::create(dir.file("papers"), small_config(4));
    database.insert(7, unit(4, 0));
    EXPECT_THROW(database.insert(7, unit(4, 1)), DuplicateIdError);
    EXPECT_EQ(database.size(), 1U);
}

TEST(Database, RejectsWrongDimensionAndNonFiniteVectors) {
    const TempDir dir;
    Database database = Database::create(dir.file("papers"), small_config(4));

    EXPECT_THROW(database.insert(1, std::vector<float>(3, 1.0F)), DimensionMismatchError);
    EXPECT_THROW(
        database.insert(
            2, std::vector<float>{1.0F, 0.0F, 0.0F, std::numeric_limits<float>::quiet_NaN()}),
        InvalidVectorError);
    EXPECT_EQ(database.size(), 0U);
}

// ---------------------------------------------------------------------------
// Deletion, compaction, rebuild
// ---------------------------------------------------------------------------

TEST(Database, DeletionRemovesTheVectorAndItsMetadata) {
    const TempDir dir;
    Database database = Database::create(dir.file("papers"), small_config(4));
    database.insert(1, unit(4, 0), Metadata{{"k", std::string("v")}});

    EXPECT_TRUE(database.remove(1));
    EXPECT_FALSE(database.remove(1));
    EXPECT_FALSE(database.contains(1));
    EXPECT_FALSE(database.get(1).has_value());
    EXPECT_TRUE(database.search(unit(4, 0), QueryOptions{.k = 5}).empty());
}

TEST(Database, DeletionSurvivesAReopen) {
    const TempDir dir;
    const auto path = dir.file("papers");
    {
        Database database = Database::create(path, small_config(4));
        database.insert(1, unit(4, 0));
        database.insert(2, unit(4, 1));
        database.remove(1);
    }

    Database reopened = Database::open(path);
    EXPECT_EQ(reopened.size(), 1U);
    EXPECT_FALSE(reopened.contains(1)) << "a reopen must not resurrect a deleted vector";
    EXPECT_TRUE(reopened.contains(2));
}

TEST(Database, CompactReclaimsTombstonesAndKeepsVectorIds) {
    const TempDir dir;
    Database database = Database::create(dir.file("papers"), small_config(8));
    const VectorArray data = generate_dataset({.dimension = 8, .count = 100, .seed = 2});
    for (LocalId i = 0; i < data.size(); ++i) {
        database.insert(static_cast<VectorId>(i) * 3, data[i]);
    }
    for (VectorId id = 0; id < 150; id += 6) {
        database.remove(id);
    }

    const std::size_t before = database.stats().tombstones;
    ASSERT_GT(before, 0U);

    EXPECT_EQ(database.compact(), before);
    EXPECT_EQ(database.stats().tombstones, 0U);
    EXPECT_EQ(database.stats().slots, database.size());

    // The whole point of a separate VectorId space: compaction renumbers slots
    // and users never notice.
    EXPECT_TRUE(database.contains(3));
    EXPECT_TRUE(database.contains(297));
    EXPECT_FALSE(database.contains(0));
}

TEST(Database, CompactOnACleanDatabaseIsANoOp) {
    const TempDir dir;
    Database database = Database::create(dir.file("papers"), small_config(4));
    database.insert(1, unit(4, 0));
    EXPECT_EQ(database.compact(), 0U);
    EXPECT_EQ(database.size(), 1U);
}

TEST(Database, RebuildIndexKeepsResultsIntact) {
    const TempDir dir;
    const DatasetSpec spec{.dimension = 16, .count = 400, .seed = 8};
    const VectorArray data = generate_dataset(spec);

    Database database =
        Database::create(dir.file("papers"), small_config(16, IndexType::kHnsw));
    for (LocalId i = 0; i < data.size(); ++i) {
        database.insert(static_cast<VectorId>(i), data[i]);
    }

    const VectorArray queries = generate_queries(spec, 10);
    std::vector<std::vector<QueryResult>> before;
    for (LocalId q = 0; q < queries.size(); ++q) {
        before.push_back(database.search(queries[q], QueryOptions{.k = 5}));
    }

    database.rebuild_index();

    for (LocalId q = 0; q < queries.size(); ++q) {
        const auto after = database.search(queries[q], QueryOptions{.k = 5});
        ASSERT_EQ(after.size(), before[q].size()) << "query " << q;
        for (std::size_t i = 0; i < after.size(); ++i) {
            EXPECT_EQ(after[i].id, before[q][i].id) << "query " << q << " position " << i;
        }
    }
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

TEST(Database, ReportsMetricNativeScores) {
    const TempDir dir;
    DatabaseConfig config = small_config(2);
    config.metric = Metric::kCosine;
    Database database = Database::create(dir.file("papers"), config);

    database.insert(1, std::vector<float>{1.0F, 0.0F});
    database.insert(2, std::vector<float>{0.0F, 1.0F});
    database.insert(3, std::vector<float>{-1.0F, 0.0F});

    const auto results = database.search(std::vector<float>{1.0F, 0.0F}, QueryOptions{.k = 3});
    ASSERT_EQ(results.size(), 3U);
    // Cosine reports similarity, so larger is better and the list descends.
    EXPECT_NEAR(results[0].score, 1.0F, 1e-5F);
    EXPECT_NEAR(results[1].score, 0.0F, 1e-5F);
    EXPECT_NEAR(results[2].score, -1.0F, 1e-5F);
}

TEST(Database, AttachesMetadataToResults) {
    const TempDir dir;
    Database database = Database::create(dir.file("papers"), small_config(4));
    database.insert(1, unit(4, 0), Metadata{{"category", std::string("finance")}});

    const auto results = database.search(unit(4, 0), QueryOptions{.k = 1});
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(std::get<std::string>(results[0].metadata.at("category")), "finance");
}

TEST(Database, FiltersResultsByMetadata) {
    const TempDir dir;
    Database database = Database::create(dir.file("papers"), small_config(4));
    for (VectorId id = 0; id < 20; ++id) {
        database.insert(id,
                        unit(4, static_cast<Dimension>(id % 4)),
                        Metadata{{"category", std::string(id % 2 == 0 ? "finance" : "biology")},
                                 {"year", static_cast<std::int64_t>(2000 + id)}});
    }

    QueryOptions options;
    options.k = 5;
    options.filter = Filter::parse("category == \"finance\"");

    const auto results = database.search(unit(4, 0), options);
    ASSERT_FALSE(results.empty());
    for (const QueryResult& result : results) {
        EXPECT_EQ(result.id % 2, 0U) << "a non-matching vector was returned";
        EXPECT_EQ(std::get<std::string>(result.metadata.at("category")), "finance");
    }
}

TEST(Database, ExactSearchWithASelectiveFilterFindsWhatAnnMayMiss) {
    const TempDir dir;
    const DatasetSpec spec{.dimension = 16, .count = 2000, .seed = 44};
    const VectorArray data = generate_dataset(spec);

    Database database =
        Database::create(dir.file("papers"), small_config(16, IndexType::kHnsw));
    for (LocalId i = 0; i < data.size(); ++i) {
        // Only 1 vector in 200 matches — the regime where a post-filter over an
        // over-fetched window runs out of candidates.
        database.insert(static_cast<VectorId>(i),
                        data[i],
                        Metadata{{"rare", static_cast<std::int64_t>(i % 200 == 0 ? 1 : 0)}});
    }

    QueryOptions options;
    options.k = 5;
    options.filter = Filter::parse("rare == 1");
    options.exact = true;

    const auto results = database.search(generate_queries(spec, 1)[0], options);
    EXPECT_EQ(results.size(), 5U)
        << "an exact scan must fill k even for a highly selective filter";
    for (const QueryResult& result : results) {
        EXPECT_EQ(result.id % 200, 0U);
    }
}

TEST(Database, FilterIdsIgnoresMetadataForDeletedVectors) {
    const TempDir dir;
    Database database = Database::create(dir.file("papers"), small_config(4));
    database.insert(1, unit(4, 0), Metadata{{"tag", std::string("x")}});
    database.insert(2, unit(4, 1), Metadata{{"tag", std::string("x")}});

    EXPECT_EQ(database.filter_ids(Filter::parse("tag == \"x\"")).size(), 2U);
    database.remove(1);
    EXPECT_EQ(database.filter_ids(Filter::parse("tag == \"x\"")), (std::vector<VectorId>{2}));
}

TEST(Database, BatchSearchMatchesRepeatedSingleSearches) {
    const TempDir dir;
    const DatasetSpec spec{.dimension = 8, .count = 200, .seed = 19};
    const VectorArray data = generate_dataset(spec);
    Database database = Database::create(dir.file("papers"), small_config(8));
    for (LocalId i = 0; i < data.size(); ++i) {
        database.insert(static_cast<VectorId>(i), data[i]);
    }

    const VectorArray queries = generate_queries(spec, 10);
    const auto batch = database.batch_search(queries, QueryOptions{.k = 3});

    ASSERT_EQ(batch.size(), queries.size());
    for (LocalId q = 0; q < queries.size(); ++q) {
        const auto single = database.search(queries[q], QueryOptions{.k = 3});
        ASSERT_EQ(batch[q].size(), single.size());
        for (std::size_t i = 0; i < single.size(); ++i) {
            EXPECT_EQ(batch[q][i].id, single[i].id);
        }
    }
}

TEST(Database, RejectsBadSearchArguments) {
    const TempDir dir;
    Database database = Database::create(dir.file("papers"), small_config(4));
    database.insert(1, unit(4, 0));

    EXPECT_THROW(static_cast<void>(database.search(unit(4, 0), QueryOptions{.k = 0})),
                 InvalidArgumentError);
    EXPECT_THROW(
        static_cast<void>(database.search(std::vector<float>(3, 0.0F), QueryOptions{.k = 1})),
        DimensionMismatchError);
}

// ---------------------------------------------------------------------------
// Normalization
// ---------------------------------------------------------------------------

TEST(Database, NormalizingDatabaseStoresUnitVectors) {
    const TempDir dir;
    DatabaseConfig config = small_config(2);
    config.metric = Metric::kCosine;
    config.normalize = true;

    const auto path = dir.file("papers");
    {
        Database database = Database::create(path, config);
        database.insert(1, std::vector<float>{3.0F, 4.0F});
    }

    Database reopened = Database::open(path);
    EXPECT_TRUE(reopened.config().normalize) << "the flag must survive a reopen";
    const auto stored = reopened.get(1);
    ASSERT_TRUE(stored.has_value());
    EXPECT_NEAR(stored->vector[0], 0.6F, 1e-5F);
    EXPECT_NEAR(stored->vector[1], 0.8F, 1e-5F);
}

// ---------------------------------------------------------------------------
// Stats and check
// ---------------------------------------------------------------------------

TEST(Database, ReportsStats) {
    const TempDir dir;
    Database database =
        Database::create(dir.file("papers"), small_config(16, IndexType::kHnsw));
    const VectorArray data = generate_dataset({.dimension = 16, .count = 50, .seed = 1});
    for (LocalId i = 0; i < data.size(); ++i) {
        database.insert(
            static_cast<VectorId>(i), data[i], Metadata{{"n", static_cast<std::int64_t>(i)}});
    }
    database.remove(0);
    database.flush();

    const DatabaseStats stats = database.stats();
    EXPECT_EQ(stats.dimension, 16U);
    EXPECT_EQ(stats.live_vectors, 49U);
    EXPECT_EQ(stats.slots, 50U);
    EXPECT_EQ(stats.tombstones, 1U);
    EXPECT_EQ(stats.metadata_vectors, 49U) << "deleting a vector removes its metadata";
    EXPECT_EQ(stats.metadata_keys, (std::vector<std::string>{"n"}));
    EXPECT_GT(stats.vector_file_bytes, 0U);
    EXPECT_GT(stats.index_file_bytes, 0U);
    EXPECT_NE(stats.index_description.find("hnsw"), std::string::npos);
}

TEST(Database, CheckPassesOnAHealthyDatabase) {
    const TempDir dir;
    Database database = Database::create(dir.file("papers"), small_config(8, IndexType::kHnsw));
    const VectorArray data = generate_dataset({.dimension = 8, .count = 60, .seed = 4});
    for (LocalId i = 0; i < data.size(); ++i) {
        database.insert(static_cast<VectorId>(i), data[i]);
    }
    database.flush();

    const CheckReport report = database.check(/*deep=*/true);
    for (const CheckIssue& issue : report.issues) {
        ADD_FAILURE() << issue.severity << ": " << issue.message;
    }
    EXPECT_TRUE(report.ok());
}

TEST(Database, CheckReportsADamagedVectorPayload) {
    const TempDir dir;
    const auto path = dir.file("papers");
    {
        Database database = Database::create(path, small_config(8));
        const VectorArray data = generate_dataset({.dimension = 8, .count = 40, .seed = 5});
        for (LocalId i = 0; i < data.size(); ++i) {
            database.insert(static_cast<VectorId>(i), data[i]);
        }
    }

    {
        std::fstream file(Database::vector_file_path(path),
                          std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(80);
        const char byte = 0x7F;
        file.write(&byte, 1);
    }

    Database reopened = Database::open(path);
    const CheckReport shallow = reopened.check(/*deep=*/false);
    const CheckReport deep = reopened.check(/*deep=*/true);

    EXPECT_TRUE(shallow.ok()) << "structural validation cannot see a flipped payload bit";
    EXPECT_GE(deep.error_count(), 1U) << "the deep check must catch it";
}

TEST(Database, CheckWarnsAboutHeavyTombstoning) {
    const TempDir dir;
    Database database = Database::create(dir.file("papers"), small_config(4));
    for (VectorId id = 0; id < 200; ++id) {
        database.insert(id, unit(4, static_cast<Dimension>(id % 4)));
    }
    for (VectorId id = 0; id < 150; ++id) {
        database.remove(id);
    }
    database.flush();

    const CheckReport report = database.check();
    EXPECT_EQ(report.error_count(), 0U);
    EXPECT_GE(report.warning_count(), 1U);
    bool mentions_compact = false;
    for (const CheckIssue& issue : report.issues) {
        if (issue.message.find("compact") != std::string::npos) {
            mentions_compact = true;
        }
    }
    EXPECT_TRUE(mentions_compact) << "a warning must say what to do about it";
}

}  // namespace
}  // namespace vectordb
