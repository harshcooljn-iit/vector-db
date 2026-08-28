// SPDX-License-Identifier: MIT
#include <cassert>
#include <string>

#include <vectordb/core/error.hpp>
#include <vectordb/core/vector_array.hpp>

namespace vectordb {

VectorArray::VectorArray(Dimension dimension) : dimension_(dimension) {
    if (dimension == 0) {
        throw InvalidArgumentError("vector dimension must be at least 1");
    }
    if (dimension > kMaxDimension) {
        throw InvalidArgumentError("vector dimension " + std::to_string(dimension) +
                                   " exceeds the supported maximum of " +
                                   std::to_string(kMaxDimension));
    }
}

LocalId VectorArray::push_back(VectorView vector) {
    if (vector.size() != static_cast<std::size_t>(dimension_)) {
        throw DimensionMismatchError(dimension_, static_cast<Dimension>(vector.size()));
    }
    if (count_ >= kMaxVectorCount) {
        throw InvalidArgumentError(
            "vector array is full: LocalId is 32-bit, so a single database holds at "
            "most " +
            std::to_string(kMaxVectorCount) + " vectors");
    }

    storage_.insert(storage_.end(), vector.begin(), vector.end());
    return static_cast<LocalId>(count_++);
}

LocalId VectorArray::append_uninitialized(std::size_t count) {
    if (count_ + count > kMaxVectorCount) {
        throw InvalidArgumentError("appending " + std::to_string(count) +
                                   " vectors would exceed the maximum of " +
                                   std::to_string(kMaxVectorCount));
    }

    const auto first = static_cast<LocalId>(count_);
    // resize() value-initialises the new floats to 0.0F. "Uninitialised" in the
    // name means "the caller is expected to overwrite them", not that the bytes
    // are indeterminate: leaving genuinely uninitialised floats around would
    // make a partially-filled array a source of UB and of non-reproducible
    // behaviour under a sanitizer.
    storage_.resize(storage_.size() + count * static_cast<std::size_t>(dimension_));
    count_ += count;
    return first;
}

void VectorArray::assign(LocalId id, VectorView vector) {
    if (static_cast<std::size_t>(id) >= count_) {
        throw InvalidArgumentError("vector slot " + std::to_string(id) +
                                   " is out of range (size = " + std::to_string(count_) + ")");
    }
    if (vector.size() != static_cast<std::size_t>(dimension_)) {
        throw DimensionMismatchError(dimension_, static_cast<Dimension>(vector.size()));
    }

    float* destination = storage_.data() + offset_of(id);
    for (std::size_t i = 0; i < vector.size(); ++i) {
        destination[i] = vector[i];
    }
}

VectorView VectorArray::operator[](LocalId id) const noexcept {
    assert(static_cast<std::size_t>(id) < count_ && "VectorArray index out of range");
    return VectorView(storage_.data() + offset_of(id), dimension_);
}

VectorView VectorArray::at(LocalId id) const {
    if (static_cast<std::size_t>(id) >= count_) {
        throw InvalidArgumentError("vector slot " + std::to_string(id) +
                                   " is out of range (size = " + std::to_string(count_) + ")");
    }
    return VectorView(storage_.data() + offset_of(id), dimension_);
}

MutableVectorView VectorArray::mutable_at(LocalId id) {
    if (static_cast<std::size_t>(id) >= count_) {
        throw InvalidArgumentError("vector slot " + std::to_string(id) +
                                   " is out of range (size = " + std::to_string(count_) + ")");
    }
    return MutableVectorView(storage_.data() + offset_of(id), dimension_);
}

std::size_t VectorArray::payload_bytes() const noexcept {
    return count_ * static_cast<std::size_t>(dimension_) * sizeof(float);
}

std::size_t VectorArray::allocated_bytes() const noexcept {
    return storage_.capacity() * sizeof(float);
}

void VectorArray::reserve(std::size_t count) {
    storage_.reserve(count * static_cast<std::size_t>(dimension_));
}

void VectorArray::clear() noexcept {
    storage_.clear();
    count_ = 0;
}

void VectorArray::shrink_to_fit() {
    storage_.shrink_to_fit();
}

}  // namespace vectordb
