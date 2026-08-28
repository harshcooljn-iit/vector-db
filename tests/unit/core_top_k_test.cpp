// SPDX-License-Identifier: MIT
#include <algorithm>
#include <numeric>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include <vectordb/core/error.hpp>
#include <vectordb/core/top_k.hpp>

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

TEST(TopKCollector, RejectsAZeroK) {
    EXPECT_THROW(TopKCollector{0}, InvalidArgumentError);
}

TEST(TopKCollector, StartsEmptyAndUnbounded) {
    const TopKCollector collector(5);
    EXPECT_TRUE(collector.empty());
    EXPECT_FALSE(collector.full());
    EXPECT_EQ(collector.size(), 0U);
    EXPECT_EQ(collector.k(), 5U);
    // +inf so that an unfilled heap accepts everything through the same code
    // path a full one uses, with no special case.
    EXPECT_TRUE(std::isinf(collector.worst()));
    EXPECT_TRUE(collector.would_accept(1e30F));
}

TEST(TopKCollector, KeepsTheKBestAndDiscardsTheRest) {
    TopKCollector collector(3);
    // Offered worst-first so that every insertion has to displace something.
    collector.offer(0, 9.0F);
    collector.offer(1, 8.0F);
    collector.offer(2, 7.0F);
    collector.offer(3, 1.0F);
    collector.offer(4, 5.0F);
    collector.offer(5, 100.0F);

    ASSERT_TRUE(collector.full());
    EXPECT_EQ(collector.size(), 3U);

    const std::vector<Candidate> sorted = collector.take_sorted();
    ASSERT_EQ(sorted.size(), 3U);
    EXPECT_EQ(ids_of(sorted), (std::vector<LocalId>{3, 4, 2}));
    EXPECT_FLOAT_EQ(sorted[0].key, 1.0F);
    EXPECT_FLOAT_EQ(sorted[1].key, 5.0F);
    EXPECT_FLOAT_EQ(sorted[2].key, 7.0F);
}

TEST(TopKCollector, ReturnsBestFirst) {
    TopKCollector collector(4);
    for (const float key : {5.0F, 1.0F, 4.0F, 2.0F, 3.0F}) {
        collector.offer(static_cast<LocalId>(key), key);
    }

    const std::vector<Candidate> sorted = collector.take_sorted();
    ASSERT_EQ(sorted.size(), 4U);
    for (std::size_t i = 1; i < sorted.size(); ++i) {
        EXPECT_LE(sorted[i - 1].key, sorted[i].key) << "must be ascending by rank key";
    }
}

TEST(TopKCollector, ReturnsEverythingWhenFewerThanKAreOffered) {
    TopKCollector collector(10);
    collector.offer(7, 2.0F);
    collector.offer(8, 1.0F);

    const std::vector<Candidate> sorted = collector.take_sorted();
    ASSERT_EQ(sorted.size(), 2U);
    EXPECT_EQ(sorted[0].local_id, 8U);
    EXPECT_EQ(sorted[1].local_id, 7U);
}

TEST(TopKCollector, WorstTracksTheThresholdOnceFull) {
    TopKCollector collector(2);
    EXPECT_TRUE(std::isinf(collector.worst()));

    collector.offer(0, 5.0F);
    EXPECT_TRUE(std::isinf(collector.worst())) << "still unbounded while not full";

    collector.offer(1, 3.0F);
    EXPECT_FLOAT_EQ(collector.worst(), 5.0F);

    collector.offer(2, 1.0F);
    EXPECT_FLOAT_EQ(collector.worst(), 3.0F) << "the 5.0 entry must have been displaced";
}

// The pre-filter must never say no to something offer() would keep, or two
// callers — one using it to skip work, one not — would return different results.
TEST(TopKCollector, WouldAcceptIsConservativeAboutTies) {
    TopKCollector collector(2);
    collector.offer(10, 4.0F);
    collector.offer(11, 4.0F);

    EXPECT_TRUE(collector.would_accept(4.0F))
        << "a tie can still win on local_id, so the pre-filter must allow it";
    EXPECT_TRUE(collector.would_accept(3.9F));
    EXPECT_FALSE(collector.would_accept(4.1F));

    // And the tie really is decided by local_id.
    collector.offer(5, 4.0F);
    const std::vector<Candidate> sorted = collector.take_sorted();
    EXPECT_EQ(ids_of(sorted), (std::vector<LocalId>{5, 10}));
}

// Determinism is what makes recall comparisons against brute force meaningful.
// Without it, two runs can return equally-good-but-different sets and the
// difference is misread as an index defect.
TEST(TopKCollector, ResultIsIndependentOfOfferOrder) {
    std::vector<Candidate> input;
    for (LocalId id = 0; id < 50; ++id) {
        // Deliberately many ties: 10 distinct keys across 50 candidates.
        input.push_back(Candidate{id, static_cast<float>(id % 10)});
    }

    std::vector<LocalId> reference;
    std::mt19937 rng(4242);
    for (int trial = 0; trial < 20; ++trial) {
        std::shuffle(input.begin(), input.end(), rng);

        TopKCollector collector(7);
        for (const Candidate& candidate : input) {
            collector.offer(candidate.local_id, candidate.key);
        }
        const std::vector<LocalId> ids = ids_of(collector.take_sorted());

        if (trial == 0) {
            reference = ids;
        } else {
            EXPECT_EQ(ids, reference) << "shuffling the input must not change the result";
        }
    }

    // Keys 0 and 1 appear at ids {0,10,20,30,40} and {1,11,...}; the seven best
    // under (key, then local_id) are the five zeros then the two smallest ones.
    EXPECT_EQ(reference, (std::vector<LocalId>{0, 10, 20, 30, 40, 1, 11}));
}

TEST(TopKCollector, AgreesWithAFullSortOnRandomData) {
    constexpr std::size_t kCount = 5000;
    constexpr std::size_t kK = 25;

    std::mt19937 rng(20260829);
    std::uniform_real_distribution<float> dist(-1000.0F, 1000.0F);

    std::vector<Candidate> all;
    all.reserve(kCount);
    TopKCollector collector(kK);
    for (LocalId id = 0; id < kCount; ++id) {
        const float key = dist(rng);
        all.push_back(Candidate{id, key});
        collector.offer(id, key);
    }

    std::sort(all.begin(), all.end(), CandidateBetter{});
    all.resize(kK);

    const std::vector<Candidate> heaped = collector.take_sorted();
    ASSERT_EQ(heaped.size(), kK);
    for (std::size_t i = 0; i < kK; ++i) {
        EXPECT_EQ(heaped[i].local_id, all[i].local_id) << "position " << i;
        EXPECT_FLOAT_EQ(heaped[i].key, all[i].key) << "position " << i;
    }
}

TEST(TopKCollector, ClearKeepsCapacitySoABatchReusesOneCollector) {
    TopKCollector collector(3);
    collector.offer(0, 1.0F);
    collector.offer(1, 2.0F);
    collector.clear();

    EXPECT_TRUE(collector.empty());
    EXPECT_EQ(collector.k(), 3U);
    EXPECT_TRUE(std::isinf(collector.worst()));

    collector.offer(9, 7.0F);
    EXPECT_EQ(collector.size(), 1U);
}

TEST(TopKCollector, ResetChangesK) {
    TopKCollector collector(2);
    collector.offer(0, 1.0F);
    collector.reset(5);

    EXPECT_EQ(collector.k(), 5U);
    EXPECT_TRUE(collector.empty());
    EXPECT_THROW(collector.reset(0), InvalidArgumentError);
}

TEST(TopKCollector, TakeSortedLeavesTheCollectorReusable) {
    TopKCollector collector(2);
    collector.offer(0, 1.0F);
    collector.offer(1, 2.0F);

    EXPECT_EQ(collector.take_sorted().size(), 2U);
    EXPECT_TRUE(collector.empty());

    collector.offer(3, 9.0F);
    EXPECT_EQ(collector.size(), 1U);
}

// ---------------------------------------------------------------------------
// merge_top_k — combining per-thread results
// ---------------------------------------------------------------------------

TEST(MergeTopK, CombinesSortedListsIntoTheOverallBest) {
    const std::vector<std::vector<Candidate>> lists{
        {{0, 1.0F}, {1, 4.0F}, {2, 9.0F}},
        {{3, 2.0F}, {4, 5.0F}},
        {{5, 0.5F}, {6, 3.0F}, {7, 6.0F}},
    };

    const std::vector<Candidate> merged = merge_top_k(lists, 4);
    ASSERT_EQ(merged.size(), 4U);
    EXPECT_EQ(ids_of(merged), (std::vector<LocalId>{5, 0, 3, 6}));
}

TEST(MergeTopK, HandlesEmptyAndShortInputs) {
    EXPECT_TRUE(merge_top_k({}, 5).empty());
    EXPECT_TRUE(merge_top_k({{}, {}}, 5).empty());

    const std::vector<std::vector<Candidate>> one{{{7, 1.0F}}};
    const std::vector<Candidate> merged = merge_top_k(one, 5);
    ASSERT_EQ(merged.size(), 1U);
    EXPECT_EQ(merged[0].local_id, 7U);
}

TEST(MergeTopK, RejectsAZeroK) {
    EXPECT_THROW(static_cast<void>(merge_top_k({{{0, 1.0F}}}, 0)), InvalidArgumentError);
}

TEST(MergeTopK, MatchesASingleCollectorOverTheSameCandidates) {
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> dist(0.0F, 100.0F);

    constexpr std::size_t kK = 10;
    std::vector<std::vector<Candidate>> lists(4);
    TopKCollector reference(kK);

    LocalId next_id = 0;
    for (std::vector<Candidate>& list : lists) {
        TopKCollector partial(kK);
        for (int i = 0; i < 200; ++i) {
            const float key = dist(rng);
            partial.offer(next_id, key);
            reference.offer(next_id, key);
            ++next_id;
        }
        list = partial.take_sorted();
    }

    EXPECT_EQ(ids_of(merge_top_k(lists, kK)), ids_of(reference.take_sorted()));
}

// ---------------------------------------------------------------------------
// SearchResult translation
// ---------------------------------------------------------------------------

TEST(ToSearchResults, ConvertsRankKeysToMetricNativeScores) {
    const std::vector<Candidate> candidates{{0, 0.0F}, {1, 1.0F}, {2, 2.0F}};
    const auto to_id = [](LocalId local) { return static_cast<VectorId>(local) + 1000; };

    const std::vector<SearchResult> cosine =
        to_search_results(candidates, Metric::kCosine, to_id);
    ASSERT_EQ(cosine.size(), 3U);
    EXPECT_EQ(cosine[0].id, 1000U);
    EXPECT_FLOAT_EQ(cosine[0].score, 1.0F) << "rank key 0 is similarity 1";
    EXPECT_FLOAT_EQ(cosine[1].score, 0.0F);
    EXPECT_FLOAT_EQ(cosine[2].score, -1.0F);

    const std::vector<SearchResult> l2 =
        to_search_results(candidates, Metric::kL2Squared, to_id);
    EXPECT_FLOAT_EQ(l2[2].score, 2.0F) << "l2 scores pass through unchanged";
}

}  // namespace
}  // namespace vectordb
