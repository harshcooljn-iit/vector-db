// SPDX-License-Identifier: MIT
//
// The on-disk format for an HNSW graph.
//
// Specification: docs/hnsw-format.md
// Learning note: learnings/50-storage/05-index-persistence.md
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

#include <vectordb/index/hnsw_index.hpp>

namespace vectordb {

inline constexpr std::string_view kHnswFileMagic = "VDBHNSW";
inline constexpr std::uint32_t kHnswFileVersion = 1;

/// Fixed header size, a multiple of 64 so the arrays that follow start
/// cache-line aligned.
inline constexpr std::size_t kHnswFileHeaderSize = 128;

/// A sanity ceiling. An index file larger than this is corrupt or is not ours,
/// and either way we should say so rather than try to map it.
inline constexpr std::uint64_t kMaxHnswFileBytes = 64ULL * 1024 * 1024 * 1024;

/// The header, after validation.
struct HnswFileHeader {
    std::uint32_t format_version = kHnswFileVersion;
    Dimension dimension = 0;
    Metric metric = Metric::kL2Squared;
    HnswConfig config;

    std::uint64_t node_count = 0;
    std::uint64_t live_count = 0;
    std::uint64_t layer0_link_count = 0;
    std::uint64_t upper_link_count = 0;

    LocalId entry_point = kInvalidLocalId;
    std::uint32_t max_level = 0;
    std::uint64_t rng_state = 0;

    std::uint32_t payload_crc = 0;
    std::uint32_t header_crc = 0;

    [[nodiscard]] std::uint64_t levels_offset() const noexcept;
    [[nodiscard]] std::uint64_t offsets_offset() const noexcept;
    [[nodiscard]] std::uint64_t layer0_offset() const noexcept;
    [[nodiscard]] std::uint64_t upper_offset() const noexcept;
    [[nodiscard]] std::uint64_t total_size() const noexcept;
};

/// Writes `index` to `path`, atomically.
/// @throws IoError
void write_hnsw_file(const std::filesystem::path& path,
                     const HnswIndex& index,
                     Dimension dimension);

/// Parses and validates a header.
/// @throws CorruptionError, UnsupportedVersionError
[[nodiscard]] HnswFileHeader parse_hnsw_file_header(std::span<const std::byte> bytes,
                                                    std::uint64_t file_size);

/// Reads the header only — enough for `vectordb info` to report the index's
/// parameters without loading the graph.
[[nodiscard]] HnswFileHeader read_hnsw_file_header(const std::filesystem::path& path);

/// Loads a graph and binds it to `accessor`.
///
/// **Every neighbour reference is checked against the node count before the
/// graph is used.** That pass is `O(edges)` and it buys two things: the search
/// loop never bounds-checks (so a corrupt file cannot become an out-of-bounds
/// read at query time), and a damaged index is reported as damaged rather than
/// silently returning wrong answers forever.
///
/// The caller must also confirm that `expected_*` match the database's
/// configuration; an index built for a different metric or dimension is not
/// merely stale, it is meaningless.
///
/// @throws CorruptionError, UnsupportedVersionError, IoError
[[nodiscard]] std::unique_ptr<HnswIndex> read_hnsw_file(
    const std::filesystem::path& path,
    VectorAccessor accessor,
    Metric expected_metric,
    Dimension expected_dimension,
    const DistanceKernel& kernel = active_kernel());

/// Verifies the payload checksum. Called by `vectordb check`, not on every open.
[[nodiscard]] bool verify_hnsw_file_payload(const std::filesystem::path& path);

}  // namespace vectordb
