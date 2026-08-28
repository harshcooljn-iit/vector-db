// SPDX-License-Identifier: MIT
#include <string>

#include <vectordb/core/error.hpp>
#include <vectordb/storage/vector_store.hpp>

namespace vectordb {

VectorStore::VectorStore(Dimension dimension, bool normalize)
    : vectors_(dimension), normalize_(normalize) {}

void VectorStore::ensure_capacity_for_one() const {
    if (vectors_.size() >= kMaxVectorCount) {
        throw InvalidArgumentError(
            "database is full: LocalId is 32-bit, so a store holds at most " +
            std::to_string(kMaxVectorCount) + " slots (including tombstones)");
    }
}

LocalId VectorStore::insert(VectorId id, VectorView vector) {
    if (id_to_local_.find(id) != id_to_local_.end()) {
        throw DuplicateIdError(id);
    }
    ensure_capacity_for_one();

    // Validate before mutating anything. A rejected insert must leave the store
    // exactly as it was, or a caller that catches the exception and retries is
    // operating on a store that half-accepted the vector.
    validate_vector(vector, vectors_.dimension());

    const LocalId local_id = vectors_.push_back(vector);

    float norm = 0.0F;
    if (normalize_) {
        norm = normalize_in_place(vectors_.mutable_at(local_id));
        // The stored vector is now unit length, so its cached norm is 1 — which
        // is what collapses cosine into a plain dot product. A zero vector
        // cannot be normalized and keeps a norm of 0; the cosine code defines
        // its similarity rather than dividing by it (ADR-0007).
        norms_.push_back(norm == 0.0F ? 0.0F : 1.0F);
    } else {
        norm = l2_norm(vectors_[local_id]);
        norms_.push_back(norm);
    }

    live_.push_back(1U);
    local_to_id_.push_back(id);
    id_to_local_.emplace(id, local_id);
    ++live_count_;

    return local_id;
}

LocalId VectorStore::upsert(VectorId id, VectorView vector) {
    const auto existing = id_to_local_.find(id);
    if (existing == id_to_local_.end()) {
        return insert(id, vector);
    }

    validate_vector(vector, vectors_.dimension());

    const LocalId local_id = existing->second;
    vectors_.assign(local_id, vector);

    if (normalize_) {
        const float norm = normalize_in_place(vectors_.mutable_at(local_id));
        norms_[local_id] = norm == 0.0F ? 0.0F : 1.0F;
    } else {
        norms_[local_id] = l2_norm(vectors_[local_id]);
    }

    return local_id;
}

bool VectorStore::remove(VectorId id) {
    const auto it = id_to_local_.find(id);
    if (it == id_to_local_.end()) {
        return false;
    }

    // Tombstone: clear the liveness byte and forget the id. The floats stay put.
    // Physically removing slot 5 of a million would mean moving 999,995 vectors
    // and renumbering every graph edge that referred to them.
    live_[it->second] = 0U;
    id_to_local_.erase(it);
    --live_count_;
    return true;
}

std::optional<LocalId> VectorStore::find(VectorId id) const {
    const auto it = id_to_local_.find(id);
    if (it == id_to_local_.end()) {
        return std::nullopt;
    }
    return it->second;
}

VectorId VectorStore::vector_id(LocalId local_id) const {
    if (static_cast<std::size_t>(local_id) >= local_to_id_.size()) {
        throw InvalidArgumentError(
            "slot " + std::to_string(local_id) +
            " is out of range (slot count = " + std::to_string(local_to_id_.size()) + ")");
    }
    return local_to_id_[local_id];
}

VectorView VectorStore::get(VectorId id) const {
    const std::optional<LocalId> local_id = find(id);
    if (!local_id.has_value()) {
        throw NotFoundError("no vector with id " + std::to_string(id));
    }
    return vectors_[*local_id];
}

double VectorStore::tombstone_ratio() const noexcept {
    if (vectors_.empty()) {
        return 0.0;
    }
    return static_cast<double>(tombstone_count()) / static_cast<double>(slot_count());
}

std::size_t VectorStore::memory_bytes() const noexcept {
    // The unordered_map's real footprint is allocator-dependent; this is a
    // deliberate lower-bound estimate of buckets plus nodes, and `vectordb
    // stats` labels it as approximate rather than implying precision it does
    // not have.
    const std::size_t map_bytes =
        id_to_local_.bucket_count() * sizeof(void*) +
        id_to_local_.size() * (sizeof(VectorId) + sizeof(LocalId) + sizeof(void*));

    return vectors_.allocated_bytes() + norms_.capacity() * sizeof(float) +
           live_.capacity() * sizeof(std::uint8_t) +
           local_to_id_.capacity() * sizeof(VectorId) + map_bytes;
}

void VectorStore::reserve(std::size_t count) {
    vectors_.reserve(count);
    norms_.reserve(count);
    live_.reserve(count);
    local_to_id_.reserve(count);
    id_to_local_.reserve(count);
}

void VectorStore::clear() noexcept {
    vectors_.clear();
    norms_.clear();
    live_.clear();
    local_to_id_.clear();
    id_to_local_.clear();
    live_count_ = 0;
}

LocalId VectorStore::append_trusted(VectorId id, VectorView vector, float norm, bool live) {
    ensure_capacity_for_one();

    // Dimension is still checked — it is structural, cheap, and a mismatch here
    // means the file header lied, which is corruption we must not paper over.
    // Component-wise finiteness is not rechecked; see the header comment.
    const LocalId local_id = vectors_.push_back(vector);
    norms_.push_back(norm);
    live_.push_back(live ? 1U : 0U);
    local_to_id_.push_back(id);

    if (live) {
        id_to_local_.emplace(id, local_id);
        ++live_count_;
    }
    return local_id;
}

}  // namespace vectordb
