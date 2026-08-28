// SPDX-License-Identifier: MIT
//
// Contiguous storage for many equal-length vectors.
//
// Learning note: learnings/10-cpp-systems-foundations/03-memory-layout.md
#pragma once

#include <cstddef>
#include <vector>

#include <vectordb/core/types.hpp>
#include <vectordb/core/vector.hpp>

namespace vectordb {

/// A growable array of `dimension`-length float vectors stored back to back in
/// one allocation.
///
/// ## Why not `std::vector<std::vector<float>>`?
///
/// That is the obvious design and it is wrong at scale, for three separate
/// reasons. For 1,000,000 vectors of dimension 768:
///
/// 1. **Allocation count.** One million heap allocations instead of one. Each
///    costs an allocator round trip on insert and a free on destruction.
///
/// 2. **Overhead.** Every inner `std::vector` carries 24 bytes of pointers plus
///    a malloc header of ~16 bytes: ~40 MB of pure bookkeeping on top of 3 GB
///    of payload. That is bad but survivable.
///
/// 3. **Locality — the one that actually hurts.** The inner buffers are
///    scattered across the heap in allocation order, which is not scan order.
///    A brute-force scan then jumps randomly through memory, and the hardware
///    prefetcher — which is very good at "keep reading forward" and useless at
///    "guess this pointer" — cannot help. With one contiguous block the scan is
///    a straight walk and the prefetcher stays several cache lines ahead the
///    whole way. This is a large constant factor on the single hottest loop in
///    the system.
///
/// So: one `std::vector<float>` of `count * dimension` elements, and vector `i`
/// occupies `[i * dimension, (i + 1) * dimension)`.
///
/// ## Stride
///
/// The row stride equals the dimension exactly — no padding to a SIMD or cache
/// boundary. Padding would let SIMD kernels drop their tail handling and would
/// give every row the same alignment, but it wastes up to (width - 1) floats
/// per vector and, more importantly, it would make the in-memory layout differ
/// from the on-disk layout. Keeping them identical is what will let the vector
/// file be memory-mapped and used directly, with no transformation pass.
/// The kernels handle their own tails. See
/// docs/decisions/ADR-0004-contiguous-storage.md.
///
/// ## Reference invalidation
///
/// `VectorView`s returned by this class are invalidated by anything that can
/// reallocate the buffer (`push_back`, `reserve`, `resize`). This is exactly
/// `std::vector`'s contract and the same discipline applies: do not hold a view
/// across an insertion.
///
/// ## Thread safety
///
/// None beyond the usual: concurrent reads are safe, any write requires
/// exclusive access. Higher layers provide the locking.
class VectorArray {
public:
    /// @throws InvalidArgumentError if `dimension` is 0 or above `kMaxDimension`
    explicit VectorArray(Dimension dimension);

    /// Appends a copy of `vector` and returns its slot.
    ///
    /// Does *not* validate finiteness — that belongs at the database boundary,
    /// where a rejection can still be reported to the user, not here where it
    /// would run again on every internal copy and rebuild. Callers holding
    /// unvalidated input must call `validate_vector` first.
    ///
    /// @throws DimensionMismatchError if `vector.size() != dimension()`
    /// @throws InvalidArgumentError   if the array is already at `kMaxVectorCount`
    LocalId push_back(VectorView vector);

    /// Appends `count` uninitialised slots and returns the first new id.
    /// Used when the contents are about to be filled from a file or a decoder;
    /// avoids writing every float twice.
    LocalId append_uninitialized(std::size_t count);

    /// Overwrites the vector at `id`.
    /// @throws InvalidArgumentError if `id` is out of range
    /// @throws DimensionMismatchError on a length mismatch
    void assign(LocalId id, VectorView vector);

    /// Read-only access. Bounds-checked in debug builds only — the hot search
    /// loop calls this once per candidate, so a branch here is a branch in the
    /// innermost loop of the system.
    [[nodiscard]] VectorView operator[](LocalId id) const noexcept;

    /// Bounds-checked access for cold paths and for anything driven by data
    /// read off disk.
    /// @throws InvalidArgumentError if `id` is out of range
    [[nodiscard]] VectorView at(LocalId id) const;

    /// Mutable access, for filling a slot returned by `append_uninitialized`.
    [[nodiscard]] MutableVectorView mutable_at(LocalId id);

    /// Pointer to the first float of the whole block. The vector at slot `i`
    /// starts at `data() + i * dimension()`. Handed to file writers so an
    /// entire array can go out in one `write()`.
    [[nodiscard]] const float* data() const noexcept { return storage_.data(); }

    [[nodiscard]] std::size_t size() const noexcept { return count_; }

    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }

    [[nodiscard]] Dimension dimension() const noexcept { return dimension_; }

    /// Number of floats between the start of consecutive vectors.
    /// Equal to `dimension()` today; exposed so that callers are written
    /// against the concept rather than the current coincidence.
    [[nodiscard]] Dimension stride() const noexcept { return dimension_; }

    /// Bytes of payload actually in use: `size() * dimension() * 4`.
    [[nodiscard]] std::size_t payload_bytes() const noexcept;

    /// Bytes actually allocated, including capacity not yet used. Reported by
    /// `vectordb stats`, where the gap between this and `payload_bytes()` is
    /// the thing worth looking at.
    [[nodiscard]] std::size_t allocated_bytes() const noexcept;

    /// Reserves room for `count` vectors, so a bulk load performs one
    /// allocation instead of log2(n) reallocation-and-copy rounds.
    void reserve(std::size_t count);

    /// Drops every vector but keeps the allocation.
    void clear() noexcept;

    /// Drops every vector and releases the allocation.
    void shrink_to_fit();

private:
    [[nodiscard]] std::size_t offset_of(LocalId id) const noexcept {
        return static_cast<std::size_t>(id) * static_cast<std::size_t>(dimension_);
    }

    Dimension dimension_;
    std::size_t count_ = 0;
    std::vector<float> storage_;
};

}  // namespace vectordb
