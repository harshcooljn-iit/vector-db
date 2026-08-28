// SPDX-License-Identifier: MIT
#include <vectordb/core/error.hpp>
#include <vectordb/core/top_k.hpp>
#include <vectordb/index/brute_force_index.hpp>

namespace vectordb {

BruteForceIndex::BruteForceIndex(VectorAccessor accessor,
                                 Metric metric,
                                 const DistanceKernel& kernel) noexcept
    : accessor_(accessor), metric_(metric), distance_(metric, kernel) {}

void BruteForceIndex::add(LocalId /*local_id*/) noexcept {
    // Nothing to do. The store already holds the vector and marks the slot
    // live; this index reads both directly on every search. See the class
    // comment — the emptiness here is the design, not an omission.
}

void BruteForceIndex::remove(LocalId /*local_id*/) noexcept {
    // Likewise: the store's tombstone is authoritative and is checked in the
    // scan loop.
}

void BruteForceIndex::search_range(VectorView query,
                                   LocalId first,
                                   LocalId last,
                                   TopKCollector& collector) const noexcept {
    // The query norm is needed once per *query* for cosine, not once per
    // candidate. Hoisting it out of the loop is the difference between two
    // O(D) passes per candidate and one.
    const float query_norm = metric_ == Metric::kCosine ? l2_norm(query) : 0.0F;

    for (LocalId id = first; id < last; ++id) {
        if (!accessor_.live(id)) {
            continue;
        }
        const float key =
            distance_.with_norms(query, query_norm, accessor_.vector(id), accessor_.norm(id));
        collector.offer(id, key);
    }
}

std::vector<Candidate> BruteForceIndex::search(VectorView query,
                                               const SearchParams& params) const {
    if (params.k == 0) {
        throw InvalidArgumentError("search requires k >= 1");
    }
    if (query.size() != static_cast<std::size_t>(accessor_.dimension())) {
        throw DimensionMismatchError(accessor_.dimension(),
                                     static_cast<Dimension>(query.size()));
    }

    // Validation happens here, before the loop, and never inside it. Everything
    // below this point may assume a well-formed query and a total order over
    // rank keys — which is exactly what the NaN policy buys us (ADR-0007).
    TopKCollector collector(params.k);
    search_range(query, 0, static_cast<LocalId>(accessor_.size()), collector);
    return collector.take_sorted();
}

std::size_t BruteForceIndex::size() const noexcept {
    std::size_t live = 0;
    for (LocalId id = 0; id < static_cast<LocalId>(accessor_.size()); ++id) {
        live += accessor_.live(id) ? 1U : 0U;
    }
    return live;
}

std::string BruteForceIndex::describe() const {
    std::string text = "brute_force(metric=";
    text += metric_name(metric_);
    text += ", kernel=";
    text += distance_.kernel().name;
    text += ")";
    return text;
}

}  // namespace vectordb
