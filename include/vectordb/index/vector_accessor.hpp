// SPDX-License-Identifier: MIT
//
// How an index reads the vectors it searches.
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

#include <vectordb/core/types.hpp>
#include <vectordb/core/vector.hpp>
#include <vectordb/core/vector_array.hpp>

namespace vectordb {

/// A non-owning, read-only view of the vector store, as an index sees it.
///
/// ## Why not have the index own its vectors?
///
/// Because there would then be two copies of N x D floats — 6 GB instead of 3
/// at a million 768-dimensional vectors — and two things that could disagree
/// about what the data is. The vector store is authoritative; an index is
/// derived, disposable state that can always be rebuilt from it.
///
/// ## Why not a virtual interface?
///
/// This is read once per candidate, which is the innermost loop of the system.
/// A virtual `vector(LocalId)` would put an indirect call in front of a pointer
/// addition, and — worse — would block inlining, so the compiler could no
/// longer keep the base pointer in a register across the scan.
///
/// So this is a concrete type: three pointers and a length, copied by value.
/// It still decouples the index from the store's internals (tombstone
/// representation, norm caching, how the ids are mapped), which is the part of
/// the abstraction that was actually worth having.
///
/// ## Lifetime
///
/// Non-owning. Valid only while the store is alive and has not reallocated.
/// An index holds one for the duration of a search, not across an insertion.
class VectorAccessor {
public:
    /// @param vectors  contiguous storage, indexed by `LocalId`
    /// @param norms    cached L2 norm per slot, same length as `vectors`
    /// @param live     0 = tombstoned, non-zero = live; same length
    VectorAccessor(const VectorArray& vectors,
                   std::span<const float> norms,
                   std::span<const std::uint8_t> live) noexcept
        : vectors_(&vectors), norms_(norms), live_(live) {
        assert(norms.size() == vectors.size());
        assert(live.size() == vectors.size());
    }

    /// The vector in slot `id`. Unchecked — this is the hot path.
    [[nodiscard]] VectorView vector(LocalId id) const noexcept { return (*vectors_)[id]; }

    /// The precomputed L2 norm of slot `id`.
    ///
    /// Cached because cosine otherwise needs an O(D) pass per candidate just to
    /// divide it out. Four bytes per vector to avoid 2*D bytes of traffic per
    /// query is a 1536:1 return at D = 768.
    [[nodiscard]] float norm(LocalId id) const noexcept { return norms_[id]; }

    /// False when the slot has been deleted.
    ///
    /// A byte per slot rather than a packed bit: 1 MB per million vectors
    /// against 3 GB of payload is 0.03%, and a plain byte load has no shift or
    /// mask in the inner loop. A bitset would be the right call if this ever
    /// became the working set, which it is not.
    [[nodiscard]] bool live(LocalId id) const noexcept { return live_[id] != 0U; }

    /// Total number of slots, live and tombstoned. Slot ids run `[0, size())`.
    [[nodiscard]] std::size_t size() const noexcept { return vectors_->size(); }

    [[nodiscard]] Dimension dimension() const noexcept { return vectors_->dimension(); }

    /// Base pointer of the contiguous block, for code that wants to walk it
    /// directly rather than slot by slot.
    [[nodiscard]] const float* data() const noexcept { return vectors_->data(); }

private:
    const VectorArray* vectors_;
    std::span<const float> norms_;
    std::span<const std::uint8_t> live_;
};

}  // namespace vectordb
