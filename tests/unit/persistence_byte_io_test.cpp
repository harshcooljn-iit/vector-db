// SPDX-License-Identifier: MIT
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <vectordb/core/error.hpp>
#include <vectordb/persistence/byte_io.hpp>

namespace vectordb {
namespace {

std::span<const std::byte> view(const std::vector<std::byte>& buffer) {
    return {buffer.data(), buffer.size()};
}

TEST(ByteWriter, WritesLittleEndianWithNoPadding) {
    std::vector<std::byte> buffer;
    ByteWriter writer(buffer);
    writer.u32(0x11223344U);
    writer.u64(0x8899AABBCCDDEEFFULL);

    // 12 bytes, not 16: writing the equivalent struct would insert 4 bytes of
    // alignment padding between the fields.
    ASSERT_EQ(buffer.size(), 12U);
    EXPECT_EQ(static_cast<std::uint8_t>(buffer[0]), 0x44U) << "least significant first";
    EXPECT_EQ(static_cast<std::uint8_t>(buffer[3]), 0x11U);
    EXPECT_EQ(static_cast<std::uint8_t>(buffer[4]), 0xFFU);
    EXPECT_EQ(static_cast<std::uint8_t>(buffer[11]), 0x88U);
}

TEST(ByteIo, RoundTripsEveryFixedWidthType) {
    std::vector<std::byte> buffer;
    ByteWriter writer(buffer);
    writer.u8(0xABU);
    writer.u16(0xBEEFU);
    writer.u32(0xDEADBEEFU);
    writer.u64(0x0123456789ABCDEFULL);
    writer.f32(3.14159F);
    writer.f32(-0.0F);
    writer.fixed_string("vectordb", 16);

    ByteReader reader(view(buffer), "test");
    EXPECT_EQ(reader.u8(), 0xABU);
    EXPECT_EQ(reader.u16(), 0xBEEFU);
    EXPECT_EQ(reader.u32(), 0xDEADBEEFU);
    EXPECT_EQ(reader.u64(), 0x0123456789ABCDEFULL);
    EXPECT_FLOAT_EQ(reader.f32(), 3.14159F);
    EXPECT_TRUE(std::signbit(reader.f32())) << "negative zero must survive the round trip";
    EXPECT_EQ(reader.fixed_string(16), "vectordb");
    EXPECT_TRUE(reader.exhausted());
}

TEST(ByteIo, PreservesSpecialFloatBitPatterns) {
    std::vector<std::byte> buffer;
    ByteWriter writer(buffer);
    const float values[] = {std::numeric_limits<float>::min(),
                            std::numeric_limits<float>::denorm_min(),
                            std::numeric_limits<float>::max(),
                            1.0F / 3.0F};
    for (const float value : values) {
        writer.f32(value);
    }

    ByteReader reader(view(buffer), "test");
    for (const float value : values) {
        // Bit-exact, not approximate: serialization must not perturb a value.
        EXPECT_EQ(std::bit_cast<std::uint32_t>(reader.f32()),
                  std::bit_cast<std::uint32_t>(value));
    }
}

TEST(ByteWriter, TruncatesAndPadsFixedStrings) {
    std::vector<std::byte> buffer;
    ByteWriter writer(buffer);
    writer.fixed_string("a-very-long-name", 4);
    writer.fixed_string("ab", 4);

    ASSERT_EQ(buffer.size(), 8U);
    ByteReader reader(view(buffer), "test");
    EXPECT_EQ(reader.fixed_string(4), "a-ve");
    EXPECT_EQ(reader.fixed_string(4), "ab");
}

// The rule that stops a corrupt file becoming a memory-safety bug.
TEST(ByteReader, RefusesToReadPastTheEnd) {
    std::vector<std::byte> buffer;
    ByteWriter writer(buffer);
    writer.u32(1);

    ByteReader reader(view(buffer), "truncated artefact");
    EXPECT_EQ(reader.u32(), 1U);
    EXPECT_THROW(static_cast<void>(reader.u8()), CorruptionError);
}

TEST(ByteReader, ErrorMessageNamesTheContextOffsetAndShortfall) {
    std::vector<std::byte> buffer(2);
    ByteReader reader(view(buffer), "hnsw index header");
    try {
        static_cast<void>(reader.u64());
        FAIL() << "expected CorruptionError";
    } catch (const CorruptionError& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("hnsw index header"), std::string::npos) << message;
        EXPECT_NE(message.find("offset 0"), std::string::npos) << message;
        EXPECT_NE(message.find('8'), std::string::npos) << message;
        EXPECT_NE(message.find('2'), std::string::npos) << message;
    }
}

TEST(ByteReader, RequireChecksBeforeAnyAllocationWouldHappen) {
    std::vector<std::byte> buffer(16);
    ByteReader reader(view(buffer), "test");

    // This is the pattern real deserialization uses: read a length, check it
    // against what is actually there, and only then size anything.
    EXPECT_NO_THROW(reader.require(16, "payload"));
    EXPECT_THROW(reader.require(17, "payload"), CorruptionError);
    EXPECT_THROW(reader.require(0xFFFFFFFFFFFFFFFFULL, "payload"), CorruptionError);
}

TEST(ByteReader, ExpectRemainingCatchesTrailingData) {
    std::vector<std::byte> buffer(24);
    ByteReader reader(view(buffer), "test");
    reader.skip(8);

    EXPECT_NO_THROW(reader.expect_remaining(16, "body"));
    EXPECT_THROW(reader.expect_remaining(8, "body"), CorruptionError);
    EXPECT_THROW(reader.expect_remaining(32, "body"), CorruptionError);
}

TEST(ByteReader, TracksPositionAndRemaining) {
    std::vector<std::byte> buffer(10);
    ByteReader reader(view(buffer), "test");
    EXPECT_EQ(reader.position(), 0U);
    EXPECT_EQ(reader.remaining(), 10U);

    reader.skip(4);
    EXPECT_EQ(reader.position(), 4U);
    EXPECT_EQ(reader.remaining(), 6U);
    EXPECT_FALSE(reader.exhausted());

    static_cast<void>(reader.bytes(6));
    EXPECT_TRUE(reader.exhausted());
    EXPECT_THROW(reader.skip(1), CorruptionError);
}

TEST(ByteReader, BytesReturnsAViewNotACopy) {
    std::vector<std::byte> buffer(8, std::byte{7});
    ByteReader reader(view(buffer), "test");
    const std::span<const std::byte> slice = reader.bytes(4);
    EXPECT_EQ(slice.data(), buffer.data()) << "no copy for a bulk region";
    EXPECT_EQ(slice.size(), 4U);
}

// ---------------------------------------------------------------------------
// CRC-32
// ---------------------------------------------------------------------------

TEST(Crc32, MatchesTheKnownIeeeCheckValue) {
    // The standard check value for the string "123456789" under CRC-32/ISO-HDLC.
    const std::string_view text = "123456789";
    const std::span<const std::byte> bytes{reinterpret_cast<const std::byte*>(text.data()),
                                           text.size()};
    EXPECT_EQ(crc32(bytes), 0xCBF43926U);
}

TEST(Crc32, OfNothingIsZero) {
    EXPECT_EQ(crc32({}), 0U);
}

TEST(Crc32, DetectsASingleFlippedBit) {
    std::vector<std::byte> data(1024, std::byte{0xA5});
    const std::uint32_t original = crc32(view(data));

    data[512] = static_cast<std::byte>(0xA5U ^ 0x01U);
    EXPECT_NE(crc32(view(data)), original);
}

TEST(Crc32, DetectsTruncation) {
    std::vector<std::byte> data(100, std::byte{1});
    const std::uint32_t full = crc32(view(data));
    data.resize(99);
    EXPECT_NE(crc32(view(data)), full);
}

TEST(Crc32, DetectsTransposedBytes) {
    std::vector<std::byte> data{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    const std::uint32_t original = crc32(view(data));
    std::swap(data[1], data[2]);
    EXPECT_NE(crc32(view(data)), original);
}

TEST(Crc32Floats, AgreesWithTheByteFormOverTheSameData) {
    const std::vector<float> values{1.0F, -2.5F, 3.75F, 0.0F};
    const std::span<const std::byte> as_bytes{reinterpret_cast<const std::byte*>(values.data()),
                                              values.size() * sizeof(float)};
    EXPECT_EQ(crc32_floats(values), crc32(as_bytes));
}

}  // namespace
}  // namespace vectordb
