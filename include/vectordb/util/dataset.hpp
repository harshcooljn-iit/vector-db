// SPDX-License-Identifier: MIT
//
// Deterministic synthetic datasets.
//
// Benchmarks and recall tests both need data that is identical on every machine
// and every run. Generated rather than committed: a 1M x 768 dataset is 3 GB,
// and it is reproducible from twelve bytes of specification.
//
// Learning note: learnings/70-performance/07-benchmarking.md
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <vectordb/core/types.hpp>
#include <vectordb/core/vector_array.hpp>

namespace vectordb {

/// What the generated point cloud looks like.
enum class DatasetKind : std::uint8_t {
    /// Independent uniform components. **A pathological case for ANN**, and the
    /// most common way to accidentally publish a misleading recall number.
    ///
    /// In high dimensions, independent uniform points are all roughly
    /// equidistant from each other — the concentration of distance that people
    /// mean by "the curse of dimensionality". The ratio between the nearest and
    /// the furthest neighbour approaches 1, so there is barely a "nearest
    /// neighbour" to find, and a graph index looks far worse than it is on real
    /// data. Useful precisely as a worst case; misleading as a default.
    kUniform,

    /// Independent standard normal components. Slightly kinder than uniform,
    /// still essentially structureless.
    kGaussian,

    /// Gaussian blobs around random centroids. **The default**, because it is
    /// the closest cheap approximation to what real embeddings look like:
    /// locally dense, globally sparse, with genuine nearest neighbours to find.
    kClustered,
};

/// A dataset, fully specified. Two identical specs produce byte-identical data
/// on any platform — see the note on portable randomness in the implementation.
struct DatasetSpec {
    Dimension dimension = 128;
    std::size_t count = 10000;
    std::uint64_t seed = 42;
    DatasetKind kind = DatasetKind::kClustered;

    /// Only for `kClustered`. 0 means "pick a sensible number", currently
    /// `max(2, sqrt(count))`.
    std::size_t cluster_count = 0;

    /// Spread of each cluster relative to the spread of the centroids. Smaller
    /// means tighter, more separable clusters and therefore easier search.
    float cluster_stddev = 0.15F;
};

/// Generates the dataset described by `spec`.
/// @throws InvalidArgumentError on a degenerate dimension or an empty dataset
[[nodiscard]] VectorArray generate_dataset(const DatasetSpec& spec);

/// Generates `count` query vectors from the same distribution as `spec`, using a
/// derived seed so that queries are never members of the dataset.
[[nodiscard]] VectorArray generate_queries(const DatasetSpec& spec, std::size_t count);

/// A tiny, xorshift-free, fully specified PRNG.
///
/// `std::mt19937_64`'s *output* is specified by the standard, but
/// `std::normal_distribution` and `std::uniform_real_distribution` are not —
/// two standard libraries may produce different values from the same engine and
/// seed. That would make "the same dataset" mean different things on macOS and
/// Linux, and quietly invalidate any cross-machine benchmark comparison.
///
/// So the float conversion is done here, explicitly, from raw bits.
class DeterministicRng {
public:
    explicit DeterministicRng(std::uint64_t seed) noexcept;

    /// Raw 64 bits.
    std::uint64_t next() noexcept;

    /// Uniform in [0, 1). Built from the top 24 bits, which is exactly the
    /// precision a float can represent without rounding surprises.
    float uniform() noexcept;

    /// Uniform in [low, high).
    float uniform(float low, float high) noexcept;

    /// Standard normal, via the Box-Muller transform on two uniforms.
    float normal() noexcept;

private:
    std::uint64_t state_;
};

}  // namespace vectordb
