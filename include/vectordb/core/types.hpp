// SPDX-License-Identifier: MIT
//
// Fundamental value types shared by every layer of VectorDB.
//
// Learning note: learnings/30-vector-database/02-vector-representations.md
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace vectordb {

/// A user-facing, stable identifier for a vector.
///
/// `VectorId` is what callers insert with, search results report, and metadata
/// rows key on. It never changes for the lifetime of a vector, and it is *not*
/// reused after deletion.
using VectorId = std::uint64_t;

/// A dense, internal slot number in the range `[0, vector_count)`.
///
/// Why two id types? The HNSW graph stores millions of neighbour references.
/// Storing a `VectorId` (8 bytes) in every neighbour slot would double the
/// graph's memory footprint versus a `LocalId` (4 bytes) — for 1M vectors with
/// M=16 that is 128 MB versus 64 MB of pure edge data. `LocalId` is also a
/// direct index into contiguous vector storage, so `store[local_id]` is a
/// pointer offset rather than a hash lookup.
///
/// The price is an indirection: converting between the two requires a lookup
/// table. That trade is documented in docs/decisions/ADR-0003-id-model.md.
using LocalId = std::uint32_t;

/// The number of components in a vector (e.g. 768 for many text embeddings).
using Dimension = std::uint32_t;

/// Sentinel meaning "no such vector".
inline constexpr VectorId kInvalidVectorId = std::numeric_limits<VectorId>::max();

/// Sentinel meaning "no such slot".
inline constexpr LocalId kInvalidLocalId = std::numeric_limits<LocalId>::max();

/// Upper bound on dimension. Not a hardware limit — a sanity limit so that a
/// corrupt file header claiming `dimension = 4'000'000'000` cannot make us
/// attempt a multi-gigabyte allocation. See docs/decisions/ADR-0008-validation.md.
inline constexpr Dimension kMaxDimension = 65536;

/// Upper bound on the number of vectors a single database may hold.
/// Bounded by `LocalId` being 32-bit, minus one slot for `kInvalidLocalId`.
inline constexpr std::uint64_t kMaxVectorCount =
    static_cast<std::uint64_t>(std::numeric_limits<LocalId>::max()) - 1U;

/// Which notion of "close" a database uses.
///
/// The mathematical definitions live in docs/distance-metrics.md; the
/// implementations live in src/distance/.
enum class Metric : std::uint8_t {
    /// Sum of squared component differences. Smaller is better.
    /// The square root is deliberately not taken — it is monotonic, so it
    /// cannot change any ranking, and it costs a `sqrt` per candidate.
    kL2Squared = 0,

    /// Cosine *distance*: `1 - cos(a, b)`. Smaller is better.
    /// Range [0, 2] for arbitrary vectors.
    kCosine = 1,

    /// Negated inner product: `-(a . b)`. Smaller is better.
    /// Negated so that every metric in this enum shares one comparison
    /// direction; see `core/score.hpp` for why that matters.
    kInnerProduct = 2,
};

/// Stable lowercase spelling used in config files, the CLI and JSON output.
[[nodiscard]] std::string_view metric_name(Metric metric) noexcept;

/// Parses a metric spelled as in `metric_name`.
/// Returns false and leaves `out` untouched when the name is unknown.
[[nodiscard]] bool parse_metric(std::string_view name, Metric& out) noexcept;

/// Which index structure answers searches.
enum class IndexType : std::uint8_t {
    /// Exact search by scanning every live vector. The correctness oracle.
    kBruteForce = 0,

    /// Hierarchical Navigable Small World graph. Approximate, sub-linear.
    kHnsw = 1,
};

[[nodiscard]] std::string_view index_type_name(IndexType type) noexcept;
[[nodiscard]] bool parse_index_type(std::string_view name, IndexType& out) noexcept;

}  // namespace vectordb
