// SPDX-License-Identifier: MIT
//
// HNSW's tuning parameters, in one place.
//
// Learning note: learnings/40-search-indexes/04-hnsw-algorithm.md
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace vectordb {

/// Parameters controlling graph construction and search.
///
/// The three that matter, and what each trades:
///
/// | | raising it costs | raising it buys |
/// |---|---|---|
/// | `m`               | memory, build time | recall, at every ef |
/// | `ef_construction` | build time only    | a better graph, permanently |
/// | `ef_search`       | query latency      | recall, tunable at runtime |
///
/// The asymmetry is the useful part. `ef_search` is a runtime dial — the same
/// index answers a fast approximate query and a slow accurate one. `m` and
/// `ef_construction` are baked into the graph and changing them means a rebuild.
struct HnswConfig {
    /// Maximum neighbours per node on layers above 0.
    ///
    /// The dominant memory parameter: the graph costs roughly
    /// `N * M * 2 * 4` bytes of edges. Typical values are 8-48; 16 is a good
    /// default for most embedding data. Below about 5 the graph loses
    /// connectivity and recall collapses; above about 64 the returns are small
    /// and the memory is not.
    std::size_t m = 16;

    /// Candidate-list width while inserting.
    ///
    /// Higher means each new node considers more potential neighbours and picks
    /// better ones, so the graph is permanently better. Costs build time and
    /// nothing at query time — which makes it the cheapest quality knob if you
    /// build once and query often.
    std::size_t ef_construction = 200;

    /// Default candidate-list width at query time.
    ///
    /// The recall/latency dial. Must be at least `k`, and is raised to `k`
    /// automatically if a query asks for more results than this.
    std::size_t ef_search = 64;

    /// Seed for level assignment.
    ///
    /// HNSW picks each node's top layer randomly. Fixing the seed makes a build
    /// reproducible, which is what lets a recall regression be told apart from
    /// ordinary randomness.
    std::uint64_t seed = 42;

    /// Neighbour budget on layer 0, which is `2 * m` by convention.
    ///
    /// Layer 0 holds every node and is where search spends nearly all of its
    /// time, so it gets a denser graph. This is the original paper's
    /// recommendation and it is a real effect, not folklore.
    [[nodiscard]] std::size_t max_m0() const noexcept { return m * 2; }

    /// Neighbour budget on `layer`.
    [[nodiscard]] std::size_t max_neighbours(std::size_t layer) const noexcept {
        return layer == 0 ? max_m0() : m;
    }

    /// The level-generation constant, `1 / ln(m)`.
    ///
    /// Chosen so that each layer holds roughly `1/m` of the layer below it,
    /// which makes the expected number of layers `ln(N)/ln(m)` and the descent
    /// through them logarithmic. This is the parameter that makes HNSW
    /// sub-linear rather than merely clever.
    [[nodiscard]] double level_multiplier() const noexcept;

    /// @throws InvalidArgumentError if any parameter is out of range
    void validate() const;

    /// e.g. "M=16, efConstruction=200, efSearch=64, seed=42"
    [[nodiscard]] std::string describe() const;
};

}  // namespace vectordb
