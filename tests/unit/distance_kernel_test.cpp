// SPDX-License-Identifier: MIT
//
// Kernel-level tests: the raw reductions, checked against values computed by
// hand or by an independent method.
//
// Every kernel in `available_kernels()` runs the whole suite, so when a SIMD
// kernel lands it is validated against exactly these expectations rather than
// against a separate, weaker set.
#include <cmath>
#include <numeric>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include <vectordb/distance/kernel.hpp>

namespace vectordb {
namespace {

class KernelTest : public ::testing::TestWithParam<const DistanceKernel*> {
protected:
    const DistanceKernel& kernel() const { return *GetParam(); }
};

TEST_P(KernelTest, L2SquaredOfIdenticalVectorsIsZero) {
    const std::vector<float> values{1.5F, -2.25F, 3.0F, 0.0F, 7.125F};
    EXPECT_FLOAT_EQ(kernel().l2_squared(values.data(), values.data(), values.size()), 0.0F);
}

TEST_P(KernelTest, L2SquaredMatchesHandComputedValues) {
    // (1-4)^2 + (2-6)^2 = 9 + 16 = 25
    const std::vector<float> a{1.0F, 2.0F};
    const std::vector<float> b{4.0F, 6.0F};
    EXPECT_FLOAT_EQ(kernel().l2_squared(a.data(), b.data(), 2), 25.0F);
}

TEST_P(KernelTest, L2SquaredIsSymmetric) {
    const std::vector<float> a{1.0F, -3.0F, 5.5F, 2.0F, 0.5F, -1.0F, 8.0F};
    const std::vector<float> b{-2.0F, 4.0F, 0.0F, 1.5F, -6.0F, 3.0F, 2.25F};

    EXPECT_FLOAT_EQ(kernel().l2_squared(a.data(), b.data(), a.size()),
                    kernel().l2_squared(b.data(), a.data(), a.size()));
}

TEST_P(KernelTest, DotMatchesHandComputedValues) {
    // 1*4 + 2*5 + 3*6 = 4 + 10 + 18 = 32
    const std::vector<float> a{1.0F, 2.0F, 3.0F};
    const std::vector<float> b{4.0F, 5.0F, 6.0F};
    EXPECT_FLOAT_EQ(kernel().dot(a.data(), b.data(), 3), 32.0F);
}

TEST_P(KernelTest, DotOfOrthogonalVectorsIsZero) {
    const std::vector<float> a{1.0F, 0.0F, 0.0F, 0.0F};
    const std::vector<float> b{0.0F, 1.0F, 0.0F, 0.0F};
    EXPECT_FLOAT_EQ(kernel().dot(a.data(), b.data(), 4), 0.0F);
}

TEST_P(KernelTest, NormSquaredEqualsDotWithItself) {
    const std::vector<float> values{3.0F, 4.0F, 12.0F, 0.5F, -7.0F};
    EXPECT_FLOAT_EQ(kernel().norm_squared(values.data(), values.size()),
                    kernel().dot(values.data(), values.data(), values.size()));
    // 9 + 16 + 144 + 0.25 + 49
    EXPECT_FLOAT_EQ(kernel().norm_squared(values.data(), values.size()), 218.25F);
}

TEST_P(KernelTest, HandlesZeroLengthWithoutReadingAnything) {
    // A null pointer with count 0 must not be dereferenced. Passing nullptr
    // makes any read a segfault rather than a silent success on stale data.
    EXPECT_FLOAT_EQ(kernel().l2_squared(nullptr, nullptr, 0), 0.0F);
    EXPECT_FLOAT_EQ(kernel().dot(nullptr, nullptr, 0), 0.0F);
    EXPECT_FLOAT_EQ(kernel().norm_squared(nullptr, 0), 0.0F);
}

// The unrolled loop processes four at a time and then a tail. Every remainder
// class must be exercised, or an off-by-one in the tail survives to production
// on precisely the dimensions nobody benchmarks.
TEST_P(KernelTest, HandlesEveryTailLengthFromOneToSeventeen) {
    std::vector<float> a(17);
    std::vector<float> b(17);
    std::iota(a.begin(), a.end(), 1.0F);
    std::iota(b.begin(), b.end(), 3.0F);

    for (std::size_t count = 1; count <= 17; ++count) {
        // Every b[i] - a[i] is exactly 2, so the answer is 4 * count.
        EXPECT_FLOAT_EQ(kernel().l2_squared(a.data(), b.data(), count),
                        4.0F * static_cast<float>(count))
            << "count = " << count;

        double expected_dot = 0.0;
        for (std::size_t i = 0; i < count; ++i) {
            expected_dot += static_cast<double>(a[i]) * static_cast<double>(b[i]);
        }
        EXPECT_NEAR(
            kernel().dot(a.data(), b.data(), count), static_cast<float>(expected_dot), 1e-3F)
            << "count = " << count;
    }
}

// A double-precision reference computed independently of the kernel. This is
// the test that will catch a SIMD kernel that reduces its lanes incorrectly.
TEST_P(KernelTest, AgreesWithADoublePrecisionReferenceOnRandomData) {
    std::mt19937 rng(20260829);
    std::uniform_real_distribution<float> dist(-1.0F, 1.0F);

    for (const std::size_t dimension : {1U, 3U, 4U, 5U, 8U, 15U, 128U, 384U, 768U}) {
        std::vector<float> a(dimension);
        std::vector<float> b(dimension);
        for (std::size_t i = 0; i < dimension; ++i) {
            a[i] = dist(rng);
            b[i] = dist(rng);
        }

        double reference_l2 = 0.0;
        double reference_dot = 0.0;
        for (std::size_t i = 0; i < dimension; ++i) {
            const double diff = static_cast<double>(a[i]) - static_cast<double>(b[i]);
            reference_l2 += diff * diff;
            reference_dot += static_cast<double>(a[i]) * static_cast<double>(b[i]);
        }

        // Relative tolerance: absolute error grows with the number of terms, so
        // a fixed epsilon would be too loose at D=1 and too tight at D=768.
        const float l2 = kernel().l2_squared(a.data(), b.data(), dimension);
        const float dot = kernel().dot(a.data(), b.data(), dimension);
        const auto tolerance = static_cast<float>(1e-5 * (1.0 + std::abs(reference_l2)));

        EXPECT_NEAR(l2, static_cast<float>(reference_l2), tolerance)
            << "l2, dimension " << dimension;
        EXPECT_NEAR(dot,
                    static_cast<float>(reference_dot),
                    static_cast<float>(1e-5 * (1.0 + std::abs(reference_dot))))
            << "dot, dimension " << dimension;
    }
}

// Multiple accumulators exist partly for accuracy. 4096 ones summed in a single
// float accumulator still lands on 4096 exactly (powers of two are kind), so
// use a value that actually stresses rounding.
TEST_P(KernelTest, StaysAccurateOverLongVectors) {
    const std::vector<float> values(4096, 0.1F);
    const float norm_squared = kernel().norm_squared(values.data(), values.size());
    EXPECT_NEAR(norm_squared, 40.96F, 1e-2F);
}

INSTANTIATE_TEST_SUITE_P(AllKernels,
                         KernelTest,
                         ::testing::ValuesIn(available_kernels().begin(),
                                             available_kernels().end()),
                         [](const ::testing::TestParamInfo<const DistanceKernel*>& info) {
                             return std::string(info.param->name);
                         });

TEST(KernelRegistry, AlwaysOffersTheScalarKernel) {
    EXPECT_NE(kernel_by_name("scalar"), nullptr);
    EXPECT_EQ(kernel_by_name("scalar"), &scalar_kernel());
}

TEST(KernelRegistry, ReturnsNullForAKernelThisMachineCannotRun) {
    EXPECT_EQ(kernel_by_name("definitely-not-a-kernel"), nullptr);
}

TEST(KernelRegistry, ActiveKernelIsOneOfTheAvailableOnes) {
    bool found = false;
    for (const DistanceKernel* kernel : available_kernels()) {
        if (kernel == &active_kernel()) {
            found = true;
        }
    }
    EXPECT_TRUE(found);
    EXPECT_FALSE(active_kernel().name.empty());
}

// Every kernel must agree with scalar, since scalar is the oracle. With only
// the scalar kernel present this is trivially true; it becomes the central
// SIMD test the moment a second kernel appears, so it is written now rather
// than remembered later.
TEST(KernelRegistry, EveryKernelAgreesWithScalarOnRandomData) {
    std::mt19937 rng(99);
    std::uniform_real_distribution<float> dist(-3.0F, 3.0F);

    constexpr std::size_t kDimension = 768;
    std::vector<float> a(kDimension);
    std::vector<float> b(kDimension);
    for (std::size_t i = 0; i < kDimension; ++i) {
        a[i] = dist(rng);
        b[i] = dist(rng);
    }

    const DistanceKernel& reference = scalar_kernel();
    const float expected_l2 = reference.l2_squared(a.data(), b.data(), kDimension);
    const float expected_dot = reference.dot(a.data(), b.data(), kDimension);

    for (const DistanceKernel* kernel : available_kernels()) {
        EXPECT_NEAR(kernel->l2_squared(a.data(), b.data(), kDimension),
                    expected_l2,
                    std::abs(expected_l2) * 1e-4F + 1e-4F)
            << "kernel " << kernel->name;
        EXPECT_NEAR(kernel->dot(a.data(), b.data(), kDimension),
                    expected_dot,
                    std::abs(expected_dot) * 1e-4F + 1e-4F)
            << "kernel " << kernel->name;
    }
}

}  // namespace
}  // namespace vectordb
