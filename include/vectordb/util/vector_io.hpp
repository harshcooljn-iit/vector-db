// SPDX-License-Identifier: MIT
//
// Portable vector interchange: one binary format for bulk data, one text format
// for examples and debugging.
//
// Specification: docs/data-formats.md
#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

#include <vectordb/core/types.hpp>
#include <vectordb/core/vector_array.hpp>

namespace vectordb {

/// Magic for the binary matrix format.
inline constexpr std::string_view kVectorMatrixMagic = "VDBDATA";
inline constexpr std::uint32_t kVectorMatrixVersion = 1;
inline constexpr std::size_t kVectorMatrixHeaderSize = 32;

/// Optional ids alongside the vectors.
struct VectorBatch {
    VectorArray vectors;

    /// Empty when the file carried no ids; otherwise one per vector.
    std::vector<VectorId> ids;

    [[nodiscard]] bool has_ids() const noexcept { return !ids.empty(); }
};

/// Writes a `.vecs` binary matrix.
///
/// Deliberately *not* the same format as `vectors.bin`. The database file
/// carries tombstones, cached norms and internal slot ordering — none of which
/// mean anything to another program. An interchange format should be the
/// smallest thing that round-trips the data, so this is a header plus a float
/// matrix plus optional ids.
///
/// @throws IoError
void write_vector_matrix(const std::filesystem::path& path,
                         const VectorArray& vectors,
                         const std::vector<VectorId>& ids = {});

/// Reads a `.vecs` binary matrix, validating the header the same way every
/// other format is validated.
/// @throws IoError, CorruptionError, UnsupportedVersionError
[[nodiscard]] VectorBatch read_vector_matrix(const std::filesystem::path& path);

/// Reads the header only, to report a file's shape without loading it.
struct VectorMatrixInfo {
    Dimension dimension = 0;
    std::uint64_t count = 0;
    bool has_ids = false;
};

[[nodiscard]] VectorMatrixInfo read_vector_matrix_info(const std::filesystem::path& path);

/// Writes CSV: one vector per line, comma-separated, optionally `id,` first.
///
/// For examples and eyeballing, never for bulk data — it is roughly 12x larger
/// than the binary form and needs parsing. `docs/data-formats.md` says so.
void write_vector_csv(const std::filesystem::path& path,
                      const VectorArray& vectors,
                      const std::vector<VectorId>& ids = {});

/// Reads CSV. Blank lines and `#` comments are skipped. If `with_ids`, the
/// first field of each line is the id.
/// @throws IoError, InvalidArgumentError with the offending line number
[[nodiscard]] VectorBatch read_vector_csv(const std::filesystem::path& path,
                                          bool with_ids = false);

/// Parses a comma- or space-separated vector from a command-line argument.
/// @throws InvalidArgumentError naming the offending component
[[nodiscard]] std::vector<float> parse_vector_literal(std::string_view text);

}  // namespace vectordb
