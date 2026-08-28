// SPDX-License-Identifier: MIT
#include <cmath>

#include <vectordb/distance/distance.hpp>

namespace vectordb {
namespace {

/// Cosine distance from a dot product and two norms, with the zero-vector case
/// handled explicitly.
///
/// A zero vector has no direction, so `cos(0, b)` is 0/0. Returning NaN would
/// break the total order every ranking structure in the engine depends on, so
/// we define the similarity as 0 — "unrelated to everything" — which gives a
/// rank key of 1.0 and sorts a zero vector behind anything with real overlap
/// and ahead of anything actively opposed.
float cosine_rank_key(float dot, float norm_a, float norm_b) noexcept {
    const float denominator = norm_a * norm_b;
    if (denominator <= 0.0F) {
        return 1.0F;
    }

    const float similarity = dot / denominator;

    // Floating-point error can push a self-comparison to 1.0000001, and a
    // caller displaying "similarity 1.0000001" looks like a bug even though the
    // ranking is unaffected. Clamping also keeps the reported score inside its
    // documented range.
    if (similarity > 1.0F) {
        return 0.0F;
    }
    if (similarity < -1.0F) {
        return 2.0F;
    }
    return 1.0F - similarity;
}

}  // namespace

float score_from_rank_key(Metric metric, RankKey key) noexcept {
    switch (metric) {
        case Metric::kL2Squared:
            return key;
        case Metric::kCosine:
            return 1.0F - key;
        case Metric::kInnerProduct:
            return -key;
    }
    return key;
}

RankKey rank_key_from_score(Metric metric, float score) noexcept {
    switch (metric) {
        case Metric::kL2Squared:
            return score;
        case Metric::kCosine:
            return 1.0F - score;
        case Metric::kInnerProduct:
            return -score;
    }
    return score;
}

bool higher_score_is_better(Metric metric) noexcept {
    switch (metric) {
        case Metric::kL2Squared:
            return false;
        case Metric::kCosine:
        case Metric::kInnerProduct:
            return true;
    }
    return false;
}

float DistanceFunction::operator()(VectorView a, VectorView b) const noexcept {
    const std::size_t count = a.size();

    switch (metric_) {
        case Metric::kL2Squared:
            return kernel_->l2_squared(a.data(), b.data(), count);

        case Metric::kInnerProduct:
            // Negated so that smaller is better, in common with every other
            // metric. See the RankKey documentation.
            return -kernel_->dot(a.data(), b.data(), count);

        case Metric::kCosine: {
            const float dot = kernel_->dot(a.data(), b.data(), count);
            const float norm_a = std::sqrt(kernel_->norm_squared(a.data(), count));
            const float norm_b = std::sqrt(kernel_->norm_squared(b.data(), count));
            return cosine_rank_key(dot, norm_a, norm_b);
        }
    }
    return 0.0F;
}

float DistanceFunction::with_norms(VectorView a,
                                   float norm_a,
                                   VectorView b,
                                   float norm_b) const noexcept {
    switch (metric_) {
        case Metric::kL2Squared:
            return kernel_->l2_squared(a.data(), b.data(), a.size());

        case Metric::kInnerProduct:
            return -kernel_->dot(a.data(), b.data(), a.size());

        case Metric::kCosine:
            return cosine_rank_key(kernel_->dot(a.data(), b.data(), a.size()), norm_a, norm_b);
    }
    return 0.0F;
}

float compute_rank_key(Metric metric, VectorView a, VectorView b) noexcept {
    return DistanceFunction(metric)(a, b);
}

}  // namespace vectordb
