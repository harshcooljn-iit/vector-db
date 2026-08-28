// SPDX-License-Identifier: MIT
#include <array>
#include <numeric>
#include <vector>

#include <gtest/gtest.h>

#include <vectordb/core/error.hpp>
#include <vectordb/core/vector_array.hpp>

namespace vectordb {
namespace {

std::vector<float> ramp(Dimension dimension, float start) {
    std::vector<float> values(dimension);
    std::iota(values.begin(), values.end(), start);
    return values;
}

TEST(VectorArray, RejectsDegenerateDimensions) {
    EXPECT_THROW(VectorArray{0}, InvalidArgumentError);
    EXPECT_THROW(VectorArray{kMaxDimension + 1}, InvalidArgumentError);
    EXPECT_NO_THROW(VectorArray{1});
    EXPECT_NO_THROW(VectorArray{kMaxDimension});
}

TEST(VectorArray, StartsEmpty) {
    const VectorArray array(8);
    EXPECT_EQ(array.size(), 0U);
    EXPECT_TRUE(array.empty());
    EXPECT_EQ(array.dimension(), 8U);
    EXPECT_EQ(array.payload_bytes(), 0U);
}

TEST(VectorArray, AssignsConsecutiveSlotsAndReadsThemBack) {
    VectorArray array(3);

    EXPECT_EQ(array.push_back(std::array<float, 3>{1.0F, 2.0F, 3.0F}), 0U);
    EXPECT_EQ(array.push_back(std::array<float, 3>{4.0F, 5.0F, 6.0F}), 1U);
    EXPECT_EQ(array.push_back(std::array<float, 3>{7.0F, 8.0F, 9.0F}), 2U);

    ASSERT_EQ(array.size(), 3U);
    EXPECT_EQ(array[1][0], 4.0F);
    EXPECT_EQ(array[1][2], 6.0F);
    EXPECT_EQ(array.at(2)[1], 8.0F);
}

// The single most important property of this class: vector i must literally sit
// at data() + i * dimension(). Everything downstream — the file format, mmap,
// the SIMD kernels, the prefetcher-friendly scan — depends on it.
TEST(VectorArray, StoresVectorsBackToBackInOneContiguousBlock) {
    VectorArray array(4);
    array.push_back(std::array<float, 4>{0.0F, 1.0F, 2.0F, 3.0F});
    array.push_back(std::array<float, 4>{4.0F, 5.0F, 6.0F, 7.0F});
    array.push_back(std::array<float, 4>{8.0F, 9.0F, 10.0F, 11.0F});

    const float* base = array.data();
    for (std::size_t i = 0; i < 12; ++i) {
        EXPECT_EQ(base[i], static_cast<float>(i)) << "flat offset " << i;
    }

    for (LocalId id = 0; id < 3; ++id) {
        EXPECT_EQ(array[id].data(), base + static_cast<std::size_t>(id) * 4)
            << "vector " << id << " must be at a fixed stride from the base";
    }

    EXPECT_EQ(array.stride(), array.dimension());
    EXPECT_EQ(array.payload_bytes(), 3U * 4U * sizeof(float));
}

TEST(VectorArray, RejectsAWrongLengthVector) {
    VectorArray array(4);
    const std::array<float, 3> too_short{1.0F, 2.0F, 3.0F};
    EXPECT_THROW(array.push_back(too_short), DimensionMismatchError);
    EXPECT_EQ(array.size(), 0U) << "a rejected push must not change the array";
}

TEST(VectorArray, AssignOverwritesInPlaceWithoutGrowing) {
    VectorArray array(2);
    array.push_back(std::array<float, 2>{1.0F, 1.0F});
    array.push_back(std::array<float, 2>{2.0F, 2.0F});

    array.assign(0, std::array<float, 2>{9.0F, 8.0F});

    EXPECT_EQ(array.size(), 2U);
    EXPECT_EQ(array[0][0], 9.0F);
    EXPECT_EQ(array[0][1], 8.0F);
    EXPECT_EQ(array[1][0], 2.0F) << "the neighbouring slot must be untouched";
}

TEST(VectorArray, AssignAndAtRejectOutOfRangeSlots) {
    VectorArray array(2);
    array.push_back(std::array<float, 2>{1.0F, 1.0F});

    EXPECT_THROW(static_cast<void>(array.at(1)), InvalidArgumentError);
    EXPECT_THROW(static_cast<void>(array.mutable_at(7)), InvalidArgumentError);
    EXPECT_THROW(array.assign(1, std::array<float, 2>{0.0F, 0.0F}), InvalidArgumentError);
}

TEST(VectorArray, AppendUninitializedZeroFillsAndIsWritable) {
    VectorArray array(3);
    const LocalId first = array.append_uninitialized(2);

    EXPECT_EQ(first, 0U);
    ASSERT_EQ(array.size(), 2U);
    // Documented as zero-filled rather than indeterminate, so that a partially
    // populated array is never a source of UB.
    for (LocalId id = 0; id < 2; ++id) {
        for (const float value : array[id]) {
            EXPECT_EQ(value, 0.0F);
        }
    }

    const MutableVectorView slot = array.mutable_at(1);
    slot[0] = 42.0F;
    EXPECT_EQ(array[1][0], 42.0F);
    EXPECT_EQ(array[0][0], 0.0F);
}

TEST(VectorArray, AppendUninitializedContinuesAfterExistingVectors) {
    VectorArray array(2);
    array.push_back(std::array<float, 2>{1.0F, 2.0F});

    EXPECT_EQ(array.append_uninitialized(3), 1U);
    EXPECT_EQ(array.size(), 4U);
    EXPECT_EQ(array[0][0], 1.0F) << "existing data must survive the append";
}

TEST(VectorArray, ReserveAvoidsReallocationForABulkLoad) {
    VectorArray array(16);
    array.reserve(1000);

    const std::size_t allocated_after_reserve = array.allocated_bytes();
    const float* base_after_reserve = array.data();

    for (int i = 0; i < 1000; ++i) {
        array.push_back(ramp(16, static_cast<float>(i)));
    }

    EXPECT_EQ(array.size(), 1000U);
    EXPECT_EQ(array.data(), base_after_reserve)
        << "reserve(1000) must make 1000 push_backs allocation-free";
    EXPECT_EQ(array.allocated_bytes(), allocated_after_reserve);
    EXPECT_EQ(array.payload_bytes(), 1000U * 16U * sizeof(float));
}

TEST(VectorArray, ClearKeepsCapacityAndShrinkReleasesIt) {
    VectorArray array(8);
    array.reserve(500);
    for (int i = 0; i < 100; ++i) {
        array.push_back(ramp(8, static_cast<float>(i)));
    }

    const std::size_t reserved = array.allocated_bytes();
    array.clear();

    EXPECT_EQ(array.size(), 0U);
    EXPECT_TRUE(array.empty());
    EXPECT_EQ(array.allocated_bytes(), reserved) << "clear() must keep the allocation";

    array.shrink_to_fit();
    EXPECT_LT(array.allocated_bytes(), reserved);
}

TEST(VectorArray, RoundTripsAThousandVectorsExactly) {
    constexpr Dimension kDimension = 32;
    VectorArray array(kDimension);

    for (int i = 0; i < 1000; ++i) {
        array.push_back(ramp(kDimension, static_cast<float>(i) * 100.0F));
    }

    ASSERT_EQ(array.size(), 1000U);
    for (LocalId id = 0; id < 1000; ++id) {
        const VectorView stored = array[id];
        ASSERT_EQ(stored.size(), kDimension);
        for (Dimension component = 0; component < kDimension; ++component) {
            const float expected =
                static_cast<float>(id) * 100.0F + static_cast<float>(component);
            // Floats are copied, never computed on, so exact equality is the
            // right assertion here: any difference is a real bug, not drift.
            EXPECT_EQ(stored[component], expected) << "id " << id << " comp " << component;
        }
    }
}

}  // namespace
}  // namespace vectordb
