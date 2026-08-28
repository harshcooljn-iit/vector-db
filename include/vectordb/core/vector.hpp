// SPDX-License-Identifier: MIT
//
// How a single vector is referred to, validated and normalized.
//
// A vector is never a type that owns its floats. It is a *view* — a pointer and
// a length — into storage owned by something else, almost always a large
// contiguous block in `VectorArray`. See
// learnings/30-vector-database/02-vector-representations.md for why.
#pragma once

#include <span>

#include <vectordb/core/types.hpp>

namespace vectordb {

/// A read-only view of one vector's components.
///
/// Non-owning: valid only while the underlying storage is alive and unresized.
/// Passing this by value is the normal thing to do — it is two words.
using VectorView = std::span<const float>;

/// A mutable view, used when writing into pre-allocated storage.
using MutableVectorView = std::span<float>;

/// Rejects a vector that this database cannot store.
///
/// Two independent checks, both throwing rather than returning a code, because
/// every caller's correct response is to abort the operation:
///
///   * dimension must match exactly — never truncated, never padded
///     (docs/decisions/ADR-0002-fixed-dimension.md)
///   * every component must be finite — no NaN, no +/-Inf
///     (docs/decisions/ADR-0007-nan-policy.md)
///
/// The NaN check is not pedantry. NaN compares false against everything, so a
/// single NaN turns distance ordering into a non-order: the top-k heap's
/// invariant breaks, HNSW's greedy descent can neither improve nor terminate
/// sensibly, and the corruption is silent. Rejecting at the door is the only
/// cheap place to catch it.
///
/// @throws DimensionMismatchError, InvalidVectorError
void validate_vector(VectorView vector, Dimension expected_dimension);

/// True when no component is NaN or infinite.
[[nodiscard]] bool is_finite(VectorView vector) noexcept;

/// Index of the first non-finite component, or `vector.size()` if all finite.
/// Used to build an error message that names *which* component was bad.
[[nodiscard]] std::size_t find_non_finite(VectorView vector) noexcept;

/// Euclidean (L2) length: `sqrt(sum(x_i^2))`.
[[nodiscard]] float l2_norm(VectorView vector) noexcept;

/// Sum of squares — the squared L2 norm. Avoids a `sqrt` where only ranking or
/// a comparison against another squared quantity is needed.
[[nodiscard]] float l2_norm_squared(VectorView vector) noexcept;

/// Scales `vector` to unit length in place and returns its *original* norm.
///
/// A zero vector has no direction, so there is no meaningful unit vector to
/// scale it to. Rather than produce NaN by dividing by zero, the vector is left
/// untouched and 0.0F is returned; callers that care must check.
float normalize_in_place(MutableVectorView vector) noexcept;

/// Copies `source` into `destination`, scaled to unit length.
/// Returns the original norm of `source`; see `normalize_in_place` for the
/// zero-vector rule.
///
/// @throws DimensionMismatchError if the spans differ in length
float normalize_into(VectorView source, MutableVectorView destination);

}  // namespace vectordb
