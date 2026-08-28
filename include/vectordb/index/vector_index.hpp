// SPDX-License-Identifier: MIT
//
// The interface both index implementations satisfy.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <vectordb/core/search_result.hpp>
#include <vectordb/core/top_k.hpp>
#include <vectordb/core/types.hpp>
#include <vectordb/core/vector.hpp>
#include <vectordb/index/vector_accessor.hpp>

namespace vectordb {

/// Per-query knobs.
///
/// A struct rather than a parameter list so that adding an option later is a
/// source-compatible change; every field has a documented default meaning
/// "use whatever the index was configured with".
struct SearchParams {
    /// How many results to return. Must be >= 1.
    std::size_t k = 10;

    /// HNSW candidate-list width at query time. 0 means "use the index's
    /// configured default". Ignored by brute force, which is exact and has
    /// nothing to trade.
    std::size_t ef_search = 0;
};

/// A structure that answers nearest-neighbour queries over a `VectorAccessor`.
///
/// ## The ownership contract
///
/// An index does **not** own vectors. The store is written first, then the
/// index is told about the change:
///
/// ```
/// store.insert(id, vector);   // authoritative
/// index->add(local_id);       // derived
/// ```
///
/// So `add` takes only a slot number — the vector is already reachable through
/// the accessor. This ordering is what makes an index disposable: if the index
/// file is corrupt, it is rebuilt by replaying `add` over every live slot.
///
/// ## Why virtual here, when `VectorAccessor` is deliberately not?
///
/// Granularity. `search()` is called once per query and then does thousands of
/// distance computations; one indirect call is free. `VectorAccessor::vector()`
/// is called once per candidate, inside that loop, where it would not be.
/// Virtual dispatch is priced per call site, not per codebase.
class VectorIndex {
public:
    VectorIndex() = default;
    virtual ~VectorIndex() = default;

    VectorIndex(const VectorIndex&) = delete;
    VectorIndex& operator=(const VectorIndex&) = delete;
    VectorIndex(VectorIndex&&) = delete;
    VectorIndex& operator=(VectorIndex&&) = delete;

    /// Registers a slot the store has already populated.
    ///
    /// Must be called with slots in ascending order for a fresh build; an index
    /// may rely on that to size its own arrays without a map.
    virtual void add(LocalId local_id) = 0;

    /// Marks a slot as no longer searchable.
    ///
    /// The store's tombstone is authoritative, so an implementation that scans
    /// the accessor (brute force) may legitimately do nothing here.
    virtual void remove(LocalId local_id) = 0;

    /// Returns up to `params.k` candidates, best first.
    ///
    /// Thread safety: safe to call concurrently with other searches. Not safe
    /// concurrently with `add` or `remove`. See docs/concurrency.md.
    ///
    /// @throws InvalidArgumentError on k == 0 or a dimension mismatch
    [[nodiscard]] virtual std::vector<Candidate> search(VectorView query,
                                                        const SearchParams& params) const = 0;

    /// Number of live, searchable entries.
    [[nodiscard]] virtual std::size_t size() const noexcept = 0;

    [[nodiscard]] virtual IndexType type() const noexcept = 0;

    [[nodiscard]] virtual Metric metric() const noexcept = 0;

    /// Bytes this index occupies *beyond* the vectors themselves. Brute force
    /// reports 0, which is the honest answer and a large part of its appeal.
    [[nodiscard]] virtual std::size_t index_bytes() const noexcept = 0;

    /// One-line human-readable description, e.g. "hnsw(M=16, ef_construction=200)".
    /// Printed by `vectordb info`.
    [[nodiscard]] virtual std::string describe() const = 0;
};

}  // namespace vectordb
