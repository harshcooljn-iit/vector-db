// SPDX-License-Identifier: MIT
//
// What a search returns, internally and externally.
#pragma once

#include <cstddef>
#include <vector>

#include <vectordb/core/types.hpp>
#include <vectordb/distance/distance.hpp>

namespace vectordb {

/// An internal, in-flight search candidate.
///
/// Uses `LocalId` and a `RankKey` (smaller is better) because that is what the
/// index works in. Translation to the user's vocabulary happens once, at the
/// end, for the k survivors — never for the millions of candidates that were
/// considered and dropped.
struct Candidate {
    LocalId local_id;
    RankKey key;
};

/// Orders candidates worst-last: `a` sorts before `b` when `a` is the better
/// result.
///
/// Ties are broken by `local_id` ascending, which is not cosmetic. Without a
/// tie-break, two runs over the same data can return different-but-equally-good
/// result sets — and then a recall comparison between HNSW and brute force
/// measures heap ordering noise as if it were an index defect. Determinism here
/// makes the recall tests meaningful.
struct CandidateBetter {
    [[nodiscard]] constexpr bool operator()(const Candidate& a,
                                            const Candidate& b) const noexcept {
        if (a.key != b.key) {
            return a.key < b.key;
        }
        return a.local_id < b.local_id;
    }
};

/// One result as the caller sees it.
///
/// `score` is the metric's natural, human-facing value: squared distance for
/// `l2` (smaller better), cosine similarity in [-1, 1] for `cosine` (larger
/// better), the raw inner product for `dot` (larger better). Use
/// `higher_score_is_better(metric)` when a caller needs to know which.
struct SearchResult {
    VectorId id;
    float score;
};

/// Translates finished candidates into user-facing results.
///
/// @param candidates    best first, as produced by `TopKCollector::take_sorted`
/// @param metric        decides how a rank key becomes a score
/// @param to_vector_id  maps a slot back to its stable id
template<typename LocalToVectorId>
[[nodiscard]] std::vector<SearchResult> to_search_results(
    const std::vector<Candidate>& candidates, Metric metric, LocalToVectorId to_vector_id) {
    std::vector<SearchResult> results;
    results.reserve(candidates.size());
    for (const Candidate& candidate : candidates) {
        results.push_back(SearchResult{to_vector_id(candidate.local_id),
                                       score_from_rank_key(metric, candidate.key)});
    }
    return results;
}

}  // namespace vectordb
