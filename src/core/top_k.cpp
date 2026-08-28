// SPDX-License-Identifier: MIT
#include <algorithm>
#include <string>

#include <vectordb/core/error.hpp>
#include <vectordb/core/top_k.hpp>

namespace vectordb {
namespace {

/// Heap comparator. `std::push_heap` and friends build a **max**-heap with
/// respect to the comparator they are given, so passing "a is better than b"
/// puts the *worst* candidate at the root — which is precisely the element the
/// hot loop needs in O(1).
constexpr CandidateBetter kBetter{};

constexpr RankKey kUnbounded = std::numeric_limits<RankKey>::infinity();

}  // namespace

TopKCollector::TopKCollector(std::size_t k) : k_(k) {
    if (k == 0) {
        throw InvalidArgumentError("top-k requires k >= 1");
    }
    // One allocation up front so offer() never allocates. The +1 is not needed
    // by the current implementation (it replaces rather than growing), but
    // reserving exactly k also means a future push-then-pop variant would not
    // silently reallocate on the hottest loop in the system.
    heap_.reserve(k);
}

bool TopKCollector::would_accept(RankKey key) const noexcept {
    if (heap_.size() < k_) {
        return true;
    }
    // Deliberately `<=`, not `<`. This is a *conservative* pre-filter: it may
    // say yes to a candidate that offer() then rejects, but it must never say
    // no to one offer() would have kept.
    //
    // The asymmetry exists because acceptance is decided by the full ordering
    // (key, then local_id) and this function only has the key. A candidate that
    // ties the current worst on key can still win on local_id, so refusing ties
    // here would silently drop it — and any caller using this to skip work
    // would then produce different results from one that did not.
    return key <= heap_.front().key;
}

void TopKCollector::offer(LocalId local_id, RankKey key) noexcept {
    if (heap_.size() < k_) {
        heap_.push_back(Candidate{local_id, key});
        std::push_heap(heap_.begin(), heap_.end(), kBetter);
        return;
    }

    const Candidate candidate{local_id, key};

    // The single comparison that most candidates lose. Everything after this
    // point is O(log k) and runs rarely; this line runs N times.
    if (!kBetter(candidate, heap_.front())) {
        return;
    }

    // Replace the root, then sift down. pop_heap moves the root to the back in
    // O(log k); overwriting it and pushing again is the standard
    // replace-the-worst idiom and avoids any allocation.
    std::pop_heap(heap_.begin(), heap_.end(), kBetter);
    heap_.back() = candidate;
    std::push_heap(heap_.begin(), heap_.end(), kBetter);
}

RankKey TopKCollector::worst() const noexcept {
    if (heap_.size() < k_) {
        return kUnbounded;
    }
    return heap_.front().key;
}

std::vector<Candidate> TopKCollector::take_sorted() {
    // sort_heap over a max-heap yields ascending order by the comparator, which
    // is best-first here. O(k log k), paid once per search.
    std::sort_heap(heap_.begin(), heap_.end(), kBetter);
    std::vector<Candidate> sorted = std::move(heap_);
    heap_.clear();
    heap_.reserve(k_);
    return sorted;
}

void TopKCollector::reset(std::size_t k) {
    if (k == 0) {
        throw InvalidArgumentError("top-k requires k >= 1");
    }
    heap_.clear();
    k_ = k;
    if (heap_.capacity() < k) {
        heap_.reserve(k);
    }
}

std::vector<Candidate> merge_top_k(const std::vector<std::vector<Candidate>>& sorted_lists,
                                   std::size_t k) {
    if (k == 0) {
        throw InvalidArgumentError("top-k requires k >= 1");
    }

    // A k-way merge with a heap of cursors would be asymptotically better, but
    // the number of lists is the thread count (single digits) and each holds at
    // most k entries. Re-offering everything into one collector is O(T*k log k)
    // with a tiny constant and far less code to get wrong. If T ever becomes
    // large this is the place to revisit.
    std::size_t total = 0;
    for (const std::vector<Candidate>& list : sorted_lists) {
        total += list.size();
    }
    if (total == 0) {
        return {};
    }

    TopKCollector collector(std::min(k, total));
    for (const std::vector<Candidate>& list : sorted_lists) {
        for (const Candidate& candidate : list) {
            // Each list is sorted best-first, so once one candidate fails to be
            // accepted, every later candidate in that list will fail too.
            if (!collector.would_accept(candidate.key)) {
                break;
            }
            collector.offer(candidate.local_id, candidate.key);
        }
    }
    return collector.take_sorted();
}

}  // namespace vectordb
