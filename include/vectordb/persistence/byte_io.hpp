// SPDX-License-Identifier: MIT
//
// Explicit, field-by-field serialization.
//
// Learning note: learnings/50-storage/02-binary-file-formats.md
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vectordb {

/// Every VectorDB format is little-endian, because every platform this project
/// targets is. Rather than write a byte-swapping path we cannot test, the build
/// fails loudly on a big-endian host and the format documentation says so.
static_assert(std::endian::native == std::endian::little,
              "VectorDB's on-disk formats are little-endian and no byte-swapping path "
              "has been written or tested. See docs/vector-storage.md.");

/// Appends fixed-width fields to a byte buffer, one at a time.
///
/// ## Why not just write the struct?
///
/// ```cpp
/// struct Header { uint32_t version; uint64_t count; };
/// write(fd, &header, sizeof(header));      // DON'T
/// ```
///
/// `sizeof(Header)` is 16, not 12: the compiler inserts 4 bytes of padding so
/// that `count` is 8-byte aligned. The padding contains whatever was on the
/// stack — so the file differs between runs, defeating any checksum, and
/// leaking stack bytes into a file you might share.
///
/// Worse, the layout is an *ABI* property. A different compiler, a different
/// architecture, or `#pragma pack` in a header included above yours changes it,
/// and a file written by one build is silently misread by another. The bug
/// surfaces as "field is 4294967296 instead of 1" long after the change.
///
/// Writing each field explicitly makes the layout a property of *this code*,
/// visible in the format documentation, and stable forever.
class ByteWriter {
public:
    explicit ByteWriter(std::vector<std::byte>& buffer) noexcept : buffer_(&buffer) {}

    void u8(std::uint8_t value);
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);

    /// IEEE-754 binary32, written through its bit pattern rather than by
    /// memcpy-ing a float, so the representation is explicit in the code.
    void f32(float value);

    void bytes(std::span<const std::byte> data);

    /// Writes exactly `size` bytes of `text`, truncating or zero-padding.
    /// Fixed-width because a length-prefixed string in a header means the
    /// header has no fixed size, and a header with no fixed size cannot be read
    /// before it has been validated.
    void fixed_string(std::string_view text, std::size_t size);

    /// Appends `count` zero bytes, for alignment or reserved fields.
    void pad(std::size_t count);

    [[nodiscard]] std::size_t size() const noexcept { return buffer_->size(); }

private:
    std::vector<std::byte>* buffer_;
};

/// Reads fixed-width fields, refusing to read past the end.
///
/// **Every read is bounds-checked.** This is not defensive style for its own
/// sake — it is the rule that stops a corrupt file from becoming a memory
/// safety bug. The classic failure:
///
/// ```cpp
/// uint64_t count = read_u64();
/// std::vector<Node> nodes(count);      // count came from the file
/// ```
///
/// A truncated or foreign file can make `count` 0xFFFFFFFFFFFFFFFF, and that
/// line attempts a 300-exabyte allocation. `require()` exists so a length is
/// always checked against the bytes that actually remain *before* it is used to
/// size anything. See docs/decisions/ADR-0008-validate-on-load.md.
class ByteReader {
public:
    /// @param data     the buffer to read from
    /// @param context  named in every error message, e.g. "hnsw index header"
    ByteReader(std::span<const std::byte> data, std::string_view context) noexcept
        : data_(data), context_(context) {}

    [[nodiscard]] std::uint8_t u8();
    [[nodiscard]] std::uint16_t u16();
    [[nodiscard]] std::uint32_t u32();
    [[nodiscard]] std::uint64_t u64();
    [[nodiscard]] float f32();

    /// A view into the underlying buffer — no copy. Valid while that buffer is.
    [[nodiscard]] std::span<const std::byte> bytes(std::size_t count);

    /// Reads `size` bytes and trims trailing NULs.
    [[nodiscard]] std::string fixed_string(std::size_t size);

    void skip(std::size_t count);

    /// Throws `CorruptionError` unless at least `count` bytes remain.
    /// @param what  what was being read, for the message
    void require(std::size_t count, std::string_view what) const;

    /// Throws unless exactly `count` bytes remain — used to catch a file that
    /// is longer than its header claims, which means the header is wrong.
    void expect_remaining(std::size_t count, std::string_view what) const;

    [[nodiscard]] std::size_t position() const noexcept { return position_; }

    [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - position_; }

    [[nodiscard]] bool exhausted() const noexcept { return remaining() == 0; }

private:
    std::span<const std::byte> data_;
    std::string_view context_;
    std::size_t position_ = 0;
};

/// CRC-32 (IEEE 802.3, the zlib/PNG polynomial).
///
/// Detects truncation, byte swaps and single-bit flips. It is a *checksum*, not
/// a cryptographic hash: it catches accidents, not tampering. That is the right
/// tool here, because the failure we are guarding against is a half-written
/// file or a flipped bit, not an adversary.
[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> data,
                                  std::uint32_t seed = 0) noexcept;

/// CRC-32 over a run of floats, without materialising a byte view.
[[nodiscard]] std::uint32_t crc32_floats(std::span<const float> values,
                                         std::uint32_t seed = 0) noexcept;

}  // namespace vectordb
