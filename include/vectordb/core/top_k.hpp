// SPDX-License-Identifier: MIT
//
// Keeping the k best of N, without sorting N.
//
// Learning note: learnings/30-vector-database/04-top-k-search.md
#pragma once

#include <cstddef>
#include <limits>
#include <vector>

#include <vectordb/core/search_result.hpp>

namespace vectordb {

/// A bounded collector that keeps the `k` best candidates it is offered.
///
/// ## Why not just sort everything?
///
/// Sorting N candidates is `O(N log N)` time and `O(N)` memory. For 5,000,000
/// vectors and `k = 10` that is 5,000,000 stored distances to find ten of them.
///
/// A bounded max-heap of size k is `O(N log k)` time and `O(k)` memory. With
/// `k = 10`, `log2(k)` is about 3.3 — but the real win is the memory: 10 floats
/// instead of 5,000,000, so the heap lives permanently in L1 cache while the
/// scan streams past it.
///
/// ## Why a *max*-heap when we want the smallest keys?
///
/// The heap holds the current best k. The only question asked of it per
/// candidate is "are you better than the worst thing I am keeping?", so the
/// element that must be instantly reachable is the **worst** one. A max-heap
/// (by rank key, where larger is worse) puts exactly that at the root.
///
/// This makes the common case `O(1)`: once the heap is full, most candidates
/// lose a single comparison against `worst()` and are discarded without ever
/// touching the heap structure. Only an improvement pays the `O(log k)`
/// replacement.
///
/// ## Allocation
///
/// The backing storage is reserved once, in the constructor. `offer()` never
/// allocates — it is called once per candidate, and an allocation in that loop
/// would dwarf the arithmetic it is there to support.
class TopKCollector {
public:
    /// @throws InvalidArgumentError if `k` is 0
    explicit TopKCollector(std::size_t k);

    /// Considers one candidate. `O(1)` when rejected, `O(log k)` when accepted.
    void offer(LocalId local_id, RankKey key) noexcept;

    /// Conservative test: false means `offer` would certainly reject this key.
    ///
    /// True does not guarantee acceptance — ties on the key are resolved by
    /// `local_id`, which this function does not see. It is a pre-filter, so it
    /// errs towards yes: a caller may use it to skip work it would otherwise do
    /// *after* the distance (a metadata lookup, say) without ever changing
    /// which results come back.
    [[nodiscard]] bool would_accept(RankKey key) const noexcept;

    /// The worst key currently kept, or `+inf` while the heap is not yet full.
    ///
    /// `+inf` is the right sentinel rather than a special case: everything
    /// compares better than it, so an unfilled heap accepts everything through
    /// the same code path a full one uses.
    [[nodiscard]] RankKey worst() const noexcept;

    [[nodiscard]] bool full() const noexcept { return heap_.size() >= k_; }

    [[nodiscard]] std::size_t size() const noexcept { return heap_.size(); }

    [[nodiscard]] std::size_t k() const noexcept { return k_; }

    [[nodiscard]] bool empty() const noexcept { return heap_.empty(); }

    /// Returns the kept candidates ordered best first, leaving the collector
    /// empty. `O(k log k)`, paid once per search rather than once per candidate.
    [[nodiscard]] std::vector<Candidate> take_sorted();

    /// Read-only view of the kept candidates in **heap order**, which is not
    /// sorted order. For diagnostics; use `take_sorted()` for results.
    [[nodiscard]] const std::vector<Candidate>& unordered() const noexcept { return heap_; }

    /// Empties the collector but keeps the allocation, so one collector can be
    /// reused across a batch of queries without re-allocating per query.
    void clear() noexcept { heap_.clear(); }

    /// Empties the collector and changes k, reserving if it grew.
    /// @throws InvalidArgumentError if `k` is 0
    void reset(std::size_t k);

private:
    std::size_t k_;
    std::vector<Candidate> heap_;
};

/// Merges several sorted candidate lists into the overall best `k`.
///
/// Used to combine per-thread results from a parallel scan. Inputs must each be
/// sorted best-first, as `take_sorted()` returns them.
[[nodiscard]] std::vector<Candidate> merge_top_k(
    const std::vector<std::vector<Candidate>>& sorted_lists, std::size_t k);

}  // namespace vectordb
