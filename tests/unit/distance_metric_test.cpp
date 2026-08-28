// SPDX-License-Identifier: MIT
//
// Metric-level tests: the ordering convention, the score conversions, and the
// edge cases that would otherwise put a NaN into a comparison.
#include <array>
#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include <vectordb/distance/distance.hpp>

namespace vectordb {
namespace {

constexpr float kTolerance = 1e-5F;

// ---------------------------------------------------------------------------
// The ordering convention. If any of these fail, every ranking structure in
// the engine is silently wrong, because they all assume smaller-is-better.
// ---------------------------------------------------------------------------

TEST(RankKeyOrdering, SmallerIsBetterForEveryMetric) {
    const std::array<float, 3> query{1.0F, 0.0F, 0.0F};
    const std::array<float, 3> near{0.9F, 0.1F, 0.0F};
    const std::array<float, 3> far{-1.0F, 0.0F, 0.0F};

    for (const Metric metric : {Metric::kL2Squared, Metric::kCosine, Metric::kInnerProduct}) {
        const DistanceFunction distance(metric);
        EXPECT_LT(distance(query, near), distance(query, far))
            << "metric " << metric_name(metric)
            << ": the closer vector must have the smaller rank key";
    }
}

TEST(RankKeyOrdering, IdenticalVectorsRankBestForEveryMetric) {
    const std::array<float, 3> query{0.6F, 0.8F, 0.0F};
    const std::array<float, 3> other{0.0F, 0.0F, 1.0F};

    for (const Metric metric : {Metric::kL2Squared, Metric::kCosine, Metric::kInnerProduct}) {
        const DistanceFunction distance(metric);
        EXPECT_LT(distance(query, query), distance(query, other))
            << "metric " << metric_name(metric);
    }
}

// ---------------------------------------------------------------------------
// Score conversion
// ---------------------------------------------------------------------------

TEST(ScoreConversion, RoundTripsForEveryMetric) {
    for (const Metric metric : {Metric::kL2Squared, Metric::kCosine, Metric::kInnerProduct}) {
        for (const float key : {0.0F, 0.25F, 1.0F, 2.0F, -0.5F}) {
            EXPECT_NEAR(
                rank_key_from_score(metric, score_from_rank_key(metric, key)), key, kTolerance)
                << "metric " << metric_name(metric) << " key " << key;
        }
    }
}

TEST(ScoreConversion, CosineScoreIsTheSimilarityNotTheDistance) {
    // Identical direction -> rank key 0 -> similarity 1.
    EXPECT_NEAR(score_from_rank_key(Metric::kCosine, 0.0F), 1.0F, kTolerance);
    // Orthogonal -> rank key 1 -> similarity 0.
    EXPECT_NEAR(score_from_rank_key(Metric::kCosine, 1.0F), 0.0F, kTolerance);
    // Opposite -> rank key 2 -> similarity -1.
    EXPECT_NEAR(score_from_rank_key(Metric::kCosine, 2.0F), -1.0F, kTolerance);
}

TEST(ScoreConversion, InnerProductScoreIsTheUnnegatedDotProduct) {
    EXPECT_NEAR(score_from_rank_key(Metric::kInnerProduct, -32.0F), 32.0F, kTolerance);
}

TEST(ScoreConversion, L2ScoreIsTheRankKeyUnchanged) {
    EXPECT_NEAR(score_from_rank_key(Metric::kL2Squared, 25.0F), 25.0F, kTolerance);
}

TEST(ScoreConversion, ReportsWhichDirectionIsBetter) {
    EXPECT_FALSE(higher_score_is_better(Metric::kL2Squared));
    EXPECT_TRUE(higher_score_is_better(Metric::kCosine));
    EXPECT_TRUE(higher_score_is_better(Metric::kInnerProduct));
}

// ---------------------------------------------------------------------------
// Metric maths
// ---------------------------------------------------------------------------

TEST(L2Metric, IsTheSquaredDistanceWithNoSquareRoot) {
    const std::array<float, 2> a{0.0F, 0.0F};
    const std::array<float, 2> b{3.0F, 4.0F};
    // 25, not 5. Taking the root is monotonic and would only cost time.
    EXPECT_NEAR(compute_rank_key(Metric::kL2Squared, a, b), 25.0F, kTolerance);
}

TEST(CosineMetric, IgnoresMagnitudeAndTracksDirection) {
    const std::array<float, 2> unit{1.0F, 0.0F};
    const std::array<float, 2> same_direction{100.0F, 0.0F};
    const std::array<float, 2> orthogonal{0.0F, 1.0F};
    const std::array<float, 2> opposite{-5.0F, 0.0F};

    EXPECT_NEAR(compute_rank_key(Metric::kCosine, unit, same_direction), 0.0F, kTolerance);
    EXPECT_NEAR(compute_rank_key(Metric::kCosine, unit, orthogonal), 1.0F, kTolerance);
    EXPECT_NEAR(compute_rank_key(Metric::kCosine, unit, opposite), 2.0F, kTolerance);
}

TEST(CosineMetric, StaysInsideItsDocumentedRangeForSelfComparison) {
    // Rounding can push cos above 1.0; a clamp keeps the reported score inside
    // [-1, 1] so nothing displays "similarity 1.0000001".
    const std::vector<float> values(768, 0.03571F);
    const float key = compute_rank_key(Metric::kCosine, values, values);
    EXPECT_GE(key, 0.0F);
    EXPECT_LE(key, 2.0F);
    EXPECT_NEAR(key, 0.0F, 1e-4F);
    EXPECT_LE(score_from_rank_key(Metric::kCosine, key), 1.0F);
}

// The zero vector is the case that produces 0/0. A NaN escaping here would
// break the total order that every ranking structure in the engine assumes.
TEST(CosineMetric, TreatsAZeroVectorAsUnrelatedRatherThanReturningNaN) {
    const std::array<float, 3> zero{0.0F, 0.0F, 0.0F};
    const std::array<float, 3> other{1.0F, 2.0F, 3.0F};

    const float key = compute_rank_key(Metric::kCosine, zero, other);
    EXPECT_FALSE(std::isnan(key));
    EXPECT_NEAR(key, 1.0F, kTolerance) << "defined as zero similarity";

    const float both_zero = compute_rank_key(Metric::kCosine, zero, zero);
    EXPECT_FALSE(std::isnan(both_zero));
    EXPECT_NEAR(both_zero, 1.0F, kTolerance);
}

TEST(InnerProductMetric, IsTheNegatedDotProduct) {
    const std::array<float, 3> a{1.0F, 2.0F, 3.0F};
    const std::array<float, 3> b{4.0F, 5.0F, 6.0F};
    EXPECT_NEAR(compute_rank_key(Metric::kInnerProduct, a, b), -32.0F, kTolerance);
    EXPECT_NEAR(score_from_rank_key(Metric::kInnerProduct,
                                    compute_rank_key(Metric::kInnerProduct, a, b)),
                32.0F,
                kTolerance);
}

// ---------------------------------------------------------------------------
// with_norms: the fast path search actually uses
// ---------------------------------------------------------------------------

TEST(WithNorms, MatchesTheFullComputationWhenGivenCorrectNorms) {
    const std::array<float, 4> a{1.0F, 2.0F, 3.0F, 4.0F};
    const std::array<float, 4> b{-2.0F, 0.5F, 1.0F, 3.0F};

    const float norm_a = l2_norm(a);
    const float norm_b = l2_norm(b);

    for (const Metric metric : {Metric::kL2Squared, Metric::kCosine, Metric::kInnerProduct}) {
        const DistanceFunction distance(metric);
        EXPECT_NEAR(distance.with_norms(a, norm_a, b, norm_b), distance(a, b), kTolerance)
            << "metric " << metric_name(metric);
    }
}

TEST(WithNorms, IgnoresNormsForMetricsThatDoNotUseThem) {
    const std::array<float, 2> a{1.0F, 0.0F};
    const std::array<float, 2> b{0.0F, 1.0F};

    for (const Metric metric : {Metric::kL2Squared, Metric::kInnerProduct}) {
        const DistanceFunction distance(metric);
        EXPECT_NEAR(distance.with_norms(a, 999.0F, b, -7.0F), distance(a, b), kTolerance)
            << "metric " << metric_name(metric)
            << ": nonsense norms must not affect a metric that does not read them";
    }
}

// When a database normalizes on insert, every stored norm is 1 and cosine
// degenerates to 1 - dot. That is the entire reason normalization is offered.
TEST(WithNorms, CosineOverNormalizedVectorsIsOneMinusDot) {
    std::vector<float> a{3.0F, 4.0F, 0.0F};
    std::vector<float> b{0.0F, 5.0F, 12.0F};
    normalize_in_place(a);
    normalize_in_place(b);

    const DistanceFunction cosine(Metric::kCosine);
    const float from_norms = cosine.with_norms(a, 1.0F, b, 1.0F);
    const float from_dot = 1.0F - scalar_kernel().dot(a.data(), b.data(), a.size());

    EXPECT_NEAR(from_norms, from_dot, kTolerance);
    EXPECT_NEAR(from_norms, cosine(a, b), kTolerance);
}

TEST(WithNorms, HandlesAZeroNormWithoutProducingNaN) {
    const std::array<float, 2> a{0.0F, 0.0F};
    const std::array<float, 2> b{1.0F, 1.0F};
    const DistanceFunction cosine(Metric::kCosine);

    EXPECT_FALSE(std::isnan(cosine.with_norms(a, 0.0F, b, l2_norm(b))));
}

// ---------------------------------------------------------------------------
// DistanceFunction plumbing
// ---------------------------------------------------------------------------

TEST(DistanceFunction, RemembersItsMetricAndKernel) {
    const DistanceFunction distance(Metric::kCosine, scalar_kernel());
    EXPECT_EQ(distance.metric(), Metric::kCosine);
    EXPECT_EQ(&distance.kernel(), &scalar_kernel());
    EXPECT_EQ(distance.kernel().name, "scalar");
}

TEST(DistanceFunction, GivesIdenticalResultsAcrossEveryAvailableKernel) {
    const std::vector<float> a{0.1F, -0.2F, 0.3F, 0.4F, -0.5F, 0.6F, 0.7F, -0.8F, 0.9F};
    const std::vector<float> b{-0.9F, 0.8F, 0.7F, -0.6F, 0.5F, 0.4F, -0.3F, 0.2F, 0.1F};

    for (const Metric metric : {Metric::kL2Squared, Metric::kCosine, Metric::kInnerProduct}) {
        const float expected = DistanceFunction(metric, scalar_kernel())(a, b);
        for (const DistanceKernel* kernel : available_kernels()) {
            EXPECT_NEAR(DistanceFunction(metric, *kernel)(a, b), expected, 1e-4F)
                << "metric " << metric_name(metric) << " kernel " << kernel->name;
        }
    }
}

}  // namespace
}  // namespace vectordb
