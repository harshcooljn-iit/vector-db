// SPDX-License-Identifier: MIT
//
// Hierarchical Navigable Small World graph index.
//
// Learning notes: learnings/40-search-indexes/03-hnsw-intuition.md
//                 learnings/40-search-indexes/04-hnsw-algorithm.md
//                 learnings/40-search-indexes/05-hnsw-implementation.md
// Specification:  docs/hnsw.md
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <vectordb/distance/distance.hpp>
#include <vectordb/index/hnsw_config.hpp>
#include <vectordb/index/vector_index.hpp>
#include <vectordb/util/dataset.hpp>

namespace vectordb {

/// An approximate index built from a layered proximity graph.
///
/// ## The idea in three sentences
///
/// Every vector is a node in a graph whose edges connect near neighbours.
/// Searching means walking greedily towards the query — always stepping to the
/// neighbour that improves things — which is fast but gets stuck in local
/// minima. HNSW fixes that by stacking sparse graphs above the full one, so the
/// search starts with huge strides through a nearly-empty top layer and
/// refines downward, arriving near the answer before the expensive layer even
/// starts.
///
/// ## Memory layout
///
/// Layer 0 contains every node and is where search spends nearly all its time,
/// so its links live in one flat array with a fixed stride:
///
/// ```
/// layer0_links_:  [count][n0][n1]...[n_2M-1] [count][n0]...
///                 |<------ 2M + 1 entries ------>|
/// ```
///
/// The count sits in the same cache line as the first neighbours, so reading
/// "how many" and "which" is one fetch. `LocalId` is 32-bit specifically so
/// this array stays half the size it would otherwise be — see
/// docs/decisions/ADR-0003-id-model.md.
///
/// Upper layers hold only about `1/M` of the nodes each, so a fixed stride
/// across all nodes would be almost entirely empty. They share one append-only
/// arena instead, with a per-node offset.
///
/// ## Thread safety
///
/// `search` is safe to call concurrently with other searches. `add` and
/// `remove` require exclusive access. See docs/concurrency.md.
class HnswIndex final : public VectorIndex {
public:
    /// @param accessor  read-only view of the store; must outlive this index
    /// @param metric    how distance is defined
    /// @param config    tuning parameters, validated here
    /// @param kernel    arithmetic implementation
    /// @throws InvalidArgumentError on an invalid config
    HnswIndex(VectorAccessor accessor,
              Metric metric,
              HnswConfig config,
              const DistanceKernel& kernel = active_kernel());

    /// Inserts the vector at `local_id`, which the store must already hold.
    void add(LocalId local_id) override;

    /// Marks a node unsearchable.
    ///
    /// The node stays in the graph, still routing traffic, but is never
    /// returned. Physically removing it would mean repairing every neighbour
    /// list that points at it and risking disconnection of the region it was
    /// bridging. See docs/decisions/ADR-0011-tombstones.md.
    void remove(LocalId local_id) override;

    [[nodiscard]] std::vector<Candidate> search(VectorView query,
                                                const SearchParams& params) const override;

    [[nodiscard]] std::size_t size() const noexcept override { return live_count_; }

    [[nodiscard]] IndexType type() const noexcept override { return IndexType::kHnsw; }

    [[nodiscard]] Metric metric() const noexcept override { return metric_; }

    [[nodiscard]] std::size_t index_bytes() const noexcept override;
    [[nodiscard]] std::string describe() const override;

    [[nodiscard]] const HnswConfig& config() const noexcept { return config_; }

    /// Number of nodes in the graph, including tombstoned ones.
    [[nodiscard]] std::size_t node_count() const noexcept { return node_level_.size(); }

    /// The highest layer any node occupies. 0 for a single-layer graph.
    [[nodiscard]] std::size_t max_level() const noexcept { return max_level_; }

    /// Where every search begins: the node with the highest level.
    [[nodiscard]] LocalId entry_point() const noexcept { return entry_point_; }

    /// The layer each node's tower reaches.
    [[nodiscard]] std::span<const std::uint8_t> node_levels() const noexcept {
        return node_level_;
    }

    /// Neighbours of `node` on `layer`. Empty if the node does not reach it.
    /// For diagnostics, `vectordb check`, and the persistence layer.
    [[nodiscard]] std::span<const LocalId> neighbours(LocalId node,
                                                      std::size_t layer) const noexcept;

    /// Refreshes the view after the store has grown or reallocated.
    void set_accessor(VectorAccessor accessor) noexcept { accessor_ = accessor; }

    /// Reserves graph storage for `count` nodes, so a bulk build performs a
    /// handful of allocations rather than a logarithmic number of copies of an
    /// increasingly large array.
    void reserve(std::size_t count);

    // --- Used by the persistence layer to restore a graph verbatim ----------

    /// Restores a fully-built graph. Every reference is validated by the
    /// caller before this is called; see `hnsw_file.cpp`.
    void restore(std::vector<std::uint8_t> node_levels,
                 std::vector<LocalId> layer0_links,
                 std::vector<LocalId> upper_links,
                 std::vector<std::uint32_t> upper_offsets,
                 LocalId entry_point,
                 std::size_t max_level,
                 std::size_t live_count,
                 std::uint64_t rng_state);

    [[nodiscard]] std::span<const LocalId> layer0_links() const noexcept {
        return layer0_links_;
    }

    [[nodiscard]] std::span<const LocalId> upper_links() const noexcept { return upper_links_; }

    [[nodiscard]] std::span<const std::uint32_t> upper_offsets() const noexcept {
        return upper_offsets_;
    }

    [[nodiscard]] std::uint64_t rng_state() const noexcept;

private:
    /// Slot 0 of each node's block holds the neighbour count, so the count and
    /// the first neighbours share a cache line.
    [[nodiscard]] std::size_t layer0_stride() const noexcept { return config_.max_m0() + 1; }

    [[nodiscard]] std::size_t upper_stride() const noexcept { return config_.m + 1; }

    [[nodiscard]] LocalId* mutable_links(LocalId node, std::size_t layer) noexcept;
    [[nodiscard]] const LocalId* links(LocalId node, std::size_t layer) const noexcept;
    void set_neighbours(LocalId node, std::size_t layer, std::span<const LocalId> ids) noexcept;

    /// Draws a node's top layer from an exponentially decaying distribution.
    [[nodiscard]] std::size_t random_level() noexcept;

    /// Greedy descent with a candidate list of width 1: from `entry`, keep
    /// moving to whichever neighbour is closer to the query until none is.
    [[nodiscard]] LocalId greedy_descend(VectorView query,
                                         float query_norm,
                                         LocalId entry,
                                         std::size_t layer) const noexcept;

    /// The core routine. Best-first search of one layer, returning the `ef`
    /// closest nodes found.
    void search_layer(VectorView query,
                      float query_norm,
                      std::span<const LocalId> entry_points,
                      std::size_t ef,
                      std::size_t layer,
                      std::vector<Candidate>& out) const;

    /// The neighbour-selection heuristic that gives HNSW its connectivity.
    /// @param candidates  distances are to the node being connected, and this
    ///                     is sorted in place
    void select_neighbours(std::vector<Candidate>& candidates,
                           std::size_t limit,
                           std::vector<LocalId>& out) const;

    /// Adds `to` to `from`'s neighbour list on `layer`, re-pruning if full.
    void link(LocalId from, LocalId to, std::size_t layer);

    VectorAccessor accessor_;
    Metric metric_;
    HnswConfig config_;
    DistanceFunction distance_;

    /// Per node: the highest layer it occupies. u8 caps the graph at 255
    /// layers, which at `1/ln(M)` spacing corresponds to vastly more vectors
    /// than `LocalId` can address.
    std::vector<std::uint8_t> node_level_;

    /// Flat, stride `max_m0() + 1`, one block per node.
    std::vector<LocalId> layer0_links_;

    /// Append-only arena for layers >= 1, stride `m + 1` per (node, layer).
    std::vector<LocalId> upper_links_;

    /// Index into `upper_links_` for each node, or `kNoUpperLinks`.
    std::vector<std::uint32_t> upper_offsets_;

    LocalId entry_point_ = kInvalidLocalId;
    std::size_t max_level_ = 0;
    std::size_t live_count_ = 0;

    /// Level generation. Mutable state, so `add` is not thread-safe — which is
    /// already true for other reasons and is documented as the contract.
    mutable DeterministicRng rng_;
};

}  // namespace vectordb
