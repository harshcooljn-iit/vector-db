// SPDX-License-Identifier: MIT
//
// Regression tests for the metadata-on-the-search-path fix.
//
// Skipping the metadata lookup is a large performance win and exactly the kind
// of optimisation that quietly changes behaviour, so each way it could go wrong
// has a test.
#include <gtest/gtest.h>

#include <vectordb/db/database.hpp>

#include "support/temp_dir.hpp"

namespace vectordb {
namespace {

using testing::TempDir;

Database make_database(const TempDir& dir) {
    DatabaseConfig config;
    config.dimension = 2;
    config.metric = Metric::kL2Squared;
    config.index_type = IndexType::kBruteForce;
    return Database::create(dir.file("db"), config);
}

TEST(MetadataOnSearchPath, MetadataIsStillAttachedByDefault) {
    const TempDir dir;
    Database database = make_database(dir);
    database.insert(
        1, std::vector<float>{1.0F, 0.0F}, Metadata{{"name", std::string("first")}});

    const auto results = database.search(std::vector<float>{1.0F, 0.0F}, QueryOptions{.k = 1});
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(std::get<std::string>(results[0].metadata.at("name")), "first");
}

TEST(MetadataOnSearchPath, IncludeMetadataFalseOmitsItButKeepsResults) {
    const TempDir dir;
    Database database = make_database(dir);
    database.insert(
        1, std::vector<float>{1.0F, 0.0F}, Metadata{{"name", std::string("first")}});

    const auto results = database.search(std::vector<float>{1.0F, 0.0F},
                                         QueryOptions{.k = 1, .include_metadata = false});
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results[0].id, 1U);
    EXPECT_TRUE(results[0].metadata.empty());
}

// A filter must work even with include_metadata = false: the lookup is still
// needed to evaluate the predicate, and skipping it would silently return
// unfiltered results.
TEST(MetadataOnSearchPath, FilteringStillWorksWithIncludeMetadataFalse) {
    const TempDir dir;
    Database database = make_database(dir);
    database.insert(1, std::vector<float>{1.0F, 0.0F}, Metadata{{"cat", std::string("a")}});
    database.insert(2, std::vector<float>{0.9F, 0.1F}, Metadata{{"cat", std::string("b")}});

    QueryOptions options{.k = 5, .include_metadata = false};
    options.filter = Filter::parse("cat == \"b\"");

    const auto results = database.search(std::vector<float>{1.0F, 0.0F}, options);
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results[0].id, 2U);
}

// The skip is gated on "this database has metadata at all". Metadata added
// after the database was opened must still be seen.
TEST(MetadataOnSearchPath, MetadataAddedAfterOpenIsStillReturned) {
    const TempDir dir;
    const auto path = dir.file("db");
    {
        DatabaseConfig config;
        config.dimension = 2;
        config.index_type = IndexType::kBruteForce;
        Database database = Database::create(path, config);
        database.insert(1, std::vector<float>{1.0F, 0.0F});  // no metadata at all
    }

    Database reopened = Database::open(path);
    // Opened with an empty metadata store, so the cached flag starts false.
    ASSERT_TRUE(reopened.search(std::vector<float>{1.0F, 0.0F}, QueryOptions{.k = 1})
                    .at(0)
                    .metadata.empty());

    reopened.set_metadata(1, Metadata{{"added", std::string("later")}});

    const auto results = reopened.search(std::vector<float>{1.0F, 0.0F}, QueryOptions{.k = 1});
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(std::get<std::string>(results[0].metadata.at("added")), "later")
        << "the cached has-metadata flag must be updated by a write";
}

TEST(MetadataOnSearchPath, MetadataFromAPriorRunIsSeenAfterReopen) {
    const TempDir dir;
    const auto path = dir.file("db");
    {
        DatabaseConfig config;
        config.dimension = 2;
        config.index_type = IndexType::kBruteForce;
        Database database = Database::create(path, config);
        database.insert(1, std::vector<float>{1.0F, 0.0F}, Metadata{{"k", std::string("v")}});
    }

    Database reopened = Database::open(path);
    const auto results = reopened.search(std::vector<float>{1.0F, 0.0F}, QueryOptions{.k = 1});
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(std::get<std::string>(results[0].metadata.at("k")), "v")
        << "the flag must be initialised from the store on open";
}

// A filter over a database with no metadata matches nothing, because a missing
// key never matches. The fast path must agree with the slow one.
TEST(MetadataOnSearchPath, FilterOverAMetadatalessDatabaseMatchesNothing) {
    const TempDir dir;
    Database database = make_database(dir);
    database.insert(1, std::vector<float>{1.0F, 0.0F});
    database.insert(2, std::vector<float>{0.0F, 1.0F});

    QueryOptions options{.k = 5};
    options.filter = Filter::parse("anything == 1");

    EXPECT_TRUE(database.search(std::vector<float>{1.0F, 0.0F}, options).empty());
}

}  // namespace
}  // namespace vectordb
