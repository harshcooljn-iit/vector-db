// SPDX-License-Identifier: MIT
#include <string>
#include <unordered_set>

#include <gtest/gtest.h>

#include <vectordb/core/error.hpp>
#include <vectordb/storage/metadata_store.hpp>

#include "support/temp_dir.hpp"

namespace vectordb {
namespace {

using testing::TempDir;

TEST(MetadataStore, CreatesItsSchemaOnFirstOpen) {
    const TempDir dir;
    const auto path = dir.file("metadata.sqlite");
    {
        const MetadataStore store(path);
        EXPECT_EQ(store.vector_count(), 0U);
    }
    EXPECT_TRUE(std::filesystem::exists(path));
    // Reopening an existing database must not fail or wipe it.
    EXPECT_NO_THROW(MetadataStore{path});
}

TEST(MetadataStore, RoundTripsEveryValueType) {
    const TempDir dir;
    MetadataStore store(dir.file("metadata.sqlite"));

    const Metadata written{
        {"title", std::string("On Interest Rates")},
        {"year", std::int64_t{2026}},
        {"score", 0.875},
        {"reviewed", std::monostate{}},
    };
    store.set(42, written);

    const Metadata read = store.get(42);
    ASSERT_EQ(read.size(), 4U);
    EXPECT_EQ(std::get<std::string>(read.at("title")), "On Interest Rates");
    EXPECT_EQ(std::get<std::int64_t>(read.at("year")), 2026);
    EXPECT_DOUBLE_EQ(std::get<double>(read.at("score")), 0.875);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(read.at("reviewed")));
}

// SQLite is dynamically typed, so a naive schema would read 2026 back as a
// number regardless of what it was stored as. The explicit type column is what
// keeps an integer an integer and a string a string.
TEST(MetadataStore, PreservesTypeRatherThanLettingSqliteCoerceIt) {
    const TempDir dir;
    MetadataStore store(dir.file("metadata.sqlite"));

    store.set(1, Metadata{{"v", std::string("2026")}});
    store.set(2, Metadata{{"v", std::int64_t{2026}}});
    store.set(3, Metadata{{"v", 2026.0}});

    EXPECT_TRUE(std::holds_alternative<std::string>(store.get(1).at("v")));
    EXPECT_TRUE(std::holds_alternative<std::int64_t>(store.get(2).at("v")));
    EXPECT_TRUE(std::holds_alternative<double>(store.get(3).at("v")));
}

TEST(MetadataStore, SurvivesReopening) {
    const TempDir dir;
    const auto path = dir.file("metadata.sqlite");
    {
        MetadataStore store(path);
        store.set(7, Metadata{{"k", std::string("v")}});
        store.set_config("dimension", "768");
    }
    {
        const MetadataStore store(path);
        EXPECT_EQ(std::get<std::string>(store.get(7).at("k")), "v");
        EXPECT_EQ(store.get_config("dimension").value(), "768");
    }
}

TEST(MetadataStore, SetReplacesRatherThanMerges) {
    const TempDir dir;
    MetadataStore store(dir.file("metadata.sqlite"));

    store.set(1, Metadata{{"a", std::int64_t{1}}, {"b", std::int64_t{2}}});
    store.set(1, Metadata{{"c", std::int64_t{3}}});

    const Metadata read = store.get(1);
    EXPECT_EQ(read.size(), 1U);
    EXPECT_TRUE(read.count("c") == 1);
    EXPECT_TRUE(read.count("a") == 0) << "set() replaces the whole map";
}

TEST(MetadataStore, SetKeyMergesRatherThanReplaces) {
    const TempDir dir;
    MetadataStore store(dir.file("metadata.sqlite"));

    store.set(1, Metadata{{"a", std::int64_t{1}}});
    store.set_key(1, "b", std::int64_t{2});
    store.set_key(1, "a", std::int64_t{99});

    const Metadata read = store.get(1);
    ASSERT_EQ(read.size(), 2U);
    EXPECT_EQ(std::get<std::int64_t>(read.at("a")), 99);
    EXPECT_EQ(std::get<std::int64_t>(read.at("b")), 2);
}

TEST(MetadataStore, SetWithAnEmptyMapClearsEverything) {
    const TempDir dir;
    MetadataStore store(dir.file("metadata.sqlite"));
    store.set(1, Metadata{{"a", std::int64_t{1}}});
    store.set(1, Metadata{});
    EXPECT_TRUE(store.get(1).empty());
}

TEST(MetadataStore, GetOfAnUnknownIdIsEmptyRatherThanAnError) {
    const TempDir dir;
    const MetadataStore store(dir.file("metadata.sqlite"));
    EXPECT_TRUE(store.get(9999).empty());
}

TEST(MetadataStore, RemovesKeysAndWholeVectors) {
    const TempDir dir;
    MetadataStore store(dir.file("metadata.sqlite"));
    store.set(1, Metadata{{"a", std::int64_t{1}}, {"b", std::int64_t{2}}});

    EXPECT_TRUE(store.remove_key(1, "a"));
    EXPECT_FALSE(store.remove_key(1, "a")) << "removing twice reports no-op";
    EXPECT_EQ(store.get(1).size(), 1U);

    EXPECT_TRUE(store.remove(1));
    EXPECT_FALSE(store.remove(1));
    EXPECT_TRUE(store.get(1).empty());
}

TEST(MetadataStore, RejectsAnEmptyKey) {
    const TempDir dir;
    MetadataStore store(dir.file("metadata.sqlite"));
    EXPECT_THROW(store.set(1, Metadata{{"", std::int64_t{1}}}), InvalidArgumentError);
    EXPECT_THROW(store.set_key(1, "", std::int64_t{1}), InvalidArgumentError);
}

TEST(MetadataStore, HandlesValuesThatWouldBreakStringConcatenatedSql) {
    const TempDir dir;
    MetadataStore store(dir.file("metadata.sqlite"));

    // If values were concatenated into SQL rather than bound as parameters,
    // this would be a syntax error at best and a dropped table at worst.
    const std::string hostile = "'; DROP TABLE metadata; --";
    store.set(1, Metadata{{"note", hostile}});

    EXPECT_EQ(std::get<std::string>(store.get(1).at("note")), hostile);
    EXPECT_EQ(store.vector_count(), 1U) << "the table must still exist";
}

TEST(MetadataStore, HandlesUnicodeAndControlCharacters) {
    const TempDir dir;
    MetadataStore store(dir.file("metadata.sqlite"));
    const std::string text = "naïve — \"quoted\"\n\ttabbed";
    store.set(1, Metadata{{"t", text}});
    EXPECT_EQ(std::get<std::string>(store.get(1).at("t")), text);
}

TEST(MetadataStore, CountsVectorsRowsAndKeys) {
    const TempDir dir;
    MetadataStore store(dir.file("metadata.sqlite"));
    store.set(1, Metadata{{"a", std::int64_t{1}}, {"b", std::int64_t{2}}});
    store.set(2, Metadata{{"a", std::int64_t{3}}, {"c", std::int64_t{4}}});

    EXPECT_EQ(store.vector_count(), 2U);
    EXPECT_EQ(store.row_count(), 4U);
    EXPECT_EQ(store.keys(), (std::vector<std::string>{"a", "b", "c"}));
}

TEST(MetadataStore, FiltersById) {
    const TempDir dir;
    MetadataStore store(dir.file("metadata.sqlite"));
    store.set(1, Metadata{{"category", std::string("finance")}, {"year", std::int64_t{2020}}});
    store.set(2, Metadata{{"category", std::string("finance")}, {"year", std::int64_t{2026}}});
    store.set(3, Metadata{{"category", std::string("biology")}, {"year", std::int64_t{2026}}});

    EXPECT_EQ(store.matching(Filter::parse("category == \"finance\"")),
              (std::vector<VectorId>{1, 2}));
    EXPECT_EQ(store.matching(Filter::parse("year >= 2026")), (std::vector<VectorId>{2, 3}));
    EXPECT_EQ(store.matching(Filter::parse("category == \"finance\" and year >= 2026")),
              (std::vector<VectorId>{2}));
    EXPECT_TRUE(store.matching(Filter::parse("category == \"physics\"")).empty());

    // An empty filter returns every id that has metadata — not every vector in
    // the database, which is a distinction worth pinning.
    EXPECT_EQ(store.matching(Filter{}).size(), 3U);
}

TEST(MetadataStore, StoresAndReadsBackConfiguration) {
    const TempDir dir;
    MetadataStore store(dir.file("metadata.sqlite"));

    store.set_config("metric", "cosine");
    store.set_config("dimension", "768");
    store.set_config("metric", "l2");  // overwrite

    EXPECT_EQ(store.get_config("metric").value(), "l2");
    EXPECT_FALSE(store.get_config("absent").has_value());

    const auto all = store.all_config();
    EXPECT_GE(all.size(), 3U) << "schema_version is written on creation";
    EXPECT_EQ(store.get_config("schema_version").value(), "1");
}

TEST(MetadataStore, RollsBackAFailedTransaction) {
    const TempDir dir;
    MetadataStore store(dir.file("metadata.sqlite"));
    store.set(1, Metadata{{"original", std::int64_t{1}}});

    EXPECT_THROW(store.in_transaction([&] {
        store.set(2, Metadata{{"added", std::int64_t{2}}});
        throw InvalidArgumentError("simulated failure");
    }),
                 InvalidArgumentError);

    EXPECT_TRUE(store.get(2).empty()) << "the partial write must have been rolled back";
    EXPECT_EQ(store.get(1).size(), 1U) << "and the pre-existing data must survive";
}

TEST(MetadataStore, CommitsASuccessfulTransaction) {
    const TempDir dir;
    const auto path = dir.file("metadata.sqlite");
    {
        MetadataStore store(path);
        store.in_transaction([&] {
            for (VectorId id = 0; id < 100; ++id) {
                store.set(id, Metadata{{"n", static_cast<std::int64_t>(id)}});
            }
        });
    }
    const MetadataStore reopened(path);
    EXPECT_EQ(reopened.vector_count(), 100U);
}

TEST(MetadataStore, FindsAndRemovesOrphanedRows) {
    const TempDir dir;
    MetadataStore store(dir.file("metadata.sqlite"));
    for (VectorId id = 1; id <= 5; ++id) {
        store.set(id, Metadata{{"n", static_cast<std::int64_t>(id)}});
    }

    const std::unordered_set<VectorId> live{1, 3, 5};
    EXPECT_EQ(store.orphaned_ids(live), (std::vector<VectorId>{2, 4}));

    EXPECT_EQ(store.remove_orphans(live), 2U);
    EXPECT_EQ(store.vector_count(), 3U);
    EXPECT_TRUE(store.orphaned_ids(live).empty());
    EXPECT_EQ(store.remove_orphans(live), 0U);
}

TEST(MetadataValue, RendersDisplayAndJsonForms) {
    EXPECT_EQ(to_display_string(MetadataValue{std::int64_t{42}}), "42");
    EXPECT_EQ(to_display_string(MetadataValue{0.5}), "0.5");
    EXPECT_EQ(to_display_string(MetadataValue{std::string("hi")}), "hi");
    EXPECT_EQ(to_display_string(MetadataValue{std::monostate{}}), "null");

    EXPECT_EQ(to_json_string(MetadataValue{std::int64_t{42}}), "42");
    EXPECT_EQ(to_json_string(MetadataValue{std::string("hi")}), "\"hi\"");
    EXPECT_EQ(to_json_string(MetadataValue{std::string("say \"hi\"")}), "\"say \\\"hi\\\"\"");
    EXPECT_EQ(to_json_string(MetadataValue{std::string("a\nb")}), "\"a\\nb\"");
    EXPECT_EQ(to_json_string(MetadataValue{std::monostate{}}), "null");
}

}  // namespace
}  // namespace vectordb
