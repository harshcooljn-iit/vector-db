// SPDX-License-Identifier: MIT
#include <cmath>

#include <vectordb/core/error.hpp>
#include <vectordb/index/hnsw_config.hpp>

namespace vectordb {

double HnswConfig::level_multiplier() const noexcept {
    // 1 / ln(M). Each layer then holds about 1/M of the layer below it, so the
    // number of layers is ~log_M(N) and the descent through them is
    // logarithmic. This constant is what makes HNSW sub-linear.
    return 1.0 / std::log(static_cast<double>(m));
}

void HnswConfig::validate() const {
    if (m < 2) {
        throw InvalidArgumentError(
            "hnsw: M must be at least 2 (got " + std::to_string(m) +
            "); below about 5 the graph loses connectivity and recall collapses");
    }
    if (m > 512) {
        throw InvalidArgumentError("hnsw: M must be at most 512 (got " + std::to_string(m) +
                                   "); beyond about 64 the returns are small and the "
                                   "memory is not");
    }
    if (ef_construction < 1) {
        throw InvalidArgumentError("hnsw: efConstruction must be at least 1");
    }
    if (ef_search < 1) {
        throw InvalidArgumentError("hnsw: efSearch must be at least 1");
    }
    if (ef_construction < m) {
        throw InvalidArgumentError(
            "hnsw: efConstruction (" + std::to_string(ef_construction) +
            ") must be at least M (" + std::to_string(m) +
            "), or a node cannot see enough candidates to fill its neighbour list");
    }
}

std::string HnswConfig::describe() const {
    return "M=" + std::to_string(m) + ", efConstruction=" + std::to_string(ef_construction) +
           ", efSearch=" + std::to_string(ef_search) + ", seed=" + std::to_string(seed);
}

}  // namespace vectordb
