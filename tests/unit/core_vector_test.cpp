// SPDX-License-Identifier: MIT
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <vectordb/core/error.hpp>
#include <vectordb/core/vector.hpp>

namespace vectordb {
namespace {

constexpr float kTolerance = 1e-6F;

TEST(ValidateVector, AcceptsAMatchingFiniteVector) {
    const std::array<float, 4> values{1.0F, -2.0F, 0.0F, 3.5F};
    EXPECT_NO_THROW(validate_vector(values, 4));
}

TEST(ValidateVector, RejectsWrongDimensionWithBothValuesInTheMessage) {
    const std::vector<float> values(384, 0.5F);
    try {
        validate_vector(values, 768);
        FAIL() << "expected DimensionMismatchError";
    } catch (const DimensionMismatchError& error) {
        EXPECT_EQ(error.expected(), 768U);
        EXPECT_EQ(error.actual(), 384U);
    }
}

TEST(ValidateVector, RejectsNaNAndNamesTheOffendingIndex) {
    std::array<float, 3> values{1.0F, std::numeric_limits<float>::quiet_NaN(), 3.0F};
    try {
        validate_vector(values, 3);
        FAIL() << "expected InvalidVectorError";
    } catch (const InvalidVectorError& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("component 1"), std::string::npos) << message;
        EXPECT_NE(message.find("NaN"), std::string::npos) << message;
    }
}

TEST(ValidateVector, RejectsPositiveAndNegativeInfinity) {
    std::array<float, 2> positive{std::numeric_limits<float>::infinity(), 0.0F};
    std::array<float, 2> negative{0.0F, -std::numeric_limits<float>::infinity()};

    EXPECT_THROW(validate_vector(positive, 2), InvalidVectorError);
    EXPECT_THROW(validate_vector(negative, 2), InvalidVectorError);

    try {
        validate_vector(negative, 2);
    } catch (const InvalidVectorError& error) {
        EXPECT_NE(std::string(error.what()).find("-Inf"), std::string::npos);
    }
}

TEST(ValidateVector, AcceptsSubnormalsAndZero) {
    // Subnormals are finite. They are slow on some hardware but they are valid
    // numbers, and silently rejecting them would surprise a caller whose model
    // produced one.
    std::array<float, 2> values{std::numeric_limits<float>::denorm_min(), 0.0F};
    EXPECT_NO_THROW(validate_vector(values, 2));
}

TEST(FindNonFinite, ReportsSizeWhenEverythingIsFinite) {
    const std::array<float, 3> values{1.0F, 2.0F, 3.0F};
    EXPECT_EQ(find_non_finite(values), 3U);
    EXPECT_TRUE(is_finite(values));
}

TEST(Norm, ComputesKnownPythagoreanValues) {
    const std::array<float, 2> three_four{3.0F, 4.0F};
    EXPECT_NEAR(l2_norm(three_four), 5.0F, kTolerance);
    EXPECT_NEAR(l2_norm_squared(three_four), 25.0F, kTolerance);

    const std::array<float, 4> unit{0.5F, 0.5F, 0.5F, 0.5F};
    EXPECT_NEAR(l2_norm(unit), 1.0F, kTolerance);
}

TEST(Norm, OfAnEmptyVectorIsZero) {
    EXPECT_NEAR(l2_norm(VectorView{}), 0.0F, kTolerance);
}

TEST(NormalizeInPlace, ProducesUnitLengthAndReturnsTheOriginalNorm) {
    std::array<float, 2> values{3.0F, 4.0F};
    const float norm = normalize_in_place(values);

    EXPECT_NEAR(norm, 5.0F, kTolerance);
    EXPECT_NEAR(values[0], 0.6F, kTolerance);
    EXPECT_NEAR(values[1], 0.8F, kTolerance);
    EXPECT_NEAR(l2_norm(values), 1.0F, kTolerance);
}

// The zero vector is the edge case that turns into NaN if handled carelessly,
// and a NaN reaching an index is exactly what validate_vector exists to stop.
TEST(NormalizeInPlace, LeavesAZeroVectorAloneRatherThanProducingNaN) {
    std::array<float, 3> values{0.0F, 0.0F, 0.0F};
    const float norm = normalize_in_place(values);

    EXPECT_EQ(norm, 0.0F);
    for (const float value : values) {
        EXPECT_EQ(value, 0.0F);
        EXPECT_FALSE(std::isnan(value));
    }
}

TEST(NormalizeInPlace, IsIdempotent) {
    std::array<float, 3> values{1.0F, 2.0F, 2.0F};
    normalize_in_place(values);
    const std::array<float, 3> once = values;

    const float second_norm = normalize_in_place(values);

    EXPECT_NEAR(second_norm, 1.0F, kTolerance);
    for (std::size_t i = 0; i < values.size(); ++i) {
        EXPECT_NEAR(values[i], once[i], kTolerance);
    }
}

TEST(NormalizeInto, WritesToTheDestinationAndLeavesTheSourceUnchanged) {
    const std::array<float, 2> source{0.0F, 7.0F};
    std::array<float, 2> destination{99.0F, 99.0F};

    const float norm = normalize_into(source, destination);

    EXPECT_NEAR(norm, 7.0F, kTolerance);
    EXPECT_NEAR(destination[0], 0.0F, kTolerance);
    EXPECT_NEAR(destination[1], 1.0F, kTolerance);
    EXPECT_NEAR(source[1], 7.0F, kTolerance);
}

TEST(NormalizeInto, RejectsMismatchedLengths) {
    const std::array<float, 2> source{1.0F, 1.0F};
    std::array<float, 3> destination{};
    EXPECT_THROW(normalize_into(source, destination), DimensionMismatchError);
}

TEST(NormalizeInto, CopiesAZeroVectorVerbatim) {
    const std::array<float, 2> source{0.0F, 0.0F};
    std::array<float, 2> destination{5.0F, 5.0F};

    EXPECT_EQ(normalize_into(source, destination), 0.0F);
    EXPECT_EQ(destination[0], 0.0F);
    EXPECT_EQ(destination[1], 0.0F);
}

// Long vectors are where a naive float accumulator drifts. 4096 components of
// 1.0 have a norm of exactly 64; a float accumulator loses the low bits well
// before then, so this pins the double-accumulator choice in l2_norm_squared.
TEST(Norm, StaysAccurateOverManyComponents) {
    const std::vector<float> values(4096, 1.0F);
    EXPECT_NEAR(l2_norm_squared(values), 4096.0F, 1e-3F);
    EXPECT_NEAR(l2_norm(values), 64.0F, 1e-4F);
}

}  // namespace
}  // namespace vectordb
