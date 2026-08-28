// SPDX-License-Identifier: MIT
//
// Metrics, and the ordering policy the whole system depends on.
//
// Mathematical definitions: docs/distance-metrics.md
// Learning note:            learnings/30-vector-database/03-distance-metrics.md
#pragma once

#include <vectordb/core/types.hpp>
#include <vectordb/core/vector.hpp>
#include <vectordb/distance/kernel.hpp>

namespace vectordb {

/// The internal ranking value. **Smaller is always better, for every metric.**
///
/// This is the single most important convention in the codebase. Because it
/// holds unconditionally, the top-k heap, the HNSW candidate queues, the result
/// merging and every comparison in the engine are written once, with no
/// per-metric branching and no "is this metric ascending or descending?" flag
/// to get wrong.
///
/// It is not what a user wants to see, though — nobody reads "-0.91" as a
/// strong cosine match. `score_from_rank_key` converts at the API boundary,
/// exactly once per returned result rather than once per candidate.
using RankKey = float;

/// Converts an internal rank key into the metric's natural, human-facing score.
///
/// | Metric        | RankKey (smaller better) | Score (what users see) |
/// |---------------|--------------------------|------------------------|
/// | kL2Squared    | squared distance         | squared distance — smaller is better |
/// | kCosine       | `1 - cos`  in [0, 2]     | `cos` in [-1, 1] — larger is better |
/// | kInnerProduct | `-(a . b)`               | `a . b` — larger is better |
[[nodiscard]] float score_from_rank_key(Metric metric, RankKey key) noexcept;

/// The inverse of `score_from_rank_key`.
[[nodiscard]] RankKey rank_key_from_score(Metric metric, float score) noexcept;

/// True when larger scores are better for this metric — i.e. whether the score
/// is a similarity rather than a distance. Used only for presentation.
[[nodiscard]] bool higher_score_is_better(Metric metric) noexcept;

/// Binds a metric to a kernel and computes rank keys.
///
/// Cheap to copy (two words). Construct one per search, not one per candidate:
/// the kernel lookup and the metric branch are resolved here so the inner loop
/// does neither.
class DistanceFunction {
public:
    explicit DistanceFunction(Metric metric,
                              const DistanceKernel& kernel = active_kernel()) noexcept
        : metric_(metric), kernel_(&kernel) {}

    [[nodiscard]] Metric metric() const noexcept { return metric_; }

    [[nodiscard]] const DistanceKernel& kernel() const noexcept { return *kernel_; }

    /// Rank key between two vectors, computing whatever the metric needs.
    ///
    /// For cosine this means two extra `O(D)` norm passes — three passes over
    /// the data instead of one. In a scan that is 3x the memory traffic on the
    /// hottest loop in the system, so search paths use `with_norms` instead and
    /// keep a precomputed norm per stored vector.
    ///
    /// Preconditions: `a.size() == b.size()`, both finite. Both are guaranteed
    /// by `validate_vector` at the database boundary, so this function does not
    /// re-check them — it is called billions of times and must not branch.
    [[nodiscard]] float operator()(VectorView a, VectorView b) const noexcept;

    /// Rank key using precomputed L2 norms, which only cosine consults.
    ///
    /// One pass over the data instead of three. When a database normalizes on
    /// insert, every stored norm is 1.0 and cosine degenerates to `1 - dot`,
    /// which is why normalization is worth offering at all.
    ///
    /// Zero-norm policy: a zero vector has no direction, so its cosine
    /// similarity to anything is undefined. We define it as 0 similarity
    /// (rank key 1.0) rather than returning NaN — a NaN here would break the
    /// ordering that the entire engine assumes. See ADR-0007.
    [[nodiscard]] float with_norms(VectorView a,
                                   float norm_a,
                                   VectorView b,
                                   float norm_b) const noexcept;

private:
    Metric metric_;
    const DistanceKernel* kernel_;
};

/// One-shot convenience for tests, tools and cold paths. Constructs a
/// `DistanceFunction` per call, so never use it inside a loop.
[[nodiscard]] float compute_rank_key(Metric metric, VectorView a, VectorView b) noexcept;

}  // namespace vectordb
