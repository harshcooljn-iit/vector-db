// SPDX-License-Identifier: MIT
//
// Exact search by scanning everything. The correctness oracle.
//
// Learning note: learnings/40-search-indexes/01-brute-force-index.md
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <vectordb/distance/distance.hpp>
#include <vectordb/index/vector_index.hpp>

namespace vectordb {

/// Computes every distance and keeps the best k.
///
/// ## It holds no state
///
/// The store already knows which slots exist and which are tombstoned, so
/// there is nothing for this class to maintain: `add` and `remove` are
/// genuinely empty, and `index_bytes()` is genuinely 0. That is not a stub —
/// it is the defining property. Brute force cannot be stale, cannot be
/// corrupted, and needs no persistence, because it *is* the data.
///
/// ## Why keep it once HNSW exists
///
/// Three reasons, in order of importance:
///
/// 1. **It is the oracle.** HNSW is approximate and fails silently — results
///    come back, distances look plausible, latency is fine. "Is HNSW correct?"
///    is only answerable relative to something exact. See
///    docs/decisions/ADR-0006-brute-force-first.md.
/// 2. **It is faster below a few tens of thousands of vectors.** No graph to
///    traverse, no index to build or load, perfectly sequential memory access.
/// 3. **It is the fallback.** If an index file fails validation, exact search
///    still works while the graph is rebuilt.
///
/// ## Cost
///
/// `O(N * D)` per query, and it is memory-bandwidth-bound rather than
/// arithmetic-bound: at N = 1,000,000 and D = 768 each query streams 3 GB.
/// That is the number HNSW exists to avoid.
class BruteForceIndex final : public VectorIndex {
public:
    /// @param accessor  read-only view of the store; must outlive this index
    /// @param metric    how distance is defined
    /// @param kernel    which arithmetic implementation to use; defaults to the
    ///                  fastest this CPU supports. Tests pin it to scalar so a
    ///                  SIMD regression cannot hide behind a passing oracle.
    BruteForceIndex(VectorAccessor accessor,
                    Metric metric,
                    const DistanceKernel& kernel = active_kernel()) noexcept;

    void add(LocalId local_id) noexcept override;
    void remove(LocalId local_id) noexcept override;

    [[nodiscard]] std::vector<Candidate> search(VectorView query,
                                                const SearchParams& params) const override;

    /// Same as `search`, but appends into a caller-supplied collector.
    ///
    /// Lets a parallel scan give each worker its own collector over a slice,
    /// with no per-worker allocation, and lets a batch of queries reuse one
    /// collector across all of them.
    void search_range(VectorView query,
                      LocalId first,
                      LocalId last,
                      TopKCollector& collector) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept override;

    [[nodiscard]] IndexType type() const noexcept override { return IndexType::kBruteForce; }

    [[nodiscard]] Metric metric() const noexcept override { return metric_; }

    [[nodiscard]] std::size_t index_bytes() const noexcept override { return 0; }

    [[nodiscard]] std::string describe() const override;

    /// Refreshes the view after the store has grown or reallocated.
    void set_accessor(VectorAccessor accessor) noexcept { accessor_ = accessor; }

private:
    VectorAccessor accessor_;
    Metric metric_;
    DistanceFunction distance_;
};

}  // namespace vectordb
