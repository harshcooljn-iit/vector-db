// SPDX-License-Identifier: MIT
#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include <vectordb/core/error.hpp>
#include <vectordb/storage/vector_store.hpp>

namespace vectordb {
namespace {

constexpr float kTolerance = 1e-5F;

std::vector<float> vec(std::initializer_list<float> values) {
    return std::vector<float>(values);
}

TEST(VectorStore, StartsEmpty) {
    const VectorStore store(4);
    EXPECT_EQ(store.live_count(), 0U);
    EXPECT_EQ(store.slot_count(), 0U);
    EXPECT_EQ(store.tombstone_count(), 0U);
    EXPECT_DOUBLE_EQ(store.tombstone_ratio(), 0.0);
    EXPECT_TRUE(store.empty());
    EXPECT_EQ(store.dimension(), 4U);
    EXPECT_FALSE(store.normalizes());
}

TEST(VectorStore, InsertAssignsAscendingSlotsAndRemembersIds) {
    VectorStore store(2);
    EXPECT_EQ(store.insert(100, vec({1.0F, 2.0F})), 0U);
    EXPECT_EQ(store.insert(7, vec({3.0F, 4.0F})), 1U);
    EXPECT_EQ(store.insert(9999, vec({5.0F, 6.0F})), 2U);

    EXPECT_EQ(store.live_count(), 3U);
    EXPECT_EQ(store.find(7).value(), 1U);
    EXPECT_EQ(store.vector_id(1), 7U);
    EXPECT_EQ(store.get(9999)[1], 6.0F);
}

TEST(VectorStore, RejectsADuplicateIdAndLeavesTheStoreUnchanged) {
    VectorStore store(2);
    store.insert(1, vec({1.0F, 1.0F}));

    EXPECT_THROW(store.insert(1, vec({9.0F, 9.0F})), DuplicateIdError);
    EXPECT_EQ(store.slot_count(), 1U);
    EXPECT_EQ(store.get(1)[0], 1.0F) << "the original vector must survive";
}

TEST(VectorStore, RejectsAWrongDimensionWithoutMutating) {
    VectorStore store(3);
    EXPECT_THROW(store.insert(1, vec({1.0F, 2.0F})), DimensionMismatchError);
    EXPECT_EQ(store.slot_count(), 0U) << "a rejected insert must leave no trace";
}

// A partially-applied insert would be far worse than a rejected one: a caller
// that catches and retries would then be operating on a half-mutated store.
TEST(VectorStore, RejectsANonFiniteVectorWithoutMutating) {
    VectorStore store(2);
    store.insert(1, vec({1.0F, 1.0F}));

    const auto nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_THROW(store.insert(2, vec({nan, 0.0F})), InvalidVectorError);

    EXPECT_EQ(store.slot_count(), 1U);
    EXPECT_EQ(store.live_count(), 1U);
    EXPECT_FALSE(store.contains(2));
}

TEST(VectorStore, UpsertInsertsThenOverwritesInPlace) {
    VectorStore store(2);
    const LocalId first = store.upsert(5, vec({1.0F, 0.0F}));
    const LocalId again = store.upsert(5, vec({0.0F, 2.0F}));

    EXPECT_EQ(first, again) << "an overwrite must not consume a new slot";
    EXPECT_EQ(store.slot_count(), 1U);
    EXPECT_EQ(store.get(5)[1], 2.0F);
    EXPECT_NEAR(store.norm(first), 2.0F, kTolerance) << "the cached norm must be updated";
}

TEST(VectorStore, CachesTheL2NormOnInsert) {
    VectorStore store(2);
    const LocalId id = store.insert(1, vec({3.0F, 4.0F}));
    EXPECT_NEAR(store.norm(id), 5.0F, kTolerance);
}

// ---------------------------------------------------------------------------
// Deletion
// ---------------------------------------------------------------------------

TEST(VectorStore, RemoveTombstonesRatherThanCompacting) {
    VectorStore store(1);
    store.insert(10, vec({1.0F}));
    store.insert(20, vec({2.0F}));
    store.insert(30, vec({3.0F}));

    EXPECT_TRUE(store.remove(20));

    EXPECT_EQ(store.live_count(), 2U);
    EXPECT_EQ(store.slot_count(), 3U) << "the slot must stay, so later ids keep theirs";
    EXPECT_EQ(store.tombstone_count(), 1U);
    EXPECT_FALSE(store.live(1));
    EXPECT_TRUE(store.live(0));
    EXPECT_TRUE(store.live(2));

    EXPECT_FALSE(store.contains(20));
    EXPECT_FALSE(store.find(20).has_value());
    EXPECT_THROW(static_cast<void>(store.get(20)), NotFoundError);

    // The critical property: deleting 20 must not renumber 30.
    EXPECT_EQ(store.find(30).value(), 2U);
}

TEST(VectorStore, RemoveIsIdempotentAndReportsWhetherItDidAnything) {
    VectorStore store(1);
    store.insert(1, vec({1.0F}));

    EXPECT_TRUE(store.remove(1));
    EXPECT_FALSE(store.remove(1));
    EXPECT_FALSE(store.remove(12345));
    EXPECT_EQ(store.live_count(), 0U);
}

TEST(VectorStore, ADeletedIdMayBeInsertedAgainIntoAFreshSlot) {
    VectorStore store(1);
    store.insert(42, vec({1.0F}));
    store.remove(42);

    const LocalId reused = store.insert(42, vec({7.0F}));
    EXPECT_EQ(reused, 1U) << "a new slot, not the tombstoned one";
    EXPECT_EQ(store.slot_count(), 2U);
    EXPECT_EQ(store.get(42)[0], 7.0F);
}

TEST(VectorStore, TombstonedSlotsStillReportTheirOriginalId) {
    VectorStore store(1);
    store.insert(77, vec({1.0F}));
    store.remove(77);
    // A rebuild needs to say *which* vector a bad slot belonged to.
    EXPECT_EQ(store.vector_id(0), 77U);
}

TEST(VectorStore, ReportsTheTombstoneRatio) {
    VectorStore store(1);
    for (VectorId id = 0; id < 4; ++id) {
        store.insert(id, vec({static_cast<float>(id)}));
    }
    store.remove(0);
    store.remove(2);
    EXPECT_DOUBLE_EQ(store.tombstone_ratio(), 0.5);
}

// ---------------------------------------------------------------------------
// Normalization
// ---------------------------------------------------------------------------

TEST(VectorStore, NormalizingStoreScalesOnInsertAndCachesANormOfOne) {
    VectorStore store(2, /*normalize=*/true);
    const LocalId id = store.insert(1, vec({3.0F, 4.0F}));

    EXPECT_TRUE(store.normalizes());
    EXPECT_NEAR(store.get(1)[0], 0.6F, kTolerance);
    EXPECT_NEAR(store.get(1)[1], 0.8F, kTolerance);
    // The cached norm becoming exactly 1 is what collapses cosine into a dot
    // product; that is the entire reason normalization is offered.
    EXPECT_NEAR(store.norm(id), 1.0F, kTolerance);
}

TEST(VectorStore, NormalizingStoreLeavesAZeroVectorAloneWithNormZero) {
    VectorStore store(3, /*normalize=*/true);
    const LocalId id = store.insert(1, vec({0.0F, 0.0F, 0.0F}));

    for (const float value : store.get(1)) {
        EXPECT_FALSE(std::isnan(value));
        EXPECT_EQ(value, 0.0F);
    }
    EXPECT_EQ(store.norm(id), 0.0F);
}

// ---------------------------------------------------------------------------
// Accessor and bookkeeping
// ---------------------------------------------------------------------------

TEST(VectorStore, AccessorSeesTheSameDataAndLiveness) {
    VectorStore store(2);
    store.insert(1, vec({3.0F, 4.0F}));
    store.insert(2, vec({1.0F, 0.0F}));
    store.remove(1);

    const VectorAccessor accessor = store.accessor();
    EXPECT_EQ(accessor.size(), 2U);
    EXPECT_EQ(accessor.dimension(), 2U);
    EXPECT_FALSE(accessor.live(0));
    EXPECT_TRUE(accessor.live(1));
    EXPECT_NEAR(accessor.norm(0), 5.0F, kTolerance);
    EXPECT_EQ(accessor.vector(1)[0], 1.0F);
}

TEST(VectorStore, RejectsOutOfRangeSlotLookups) {
    VectorStore store(1);
    store.insert(1, vec({1.0F}));
    EXPECT_THROW(static_cast<void>(store.vector_id(1)), InvalidArgumentError);
    EXPECT_THROW(static_cast<void>(store.vector(5)), InvalidArgumentError);
}

TEST(VectorStore, ClearDropsEverythingButKeepsConfiguration) {
    VectorStore store(3, /*normalize=*/true);
    store.insert(1, vec({1.0F, 0.0F, 0.0F}));
    store.clear();

    EXPECT_EQ(store.slot_count(), 0U);
    EXPECT_EQ(store.live_count(), 0U);
    EXPECT_EQ(store.dimension(), 3U);
    EXPECT_TRUE(store.normalizes());
    EXPECT_FALSE(store.contains(1));
}

TEST(VectorStore, ReportsANonZeroMemoryFootprintOnceItHoldsData) {
    VectorStore store(128);
    const std::size_t empty_bytes = store.memory_bytes();

    store.reserve(1000);
    for (VectorId id = 0; id < 1000; ++id) {
        store.insert(id, std::vector<float>(128, static_cast<float>(id)));
    }

    EXPECT_GT(store.memory_bytes(), empty_bytes);
    // Payload alone is 1000 * 128 * 4 = 512,000 bytes; the total must exceed it.
    EXPECT_GT(store.memory_bytes(), 1000U * 128U * sizeof(float));
}

TEST(AppendTrusted, RestoresSlotsIncludingTombstonesWithoutRevalidating) {
    VectorStore store(2);
    store.append_trusted(10, vec({1.0F, 0.0F}), 1.0F, /*live=*/true);
    store.append_trusted(20, vec({0.0F, 2.0F}), 2.0F, /*live=*/false);
    store.append_trusted(30, vec({3.0F, 4.0F}), 5.0F, /*live=*/true);

    EXPECT_EQ(store.slot_count(), 3U);
    EXPECT_EQ(store.live_count(), 2U);
    EXPECT_FALSE(store.live(1));
    // A tombstoned slot must not be findable by id, or a reload would resurrect
    // a deleted vector.
    EXPECT_FALSE(store.contains(20));
    EXPECT_EQ(store.find(30).value(), 2U);
    EXPECT_NEAR(store.norm(2), 5.0F, kTolerance);
}

}  // namespace
}  // namespace vectordb
