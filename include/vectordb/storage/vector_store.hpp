// SPDX-License-Identifier: MIT
//
// The authoritative in-memory model of a database's vectors.
//
// Everything else — every index, every cached norm, every search result — is
// derived from this and can be rebuilt from it. Nothing else in the system has
// that status, which is why a corrupt index is recoverable and a corrupt vector
// store is data loss.
//
// Learning note: learnings/50-storage/01-persistent-storage.md
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include <vectordb/core/types.hpp>
#include <vectordb/core/vector.hpp>
#include <vectordb/core/vector_array.hpp>
#include <vectordb/index/vector_accessor.hpp>

namespace vectordb {

/// Holds the vectors, their cached norms, their liveness flags and the mapping
/// between stable `VectorId`s and dense `LocalId` slots.
///
/// ## Four parallel arrays, not an array of structs
///
/// ```
/// vectors_      [ v0 ][ v1 ][ v2 ]   D floats each, contiguous
/// norms_        [ n0 ][ n1 ][ n2 ]   one float each
/// live_         [  1 ][  0 ][  1 ]   one byte each
/// local_to_id_  [ 42 ][  7 ][ 99 ]   one VectorId each
/// ```
///
/// They are read by different loops at wildly different rates. The scan touches
/// `vectors_` N times and `norms_`/`live_` N times; `local_to_id_` is read k
/// times, once per *result*. Interleaving them into one struct would drag the
/// id — and its padding — through every cache line the distance loop pulls in.
///
/// ## Deletion is a tombstone
///
/// `remove` clears the liveness byte and drops the id from the lookup map. The
/// floats stay where they are. Physically erasing slot 5 of a million would
/// mean either moving 999,995 vectors (and renumbering every graph edge that
/// referenced them) or leaving a hole that every future scan must special-case.
/// Tombstones make deletion `O(1)` and defer the cost to an explicit compaction.
/// See docs/decisions/ADR-0011-tombstones.md.
///
/// ## Thread safety
///
/// None. Concurrent reads are safe; any mutation requires exclusive access. The
/// database layer owns the lock — see docs/concurrency.md.
class VectorStore {
public:
    /// @param dimension  fixed for the life of the store
    /// @param normalize  when true, every inserted vector is scaled to unit
    ///                   length before storage. Lossy: the original magnitude
    ///                   is gone. Makes cosine cost exactly what a dot product
    ///                   costs, because every cached norm becomes 1.
    /// @throws InvalidArgumentError on a degenerate dimension
    explicit VectorStore(Dimension dimension, bool normalize = false);

    /// Inserts a new vector.
    ///
    /// Duplicate policy is **reject**: an id that is already live raises
    /// `DuplicateIdError`. Silent replacement is available as `upsert`, so the
    /// caller states which they meant rather than discovering it later.
    ///
    /// An id that was previously deleted is free again and gets a fresh slot.
    ///
    /// @throws DuplicateIdError, DimensionMismatchError, InvalidVectorError
    LocalId insert(VectorId id, VectorView vector);

    /// Inserts, or overwrites the existing vector for `id` in place.
    /// Returns the slot, which is unchanged for an existing id.
    LocalId upsert(VectorId id, VectorView vector);

    /// Tombstones `id`. Returns false if it was not present.
    bool remove(VectorId id);

    /// Slot for `id`, or `std::nullopt` if it is absent or deleted.
    [[nodiscard]] std::optional<LocalId> find(VectorId id) const;

    [[nodiscard]] bool contains(VectorId id) const { return find(id).has_value(); }

    /// The stable id of a slot. Valid for tombstoned slots too, which is what
    /// lets a rebuild report *which* vector a bad slot belonged to.
    /// @throws InvalidArgumentError if the slot is out of range
    [[nodiscard]] VectorId vector_id(LocalId local_id) const;

    /// The stored vector for `id` — normalized, if the store normalizes.
    /// @throws NotFoundError
    [[nodiscard]] VectorView get(VectorId id) const;

    /// Read-only view for indexes. Invalidated by any mutation.
    [[nodiscard]] VectorAccessor accessor() const noexcept {
        return VectorAccessor(vectors_, norms_, live_);
    }

    [[nodiscard]] VectorView vector(LocalId local_id) const { return vectors_.at(local_id); }

    [[nodiscard]] float norm(LocalId local_id) const noexcept { return norms_[local_id]; }

    [[nodiscard]] bool live(LocalId local_id) const noexcept { return live_[local_id] != 0U; }

    /// Number of vectors a search will actually consider.
    [[nodiscard]] std::size_t live_count() const noexcept { return live_count_; }

    /// Number of slots, including tombstoned ones. Slot ids run `[0, slot_count())`.
    [[nodiscard]] std::size_t slot_count() const noexcept { return vectors_.size(); }

    [[nodiscard]] std::size_t tombstone_count() const noexcept {
        return slot_count() - live_count_;
    }

    /// Fraction of slots that are dead, in [0, 1]. `vectordb stats` reports it;
    /// a high value is the signal to compact.
    [[nodiscard]] double tombstone_ratio() const noexcept;

    [[nodiscard]] Dimension dimension() const noexcept { return vectors_.dimension(); }

    [[nodiscard]] bool normalizes() const noexcept { return normalize_; }

    [[nodiscard]] bool empty() const noexcept { return live_count_ == 0; }

    /// Total bytes held: payload, norms, liveness and both id maps.
    [[nodiscard]] std::size_t memory_bytes() const noexcept;

    /// Pre-allocates for `count` vectors so a bulk load performs one allocation
    /// per array rather than a logarithmic number of reallocate-and-copy rounds.
    void reserve(std::size_t count);

    /// Drops everything, keeping the dimension and the normalize flag.
    void clear() noexcept;

    /// Appends a vector that has already been validated and, if applicable,
    /// normalized — i.e. one read back from our own file.
    ///
    /// Skips validation and normalization on purpose: those ran when the data
    /// first entered the system, and repeating them on load would put an
    /// O(N*D) pass on every open for no new information. `vectordb check` is
    /// the explicit command that does re-verify. See ADR-0007 and ADR-0008.
    LocalId append_trusted(VectorId id, VectorView vector, float norm, bool live);

private:
    void ensure_capacity_for_one() const;

    VectorArray vectors_;
    std::vector<float> norms_;
    std::vector<std::uint8_t> live_;
    std::vector<VectorId> local_to_id_;
    std::unordered_map<VectorId, LocalId> id_to_local_;
    std::size_t live_count_ = 0;
    bool normalize_;
};

}  // namespace vectordb
