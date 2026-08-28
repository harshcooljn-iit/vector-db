// SPDX-License-Identifier: MIT
//
// Determinism is the whole contract here: a benchmark comparing macOS with
// Linux is meaningless unless "the same dataset" means the same bytes.
#include <cmath>
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include <vectordb/core/error.hpp>
#include <vectordb/util/dataset.hpp>

namespace vectordb {
namespace {

TEST(DeterministicRng, ProducesTheSameSequenceForTheSameSeed) {
    DeterministicRng a(12345);
    DeterministicRng b(12345);
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(a.next(), b.next()) << "draw " << i;
    }
}

TEST(DeterministicRng, ProducesDifferentSequencesForDifferentSeeds) {
    DeterministicRng a(1);
    DeterministicRng b(2);
    int differences = 0;
    for (int i = 0; i < 50; ++i) {
        if (a.next() != b.next()) {
            ++differences;
        }
    }
    EXPECT_GT(differences, 45);
}

// splitmix64 is fully specified by its four lines, so these values are fixed
// across every compiler and platform. If this test ever fails, the generator
// has changed and every recorded benchmark number refers to different data.
TEST(DeterministicRng, MatchesTheKnownSplitmix64Sequence) {
    DeterministicRng rng(0);
    EXPECT_EQ(rng.next(), 0xE220A8397B1DCDAFULL);
    EXPECT_EQ(rng.next(), 0x6E789E6AA1B965F4ULL);
    EXPECT_EQ(rng.next(), 0x06C45D188009454FULL);
}

TEST(DeterministicRng, UniformStaysInRange) {
    DeterministicRng rng(7);
    for (int i = 0; i < 10000; ++i) {
        const float value = rng.uniform();
        ASSERT_GE(value, 0.0F);
        ASSERT_LT(value, 1.0F);
    }
    for (int i = 0; i < 1000; ++i) {
        const float value = rng.uniform(-3.0F, 5.0F);
        ASSERT_GE(value, -3.0F);
        ASSERT_LT(value, 5.0F);
    }
}

TEST(DeterministicRng, NormalHasRoughlyTheRightMomentsAndNoNaN) {
    DeterministicRng rng(99);
    double sum = 0.0;
    double sum_squares = 0.0;
    constexpr int kDraws = 100000;

    for (int i = 0; i < kDraws; ++i) {
        const float value = rng.normal();
        ASSERT_TRUE(std::isfinite(value)) << "draw " << i;
        sum += static_cast<double>(value);
        sum_squares += static_cast<double>(value) * static_cast<double>(value);
    }

    const double mean = sum / kDraws;
    const double variance = sum_squares / kDraws - mean * mean;
    EXPECT_NEAR(mean, 0.0, 0.02);
    EXPECT_NEAR(variance, 1.0, 0.05);
}

TEST(GenerateDataset, IsReproducibleBitForBit) {
    const DatasetSpec spec{.dimension = 16, .count = 100, .seed = 4242};
    const VectorArray a = generate_dataset(spec);
    const VectorArray b = generate_dataset(spec);

    ASSERT_EQ(a.size(), b.size());
    for (LocalId id = 0; id < a.size(); ++id) {
        for (Dimension d = 0; d < spec.dimension; ++d) {
            // Exact equality: these are copies of the same computation, so any
            // difference is a real bug rather than accumulated drift.
            ASSERT_EQ(a[id][d], b[id][d]) << "vector " << id << " component " << d;
        }
    }
}

TEST(GenerateDataset, ChangingTheSeedChangesTheData) {
    DatasetSpec spec{.dimension = 8, .count = 10, .seed = 1};
    const VectorArray a = generate_dataset(spec);
    spec.seed = 2;
    const VectorArray b = generate_dataset(spec);

    bool any_difference = false;
    for (LocalId id = 0; id < a.size() && !any_difference; ++id) {
        for (Dimension d = 0; d < 8; ++d) {
            if (a[id][d] != b[id][d]) {
                any_difference = true;
                break;
            }
        }
    }
    EXPECT_TRUE(any_difference);
}

TEST(GenerateDataset, ProducesTheRequestedShapeAndOnlyFiniteValues) {
    for (const DatasetKind kind :
         {DatasetKind::kUniform, DatasetKind::kGaussian, DatasetKind::kClustered}) {
        const DatasetSpec spec{.dimension = 24, .count = 300, .seed = 11, .kind = kind};
        const VectorArray data = generate_dataset(spec);

        ASSERT_EQ(data.size(), 300U);
        ASSERT_EQ(data.dimension(), 24U);
        for (LocalId id = 0; id < data.size(); ++id) {
            ASSERT_TRUE(is_finite(data[id])) << "kind " << static_cast<int>(kind);
        }
    }
}

TEST(GenerateDataset, UniformStaysInsideItsStatedRange) {
    const DatasetSpec spec{
        .dimension = 8, .count = 200, .seed = 3, .kind = DatasetKind::kUniform};
    const VectorArray data = generate_dataset(spec);

    for (LocalId id = 0; id < data.size(); ++id) {
        for (const float value : data[id]) {
            ASSERT_GE(value, -1.0F);
            ASSERT_LT(value, 1.0F);
        }
    }
}

// The property that makes clustered data the right default: real nearest
// neighbours exist. Under uniform data in high dimensions the nearest and
// furthest neighbours are nearly the same distance away, and an ANN index
// measured on it looks far worse than it is on real embeddings.
TEST(GenerateDataset, ClusteredDataHasMuchCloserNeighboursThanUniformData) {
    constexpr Dimension kDimension = 64;
    constexpr std::size_t kCount = 400;

    const auto nearest_over_mean = [](const VectorArray& data) {
        double nearest_total = 0.0;
        double mean_total = 0.0;
        for (LocalId i = 0; i < 50; ++i) {
            double nearest = 1e30;
            double sum = 0.0;
            for (LocalId j = 0; j < data.size(); ++j) {
                if (i == j) {
                    continue;
                }
                double squared = 0.0;
                for (Dimension d = 0; d < data.dimension(); ++d) {
                    const double diff =
                        static_cast<double>(data[i][d]) - static_cast<double>(data[j][d]);
                    squared += diff * diff;
                }
                nearest = std::min(nearest, squared);
                sum += squared;
            }
            nearest_total += nearest;
            mean_total += sum / static_cast<double>(data.size() - 1);
        }
        return nearest_total / mean_total;
    };

    const VectorArray clustered = generate_dataset(
        {.dimension = kDimension, .count = kCount, .seed = 1, .kind = DatasetKind::kClustered});
    const VectorArray uniform = generate_dataset(
        {.dimension = kDimension, .count = kCount, .seed = 1, .kind = DatasetKind::kUniform});

    const double clustered_ratio = nearest_over_mean(clustered);
    const double uniform_ratio = nearest_over_mean(uniform);

    EXPECT_LT(clustered_ratio, uniform_ratio * 0.5)
        << "clustered data must have genuinely near neighbours; "
        << "clustered ratio " << clustered_ratio << " vs uniform " << uniform_ratio;
    // Uniform data in 64 dimensions: everything is roughly equidistant.
    EXPECT_GT(uniform_ratio, 0.5);
}

TEST(GenerateQueries, FollowsTheDatasetDistributionWithoutBeingMembers) {
    const DatasetSpec spec{.dimension = 12, .count = 50, .seed = 8};
    const VectorArray data = generate_dataset(spec);
    const VectorArray queries = generate_queries(spec, 10);

    ASSERT_EQ(queries.size(), 10U);
    ASSERT_EQ(queries.dimension(), 12U);

    for (LocalId q = 0; q < queries.size(); ++q) {
        for (LocalId d = 0; d < data.size(); ++d) {
            bool identical = true;
            for (Dimension c = 0; c < 12; ++c) {
                if (queries[q][c] != data[d][c]) {
                    identical = false;
                    break;
                }
            }
            ASSERT_FALSE(identical) << "query " << q << " duplicates dataset vector " << d;
        }
    }
}

TEST(GenerateQueries, IsReproducible) {
    const DatasetSpec spec{.dimension = 4, .count = 20, .seed = 77};
    const VectorArray a = generate_queries(spec, 5);
    const VectorArray b = generate_queries(spec, 5);
    for (LocalId id = 0; id < a.size(); ++id) {
        for (Dimension d = 0; d < 4; ++d) {
            EXPECT_EQ(a[id][d], b[id][d]);
        }
    }
}

TEST(GenerateDataset, RejectsAnEmptyDataset) {
    EXPECT_THROW(static_cast<void>(generate_dataset({.dimension = 4, .count = 0})),
                 InvalidArgumentError);
}

}  // namespace
}  // namespace vectordb
