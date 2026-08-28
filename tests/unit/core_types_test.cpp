// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <vectordb/core/types.hpp>

namespace vectordb {
namespace {

TEST(MetricNames, RoundTripsThroughCanonicalSpelling) {
    for (const Metric metric : {Metric::kL2Squared, Metric::kCosine, Metric::kInnerProduct}) {
        Metric parsed{};
        ASSERT_TRUE(parse_metric(metric_name(metric), parsed))
            << "canonical name " << metric_name(metric) << " must parse back";
        EXPECT_EQ(parsed, metric);
    }
}

TEST(MetricNames, AcceptsDocumentedAliases) {
    Metric parsed{};
    ASSERT_TRUE(parse_metric("euclidean", parsed));
    EXPECT_EQ(parsed, Metric::kL2Squared);
    ASSERT_TRUE(parse_metric("l2_squared", parsed));
    EXPECT_EQ(parsed, Metric::kL2Squared);
}

TEST(MetricNames, RejectsUnknownNameWithoutTouchingOutput) {
    Metric parsed = Metric::kCosine;
    EXPECT_FALSE(parse_metric("manhattan", parsed));
    EXPECT_EQ(parsed, Metric::kCosine) << "output must be left untouched";
}

TEST(IndexTypeNames, RoundTripsThroughCanonicalSpelling) {
    for (const IndexType type : {IndexType::kBruteForce, IndexType::kHnsw}) {
        IndexType parsed{};
        ASSERT_TRUE(parse_index_type(index_type_name(type), parsed));
        EXPECT_EQ(parsed, type);
    }
}

TEST(IndexTypeNames, RejectsUnknownName) {
    IndexType parsed = IndexType::kHnsw;
    EXPECT_FALSE(parse_index_type("ivf", parsed));
    EXPECT_EQ(parsed, IndexType::kHnsw);
}

// The id model is load-bearing: LocalId is 32-bit specifically so that HNSW
// neighbour lists stay compact. If someone widens or narrows it, the storage
// format changes, so pin the assumption here.
TEST(IdModel, LocalIdIsThirtyTwoBitAndVectorIdIsSixtyFour) {
    EXPECT_EQ(sizeof(LocalId), 4U);
    EXPECT_EQ(sizeof(VectorId), 8U);
    EXPECT_EQ(kMaxVectorCount, 4294967294ULL);
}

}  // namespace
}  // namespace vectordb
