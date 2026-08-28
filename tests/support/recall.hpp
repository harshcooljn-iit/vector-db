// SPDX-License-Identifier: MIT
//
// Measuring an approximate index against an exact one.
#pragma once

#include <algorithm>
#include <unordered_set>
#include <vector>

#include <vectordb/core/search_result.hpp>
#include <vectordb/index/brute_force_index.hpp>
#include <vectordb/index/vector_index.hpp>

namespace vectordb::testing {

/// Fraction of the true top-k that `approximate` actually returned.
///
/// Set intersection, not position-by-position comparison. Demanding the same
/// *order* from an approximate algorithm is how you get a permanently red,
/// permanently ignored test — and it measures the wrong thing anyway, since two
/// results at nearly equal distance are equally good answers.
[[nodiscard]] inline double recall_at_k(const std::vector<Candidate>& exact,
                                        const std::vector<Candidate>& approximate,
                                        std::size_t k) {
    if (exact.empty()) {
        return 1.0;
    }
    const std::size_t limit = std::min(k, exact.size());

    std::unordered_set<LocalId> truth;
    truth.reserve(limit);
    for (std::size_t i = 0; i < limit; ++i) {
        truth.insert(exact[i].local_id);
    }

    std::size_t hits = 0;
    for (std::size_t i = 0; i < std::min(k, approximate.size()); ++i) {
        hits += truth.count(approximate[i].local_id);
    }
    return static_cast<double>(hits) / static_cast<double>(limit);
}

/// Mean recall@k of `index` against brute force over `queries`.
[[nodiscard]] inline double mean_recall(const VectorIndex& index,
                                        const BruteForceIndex& oracle,
                                        const VectorArray& queries,
                                        std::size_t k,
                                        std::size_t ef_search = 0) {
    double total = 0.0;
    for (LocalId q = 0; q < queries.size(); ++q) {
        const std::vector<Candidate> exact = oracle.search(queries[q], SearchParams{.k = k});
        const std::vector<Candidate> found =
            index.search(queries[q], SearchParams{.k = k, .ef_search = ef_search});
        total += recall_at_k(exact, found, k);
    }
    return total / static_cast<double>(queries.size());
}

}  // namespace vectordb::testing
