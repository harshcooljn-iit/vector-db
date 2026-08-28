// SPDX-License-Identifier: MIT
//
// The on-disk format for `vectors.bin`.
//
// Full specification: docs/vector-storage.md
// Learning note:      learnings/50-storage/02-binary-file-formats.md
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

#include <vectordb/core/types.hpp>
#include <vectordb/storage/vector_store.hpp>

namespace vectordb {

/// Identifies the file. Eight bytes rather than four because the extra four
/// make an accidental match essentially impossible, and the text is readable in
/// a hex dump — which is worth a great deal at 2 a.m.
inline constexpr std::string_view kVectorFileMagic = "VDBVEC\0\0";

/// Bumped whenever the layout changes. A file with a newer version raises
/// `UnsupportedVersionError` naming both versions, rather than being read with
/// the wrong field offsets.
inline constexpr std::uint32_t kVectorFileVersion = 1;

/// Size of the fixed header, in bytes. Also the offset of the float payload —
/// a multiple of 64, so the payload starts cache-line and page aligned and can
/// be memory-mapped and used directly with no copy or shift.
inline constexpr std::size_t kVectorFileHeaderSize = 64;

/// Refuse to even attempt a header read on anything smaller.
inline constexpr std::uint64_t kMaxVectorFileBytes = 64ULL * 1024 * 1024 * 1024;

/// The header, after validation.
struct VectorFileHeader {
    std::uint32_t format_version = kVectorFileVersion;
    Dimension dimension = 0;

    /// Total slots, including tombstoned ones.
    std::uint64_t slot_count = 0;

    /// Live slots. Redundant — it can be recomputed from the liveness bytes —
    /// and that redundancy is the point: a mismatch is a cheap corruption
    /// signal that costs one comparison to check.
    std::uint64_t live_count = 0;

    /// True if the stored vectors were normalized on insert.
    bool normalized = false;

    /// CRC-32 over the float payload only. Verified by `vectordb check`, not on
    /// every open: a full pass over 3 GB is seconds, and paying it on every
    /// open to catch a class of corruption that changes one distance slightly
    /// is a bad trade. See ADR-0008.
    std::uint32_t payload_crc = 0;

    /// CRC-32 over the first 60 bytes of the header. Always verified, because
    /// it costs nothing and every other field's trustworthiness rests on it.
    std::uint32_t header_crc = 0;

    /// Byte offsets of each region, derived from the counts. Computed rather
    /// than stored: a stored offset is one more thing that can disagree with
    /// reality, and these are trivially derivable.
    [[nodiscard]] std::uint64_t payload_offset() const noexcept;
    [[nodiscard]] std::uint64_t ids_offset() const noexcept;
    [[nodiscard]] std::uint64_t norms_offset() const noexcept;
    [[nodiscard]] std::uint64_t live_offset() const noexcept;
    [[nodiscard]] std::uint64_t total_size() const noexcept;
};

/// Writes `store` to `path`, atomically.
///
/// The file is complete or absent — never half-written — because the bytes go
/// to a temporary, are fsynced, and are then renamed over the target. See
/// `write_file_atomically`.
///
/// @throws IoError
void write_vector_file(const std::filesystem::path& path, const VectorStore& store);

/// Parses and validates a header from at least `kVectorFileHeaderSize` bytes.
///
/// Checks, in order: size, magic, version, header CRC, then every field for
/// internal consistency. `file_size` is compared against the size the header
/// implies, which is what catches truncation.
///
/// @throws CorruptionError, UnsupportedVersionError
[[nodiscard]] VectorFileHeader parse_vector_file_header(std::span<const std::byte> bytes,
                                                        std::uint64_t file_size);

/// Reads just the header. Cheap enough to run before deciding whether to open
/// the rest — which is what `vectordb info` does.
[[nodiscard]] VectorFileHeader read_vector_file_header(const std::filesystem::path& path);

/// Loads a whole file into a fresh store.
///
/// Uses a memory mapping where available, so a large file is not read into a
/// buffer and then copied a second time into the store.
///
/// @throws IoError, CorruptionError, UnsupportedVersionError
[[nodiscard]] VectorStore read_vector_file(const std::filesystem::path& path);

/// Verifies the payload CRC. This is the full pass that `read_vector_file`
/// deliberately skips; `vectordb check` calls it.
///
/// @return true if the payload matches the CRC recorded in the header
[[nodiscard]] bool verify_vector_file_payload(const std::filesystem::path& path);

}  // namespace vectordb
