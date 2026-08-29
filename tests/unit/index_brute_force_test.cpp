// SPDX-License-Identifier: MIT
//
// Brute force is the oracle, so its own tests must not lean on anything it
// could be wrong about. Expectations here are hand-computed or derived from an
// independent full sort.
#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <vectordb/core/error.hpp>
#include <vectordb/index/brute_force_index.hpp>
#include <vectordb/storage/vector_store.hpp>
#include <vectordb/util/dataset.hpp>

namespace vectordb {
namespace {

std::vector<LocalId> ids_of(const std::vector<Candidate>& candidates) {
    std::vector<LocalId> ids;
    ids.reserve(candidates.size());
    for (const Candidate& candidate : candidates) {
        ids.push_back(candidate.local_id);
    }
    return ids;
}

/// A 2-D store whose geometry is obvious on paper.
VectorStore make_plane_store() {
    VectorStore store(2);
    store.insert(0, std::vector<float>{0.0F, 0.0F});   // slot 0
    store.insert(1, std::vector<float>{1.0F, 0.0F});   // slot 1
    store.insert(2, std::vector<float>{0.0F, 1.0F});   // slot 2
    store.insert(3, std::vector<float>{3.0F, 4.0F});   // slot 3, at distance 5
    store.insert(4, std::vector<float>{-1.0F, 0.0F});  // slot 4
    return store;
}

TEST(BruteForceIndex, FindsTheNearestNeighboursInOrder) {
    const VectorStore store = make_plane_store();
    const BruteForceIndex index(store.accessor(), Metric::kL2Squared);

    const std::vector<float> query{0.1F, 0.0F};
    const std::vector<Candidate> results = index.search(query, SearchParams{.k = 3});

    ASSERT_EQ(results.size(), 3U);
    // distances^2: slot1 = 0.81, slot0 = 0.01, slot2 = 1.01, slot4 = 1.21
    EXPECT_EQ(ids_of(results), (std::vector<LocalId>{0, 1, 2}));
    EXPECT_NEAR(results[0].key, 0.01F, 1e-5F);
    EXPECT_NEAR(results[1].key, 0.81F, 1e-5F);
}

TEST(BruteForceIndex, ReturnsEverythingWhenKExceedsTheStoreSize) {
    const VectorStore store = make_plane_store();
    const BruteForceIndex index(store.accessor(), Metric::kL2Squared);

    const std::vector<Candidate> results =
        index.search(std::vector<float>{0.0F, 0.0F}, SearchParams{.k = 100});
    EXPECT_EQ(results.size(), 5U);
}

TEST(BruteForceIndex, SkipsTombstonedSlots) {
    VectorStore store = make_plane_store();
    // Slot 0 is the exact nearest neighbour of the query below.
    ASSERT_TRUE(store.remove(0));

    const BruteForceIndex index(store.accessor(), Metric::kL2Squared);
    const std::vector<Candidate> results =
        index.search(std::vector<float>{0.05F, 0.0F}, SearchParams{.k = 5});

    EXPECT_EQ(results.size(), 4U);
    for (const Candidate& candidate : results) {
        EXPECT_NE(candidate.local_id, 0U) << "a deleted slot must never be returned";
    }
    EXPECT_EQ(results[0].local_id, 1U);
}

TEST(BruteForceIndex, ReportsOnlyLiveVectorsInItsSize) {
    VectorStore store = make_plane_store();
    const BruteForceIndex index(store.accessor(), Metric::kL2Squared);
    EXPECT_EQ(index.size(), 5U);

    store.remove(2);
    const BruteForceIndex after(store.accessor(), Metric::kL2Squared);
    EXPECT_EQ(after.size(), 4U);
}

TEST(BruteForceIndex, ReturnsNothingForAnEmptyStore) {
    const VectorStore store(4);
    const BruteForceIndex index(store.accessor(), Metric::kL2Squared);
    EXPECT_TRUE(index.search(std::vector<float>(4, 0.0F), SearchParams{.k = 5}).empty());
    EXPECT_EQ(index.size(), 0U);
}

TEST(BruteForceIndex, ReturnsNothingWhenEveryVectorIsTombstoned) {
    VectorStore store = make_plane_store();
    for (VectorId id = 0; id < 5; ++id) {
        store.remove(id);
    }
    const BruteForceIndex index(store.accessor(), Metric::kL2Squared);
    EXPECT_TRUE(index.search(std::vector<float>{0.0F, 0.0F}, SearchParams{.k = 3}).empty());
}

TEST(BruteForceIndex, RejectsAWrongDimensionQuery) {
    const VectorStore store = make_plane_store();
    const BruteForceIndex index(store.accessor(), Metric::kL2Squared);
    EXPECT_THROW(static_cast<void>(index.search(std::vector<float>{1.0F}, SearchParams{})),
                 DimensionMismatchError);
}

TEST(BruteForceIndex, RejectsAZeroK) {
    const VectorStore store = make_plane_store();
    const BruteForceIndex index(store.accessor(), Metric::kL2Squared);
    EXPECT_THROW(
        static_cast<void>(index.search(std::vector<float>{0.0F, 0.0F}, SearchParams{.k = 0})),
        InvalidArgumentError);
}

// Cosine ignores magnitude, which is exactly the property that distinguishes it
// from L2 and the one most likely to be broken by a caching mistake.
TEST(BruteForceIndex, CosineRanksByDirectionNotMagnitude) {
    VectorStore store(2);
    store.insert(0, std::vector<float>{100.0F, 0.0F});  // same direction, huge
    store.insert(1, std::vector<float>{0.1F, 0.1F});    // 45 degrees away, tiny
    store.insert(2, std::vector<float>{-1.0F, 0.0F});   // opposite

    const BruteForceIndex index(store.accessor(), Metric::kCosine);
    const std::vector<Candidate> results =
        index.search(std::vector<float>{1.0F, 0.0F}, SearchParams{.k = 3});

    ASSERT_EQ(results.size(), 3U);
    EXPECT_EQ(ids_of(results), (std::vector<LocalId>{0, 1, 2}));
    EXPECT_NEAR(results[0].key, 0.0F, 1e-5F) << "identical direction";
    EXPECT_NEAR(results[2].key, 2.0F, 1e-5F) << "opposite direction";
}

TEST(BruteForceIndex, CosineIsUnaffectedByWhetherTheStoreNormalizes) {
    const std::vector<std::vector<float>> data{
        {3.0F, 4.0F}, {1.0F, 0.0F}, {-2.0F, 5.0F}, {0.5F, 0.5F}};

    VectorStore raw(2, /*normalize=*/false);
    VectorStore normalized(2, /*normalize=*/true);
    for (VectorId id = 0; id < data.size(); ++id) {
        raw.insert(id, data[id]);
        normalized.insert(id, data[id]);
    }

    const BruteForceIndex raw_index(raw.accessor(), Metric::kCosine);
    const BruteForceIndex normalized_index(normalized.accessor(), Metric::kCosine);

    const std::vector<float> query{1.0F, 1.0F};
    const auto from_raw = raw_index.search(query, SearchParams{.k = 4});
    const auto from_normalized = normalized_index.search(query, SearchParams{.k = 4});

    ASSERT_EQ(from_raw.size(), from_normalized.size());
    for (std::size_t i = 0; i < from_raw.size(); ++i) {
        EXPECT_EQ(from_raw[i].local_id, from_normalized[i].local_id) << "position " << i;
        EXPECT_NEAR(from_raw[i].key, from_normalized[i].key, 1e-5F) << "position " << i;
    }
}

TEST(BruteForceIndex, InnerProductPrefersLargerMagnitude) {
    VectorStore store(2);
    store.insert(0, std::vector<float>{1.0F, 0.0F});
    store.insert(1, std::vector<float>{10.0F, 0.0F});

    const BruteForceIndex index(store.accessor(), Metric::kInnerProduct);
    const auto results = index.search(std::vector<float>{1.0F, 0.0F}, SearchParams{.k = 2});

    ASSERT_EQ(results.size(), 2U);
    EXPECT_EQ(results[0].local_id, 1U) << "dot product rewards magnitude";
    EXPECT_NEAR(score_from_rank_key(Metric::kInnerProduct, results[0].key), 10.0F, 1e-5F);
}

// The property that makes brute force usable as an oracle: it agrees with a
// full sort, exactly, on every metric.
TEST(BruteForceIndex, AgreesWithAFullSortOnGeneratedData) {
    const DatasetSpec spec{.dimension = 32, .count = 500, .seed = 20260829};
    const VectorArray data = generate_dataset(spec);
    const VectorArray queries = generate_queries(spec, 5);

    for (const Metric metric : {Metric::kL2Squared, Metric::kCosine, Metric::kInnerProduct}) {
        VectorStore store(spec.dimension);
        store.reserve(spec.count);
        for (LocalId id = 0; id < data.size(); ++id) {
            store.insert(id, data[id]);
        }

        const BruteForceIndex index(store.accessor(), metric, scalar_kernel());
        const DistanceFunction distance(metric, scalar_kernel());

        for (LocalId q = 0; q < queries.size(); ++q) {
            const VectorView query = queries[q];

            std::vector<Candidate> reference;
            reference.reserve(data.size());
            for (LocalId id = 0; id < data.size(); ++id) {
                reference.push_back(Candidate{id, distance(query, data[id])});
            }
            std::sort(reference.begin(), reference.end(), CandidateBetter{});
            reference.resize(10);

            const std::vector<Candidate> actual = index.search(query, SearchParams{.k = 10});
            ASSERT_EQ(actual.size(), 10U);
            EXPECT_EQ(ids_of(actual), ids_of(reference))
                << "metric " << metric_name(metric) << ", query " << q;
        }
    }
}

TEST(BruteForceIndex, SearchRangeCoversASliceAndComposesIntoTheWholeScan) {
    const DatasetSpec spec{.dimension = 16, .count = 200, .seed = 5};
    const VectorArray data = generate_dataset(spec);

    VectorStore store(spec.dimension);
    for (LocalId id = 0; id < data.size(); ++id) {
        store.insert(id, data[id]);
    }
    const BruteForceIndex index(store.accessor(), Metric::kL2Squared, scalar_kernel());

    // Held in a named variable: a view into a temporary would dangle.
    const VectorArray query_batch = generate_queries(spec, 1);
    const VectorView query = query_batch[0];
    const std::vector<Candidate> whole = index.search(query, SearchParams{.k = 7});

    // Four slices, as a parallel scan would produce, merged back together.
    std::vector<std::vector<Candidate>> partials;
    for (LocalId start = 0; start < 200; start += 50) {
        TopKCollector collector(7);
        index.search_range(query, start, start + 50, collector);
        partials.push_back(collector.take_sorted());
    }

    EXPECT_EQ(ids_of(merge_top_k(partials, 7)), ids_of(whole))
        << "a sliced scan must give exactly the same answer as a whole one";
}

TEST(BruteForceIndex, AddAndRemoveAreNoOpsBecauseTheStoreIsAuthoritative) {
    VectorStore store = make_plane_store();
    BruteForceIndex index(store.accessor(), Metric::kL2Squared);

    const std::size_t before = index.size();
    index.add(3);
    index.remove(3);
    EXPECT_EQ(index.size(), before) << "brute force holds no state to change";
    EXPECT_EQ(index.index_bytes(), 0U);
}

TEST(BruteForceIndex, DescribesItselfWithMetricAndKernel) {
    const VectorStore store(4);
    const BruteForceIndex index(store.accessor(), Metric::kCosine, scalar_kernel());
    const std::string description = index.describe();

    EXPECT_NE(description.find("brute_force"), std::string::npos);
    EXPECT_NE(description.find("cosine"), std::string::npos);
    EXPECT_NE(description.find("scalar"), std::string::npos);
    EXPECT_EQ(index.type(), IndexType::kBruteForce);
    EXPECT_EQ(index.metric(), Metric::kCosine);
}

}  // namespace
}  // namespace vectordb
