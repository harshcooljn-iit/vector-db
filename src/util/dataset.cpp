// SPDX-License-Identifier: MIT
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

#include <vectordb/core/error.hpp>
#include <vectordb/util/dataset.hpp>

namespace vectordb {
namespace {

/// splitmix64. Chosen because it is four lines, has good statistical
/// properties, and — the point here — is fully specified by those four lines,
/// so it produces identical output on every compiler and platform.
std::uint64_t splitmix64(std::uint64_t& state) noexcept {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

std::size_t default_cluster_count(std::size_t count) noexcept {
    const auto root = static_cast<std::size_t>(std::sqrt(static_cast<double>(count)));
    return root < 2 ? 2 : root;
}

}  // namespace

DeterministicRng::DeterministicRng(std::uint64_t seed) noexcept : state_(seed) {}

std::uint64_t DeterministicRng::next() noexcept {
    return splitmix64(state_);
}

float DeterministicRng::uniform() noexcept {
    // 24 bits is exactly float's significand, so every value is representable
    // and the conversion introduces no rounding of its own.
    constexpr float kScale = 1.0F / 16777216.0F;  // 2^-24
    return static_cast<float>(next() >> 40) * kScale;
}

float DeterministicRng::uniform(float low, float high) noexcept {
    return low + (high - low) * uniform();
}

float DeterministicRng::normal() noexcept {
    // Box-Muller. u1 must be strictly positive or log(0) is -inf; uniform()
    // returns [0, 1), so nudge the zero case rather than looping — that keeps
    // the number of RNG draws per call constant and therefore reproducible.
    float u1 = uniform();
    if (u1 <= 0.0F) {
        u1 = std::numeric_limits<float>::min();
    }
    const float u2 = uniform();
    const float radius = std::sqrt(-2.0F * std::log(u1));
    const auto angle = static_cast<float>(2.0 * std::numbers::pi_v<double>) * u2;
    return radius * std::cos(angle);
}

VectorArray generate_dataset(const DatasetSpec& spec) {
    if (spec.count == 0) {
        throw InvalidArgumentError("dataset count must be at least 1");
    }

    VectorArray vectors(spec.dimension);
    vectors.reserve(spec.count);

    // Two independent streams. `structure_rng` places the cluster centroids and
    // is driven by `seed`; `rng` draws the individual points and is driven by
    // `sample_seed`. Keeping them separate is what lets a query set share a
    // dataset's clusters while being different points — see DatasetSpec.
    DeterministicRng structure_rng(spec.seed);
    DeterministicRng rng(spec.sample_seed);
    std::vector<float> scratch(spec.dimension);

    switch (spec.kind) {
        case DatasetKind::kUniform:
            for (std::size_t i = 0; i < spec.count; ++i) {
                for (Dimension d = 0; d < spec.dimension; ++d) {
                    scratch[d] = rng.uniform(-1.0F, 1.0F);
                }
                vectors.push_back(scratch);
            }
            break;

        case DatasetKind::kGaussian:
            for (std::size_t i = 0; i < spec.count; ++i) {
                for (Dimension d = 0; d < spec.dimension; ++d) {
                    scratch[d] = rng.normal();
                }
                vectors.push_back(scratch);
            }
            break;

        case DatasetKind::kClustered: {
            const std::size_t clusters =
                spec.cluster_count > 0 ? spec.cluster_count : default_cluster_count(spec.count);

            // Centroids first, so the same seed and cluster count always give
            // the same cluster structure regardless of how many points follow.
            VectorArray centroids(spec.dimension);
            centroids.reserve(clusters);
            for (std::size_t c = 0; c < clusters; ++c) {
                for (Dimension d = 0; d < spec.dimension; ++d) {
                    scratch[d] = structure_rng.normal();
                }
                centroids.push_back(scratch);
            }

            for (std::size_t i = 0; i < spec.count; ++i) {
                // Round-robin rather than random assignment, so every cluster
                // has a predictable population and a recall number is not at
                // the mercy of one cluster happening to have three members.
                const VectorView centroid = centroids[static_cast<LocalId>(i % clusters)];
                for (Dimension d = 0; d < spec.dimension; ++d) {
                    scratch[d] = centroid[d] + spec.cluster_stddev * rng.normal();
                }
                vectors.push_back(scratch);
            }
            break;
        }
    }

    return vectors;
}

VectorArray generate_queries(const DatasetSpec& spec, std::size_t count) {
    DatasetSpec query_spec = spec;
    query_spec.count = count;

    // `seed` is held fixed so the queries inhabit the *same* cluster structure
    // as the dataset; only `sample_seed` moves, so they are different points.
    //
    // Deriving both — the obvious thing, and what this originally did — puts
    // the queries in a different cluster layout entirely, so every query lands
    // in the empty space between the dataset's clusters. Measured on a 64-d,
    // 5000-point set, such a query's 10th nearest neighbour was 3% further away
    // than its 1st, against 44% for a real in-distribution point. Recall on
    // those queries measures tie-breaking among near-equidistant points and
    // reports ~0.75 for an index that is actually returning ~0.99.
    std::uint64_t derived = spec.sample_seed;
    query_spec.sample_seed = splitmix64(derived);
    return generate_dataset(query_spec);
}

}  // namespace vectordb
