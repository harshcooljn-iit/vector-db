// SPDX-License-Identifier: MIT
#include <cmath>
#include <cstddef>
#include <string>

#include <vectordb/core/error.hpp>
#include <vectordb/core/vector.hpp>

namespace vectordb {

std::size_t find_non_finite(VectorView vector) noexcept {
    for (std::size_t i = 0; i < vector.size(); ++i) {
        if (!std::isfinite(vector[i])) {
            return i;
        }
    }
    return vector.size();
}

bool is_finite(VectorView vector) noexcept {
    return find_non_finite(vector) == vector.size();
}

void validate_vector(VectorView vector, Dimension expected_dimension) {
    if (vector.size() != static_cast<std::size_t>(expected_dimension)) {
        // Casting through uint64 first keeps the error honest if someone hands
        // us a span longer than 2^32 rather than silently wrapping it.
        const auto actual = vector.size() > static_cast<std::size_t>(kMaxDimension)
                                ? kMaxDimension
                                : static_cast<Dimension>(vector.size());
        throw DimensionMismatchError(expected_dimension, actual);
    }

    const std::size_t bad = find_non_finite(vector);
    if (bad != vector.size()) {
        const float value = vector[bad];
        std::string what = "vector rejected: component ";
        what += std::to_string(bad);
        what += " is ";
        if (std::isnan(value)) {
            what += "NaN";
        } else {
            what += value > 0.0F ? "+Inf" : "-Inf";
        }
        what +=
            "; VectorDB rejects non-finite components at insertion because they "
            "make distance comparisons non-transitive and silently corrupt every "
            "index that relies on ordering";
        throw InvalidVectorError(what);
    }
}

float l2_norm_squared(VectorView vector) noexcept {
    // Deliberately a plain scalar loop with a double accumulator.
    //
    // This runs once per vector at insert/normalize time, not once per
    // candidate in a search, so it is not a hot path. Accumulating in double
    // costs nothing here and avoids the drift a float accumulator shows over a
    // few thousand dimensions. The distance kernels in src/distance/ are the
    // hot path and make the opposite trade, for reasons documented there.
    double sum = 0.0;
    for (const float value : vector) {
        sum += static_cast<double>(value) * static_cast<double>(value);
    }
    return static_cast<float>(sum);
}

float l2_norm(VectorView vector) noexcept {
    return std::sqrt(l2_norm_squared(vector));
}

float normalize_in_place(MutableVectorView vector) noexcept {
    const float norm = l2_norm(vector);

    // A zero vector points nowhere. Dividing by zero would fill the vector with
    // NaN, which is precisely what validate_vector() exists to keep out — so we
    // must not manufacture it here.
    if (norm == 0.0F) {
        return 0.0F;
    }

    const float inverse = 1.0F / norm;
    for (float& value : vector) {
        value *= inverse;
    }
    return norm;
}

float normalize_into(VectorView source, MutableVectorView destination) {
    if (source.size() != destination.size()) {
        throw DimensionMismatchError(static_cast<Dimension>(source.size()),
                                     static_cast<Dimension>(destination.size()));
    }

    const float norm = l2_norm(source);
    if (norm == 0.0F) {
        for (std::size_t i = 0; i < source.size(); ++i) {
            destination[i] = source[i];
        }
        return 0.0F;
    }

    const float inverse = 1.0F / norm;
    for (std::size_t i = 0; i < source.size(); ++i) {
        destination[i] = source[i] * inverse;
    }
    return norm;
}

}  // namespace vectordb
