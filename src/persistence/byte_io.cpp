// SPDX-License-Identifier: MIT
#include <array>

#include <vectordb/core/error.hpp>
#include <vectordb/persistence/byte_io.hpp>

namespace vectordb {
namespace {

/// The CRC-32 table, built at compile time so there is no initialisation order
/// question and no runtime cost.
constexpr std::array<std::uint32_t, 256> make_crc_table() noexcept {
    constexpr std::uint32_t kPolynomial = 0xEDB88320U;  // reversed 0x04C11DB7
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t value = i;
        for (int bit = 0; bit < 8; ++bit) {
            value = (value & 1U) != 0U ? (value >> 1) ^ kPolynomial : value >> 1;
        }
        table[i] = value;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> kCrcTable = make_crc_table();

}  // namespace

// ---------------------------------------------------------------------------
// ByteWriter
// ---------------------------------------------------------------------------

void ByteWriter::u8(std::uint8_t value) {
    buffer_->push_back(static_cast<std::byte>(value));
}

void ByteWriter::u16(std::uint16_t value) {
    u8(static_cast<std::uint8_t>(value & 0xFFU));
    u8(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
}

void ByteWriter::u32(std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        u8(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

void ByteWriter::u64(std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        u8(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

void ByteWriter::f32(float value) {
    // std::bit_cast rather than a reinterpret_cast or a union: it is the only
    // form that is well-defined rather than merely working in practice.
    u32(std::bit_cast<std::uint32_t>(value));
}

void ByteWriter::bytes(std::span<const std::byte> data) {
    buffer_->insert(buffer_->end(), data.begin(), data.end());
}

void ByteWriter::fixed_string(std::string_view text, std::size_t size) {
    const std::size_t copied = std::min(text.size(), size);
    for (std::size_t i = 0; i < copied; ++i) {
        u8(static_cast<std::uint8_t>(text[i]));
    }
    pad(size - copied);
}

void ByteWriter::pad(std::size_t count) {
    buffer_->insert(buffer_->end(), count, std::byte{0});
}

// ---------------------------------------------------------------------------
// ByteReader
// ---------------------------------------------------------------------------

void ByteReader::require(std::size_t count, std::string_view what) const {
    if (remaining() < count) {
        throw CorruptionError(std::string(context_) + ": truncated while reading " +
                              std::string(what) + " at offset " + std::to_string(position_) +
                              " — needed " + std::to_string(count) + " bytes but only " +
                              std::to_string(remaining()) + " remain");
    }
}

void ByteReader::expect_remaining(std::size_t count, std::string_view what) const {
    if (remaining() != count) {
        throw CorruptionError(std::string(context_) + ": " + std::string(what) +
                              " — expected exactly " + std::to_string(count) +
                              " bytes after offset " + std::to_string(position_) +
                              " but found " + std::to_string(remaining()));
    }
}

std::uint8_t ByteReader::u8() {
    require(1, "u8");
    return static_cast<std::uint8_t>(data_[position_++]);
}

std::uint16_t ByteReader::u16() {
    require(2, "u16");
    const auto low = static_cast<std::uint16_t>(u8());
    const auto high = static_cast<std::uint16_t>(u8());
    return static_cast<std::uint16_t>(low | (high << 8));
}

std::uint32_t ByteReader::u32() {
    require(4, "u32");
    std::uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(u8()) << shift;
    }
    return value;
}

std::uint64_t ByteReader::u64() {
    require(8, "u64");
    std::uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(u8()) << shift;
    }
    return value;
}

float ByteReader::f32() {
    return std::bit_cast<float>(u32());
}

std::span<const std::byte> ByteReader::bytes(std::size_t count) {
    require(count, "byte block");
    const std::span<const std::byte> view = data_.subspan(position_, count);
    position_ += count;
    return view;
}

std::string ByteReader::fixed_string(std::size_t size) {
    const std::span<const std::byte> view = bytes(size);
    std::string text;
    text.reserve(size);
    for (const std::byte value : view) {
        if (value == std::byte{0}) {
            break;
        }
        text.push_back(static_cast<char>(value));
    }
    return text;
}

void ByteReader::skip(std::size_t count) {
    require(count, "skipped region");
    position_ += count;
}

// ---------------------------------------------------------------------------
// CRC-32
// ---------------------------------------------------------------------------

std::uint32_t crc32(std::span<const std::byte> data, std::uint32_t seed) noexcept {
    std::uint32_t crc = ~seed;
    for (const std::byte value : data) {
        crc = kCrcTable[(crc ^ static_cast<std::uint8_t>(value)) & 0xFFU] ^ (crc >> 8);
    }
    return ~crc;
}

std::uint32_t crc32_floats(std::span<const float> values, std::uint32_t seed) noexcept {
    // Safe because the format is little-endian-only (static_assert in the
    // header) and float is IEEE-754 binary32 on every supported target, so the
    // in-memory bytes are exactly the on-disk bytes.
    const std::span<const std::byte> view{reinterpret_cast<const std::byte*>(values.data()),
                                          values.size() * sizeof(float)};
    return crc32(view, seed);
}

}  // namespace vectordb
