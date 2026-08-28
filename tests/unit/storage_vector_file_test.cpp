// SPDX-License-Identifier: MIT
//
// Round trips, and — more importantly — every way a file can be wrong.
#include <cstdio>
#include <fstream>
#include <vector>

#include <gtest/gtest.h>

#include <vectordb/core/error.hpp>
#include <vectordb/storage/vector_file.hpp>
#include <vectordb/util/dataset.hpp>

#include "support/temp_dir.hpp"

namespace vectordb {
namespace {

using testing::TempDir;

VectorStore make_store(Dimension dimension, std::size_t count, bool normalize = false) {
    VectorStore store(dimension, normalize);
    const VectorArray data =
        generate_dataset({.dimension = dimension, .count = count, .seed = 1234});
    for (LocalId id = 0; id < data.size(); ++id) {
        store.insert(static_cast<VectorId>(id) * 10 + 3, data[id]);
    }
    return store;
}

void corrupt_byte(const std::filesystem::path& path,
                  std::streamoff offset,
                  std::uint8_t value) {
    std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(file.is_open());
    file.seekp(offset);
    file.write(reinterpret_cast<const char*>(&value), 1);
    ASSERT_TRUE(file.good());
}

void truncate_to(const std::filesystem::path& path, std::uintmax_t size) {
    std::error_code error;
    std::filesystem::resize_file(path, size, error);
    ASSERT_FALSE(error) << error.message();
}

// ---------------------------------------------------------------------------
// Round trips
// ---------------------------------------------------------------------------

TEST(VectorFile, RoundTripsEveryVectorBitExactly) {
    const TempDir dir;
    const auto path = dir.file("vectors.bin");

    const VectorStore original = make_store(64, 500);
    write_vector_file(path, original);
    const VectorStore loaded = read_vector_file(path);

    ASSERT_EQ(loaded.slot_count(), original.slot_count());
    ASSERT_EQ(loaded.live_count(), original.live_count());
    EXPECT_EQ(loaded.dimension(), original.dimension());
    EXPECT_EQ(loaded.normalizes(), original.normalizes());

    for (LocalId slot = 0; slot < original.slot_count(); ++slot) {
        ASSERT_EQ(loaded.vector_id(slot), original.vector_id(slot)) << "slot " << slot;
        ASSERT_EQ(loaded.live(slot), original.live(slot)) << "slot " << slot;
        EXPECT_EQ(loaded.norm(slot), original.norm(slot)) << "slot " << slot;

        const VectorView a = original.vector(slot);
        const VectorView b = loaded.vector(slot);
        for (Dimension d = 0; d < original.dimension(); ++d) {
            // Bit-exact. Floats were copied, not computed on, so any difference
            // is a serialization bug rather than drift.
            ASSERT_EQ(a[d], b[d]) << "slot " << slot << " component " << d;
        }
    }
}

TEST(VectorFile, PreservesTombstonesSoDeletedVectorsStayDeleted) {
    const TempDir dir;
    const auto path = dir.file("vectors.bin");

    VectorStore original = make_store(8, 20);
    ASSERT_TRUE(original.remove(3));    // id 3 = slot 0
    ASSERT_TRUE(original.remove(103));  // id 103 = slot 10

    write_vector_file(path, original);
    const VectorStore loaded = read_vector_file(path);

    EXPECT_EQ(loaded.slot_count(), 20U) << "tombstoned slots keep their place";
    EXPECT_EQ(loaded.live_count(), 18U);
    EXPECT_FALSE(loaded.live(0));
    EXPECT_FALSE(loaded.live(10));
    EXPECT_FALSE(loaded.contains(3)) << "a reload must not resurrect a deleted vector";
    EXPECT_TRUE(loaded.contains(13));
    // And the surviving ids must keep their slots.
    EXPECT_EQ(loaded.find(13).value(), 1U);
}

TEST(VectorFile, RoundTripsAnEmptyStore) {
    const TempDir dir;
    const auto path = dir.file("vectors.bin");

    const VectorStore original(128);
    write_vector_file(path, original);
    const VectorStore loaded = read_vector_file(path);

    EXPECT_EQ(loaded.slot_count(), 0U);
    EXPECT_EQ(loaded.dimension(), 128U);
    EXPECT_EQ(std::filesystem::file_size(path), kVectorFileHeaderSize);
}

TEST(VectorFile, RoundTripsTheNormalizeFlag) {
    const TempDir dir;
    const auto path = dir.file("vectors.bin");

    const VectorStore original = make_store(16, 10, /*normalize=*/true);
    write_vector_file(path, original);
    const VectorStore loaded = read_vector_file(path);

    EXPECT_TRUE(loaded.normalizes());
    EXPECT_NEAR(loaded.norm(0), 1.0F, 1e-6F);
}

TEST(VectorFile, WritingTwiceReplacesTheFileCompletely) {
    const TempDir dir;
    const auto path = dir.file("vectors.bin");

    write_vector_file(path, make_store(4, 100));
    const auto large = std::filesystem::file_size(path);

    write_vector_file(path, make_store(4, 5));
    const auto small = std::filesystem::file_size(path);

    EXPECT_LT(small, large) << "the old contents must not be left behind";
    EXPECT_EQ(read_vector_file(path).slot_count(), 5U);
    EXPECT_FALSE(std::filesystem::exists(path.string() + ".tmp"))
        << "the temporary must be cleaned up by the rename";
}

TEST(VectorFile, HeaderCanBeReadWithoutLoadingThePayload) {
    const TempDir dir;
    const auto path = dir.file("vectors.bin");
    write_vector_file(path, make_store(32, 250));

    const VectorFileHeader header = read_vector_file_header(path);
    EXPECT_EQ(header.format_version, kVectorFileVersion);
    EXPECT_EQ(header.dimension, 32U);
    EXPECT_EQ(header.slot_count, 250U);
    EXPECT_EQ(header.live_count, 250U);
    EXPECT_EQ(header.payload_offset(), kVectorFileHeaderSize);
    EXPECT_EQ(header.total_size(), std::filesystem::file_size(path));
}

// The payload starting at a 64-byte boundary is what lets a load map the file
// and use the floats in place, with no copy and no shift.
TEST(VectorFile, PayloadStartsCacheLineAligned) {
    EXPECT_EQ(kVectorFileHeaderSize % 64, 0U);
    const VectorFileHeader header{};
    EXPECT_EQ(header.payload_offset() % 64, 0U);
}

TEST(VectorFile, VerifiesThePayloadChecksum) {
    const TempDir dir;
    const auto path = dir.file("vectors.bin");
    write_vector_file(path, make_store(8, 50));

    EXPECT_TRUE(verify_vector_file_payload(path));

    // Flip a bit inside the float payload. Structural validation cannot see
    // this — only the checksum can, which is exactly why `vectordb check`
    // exists as a separate command.
    corrupt_byte(path, static_cast<std::streamoff>(kVectorFileHeaderSize) + 17, 0xFF);
    EXPECT_FALSE(verify_vector_file_payload(path));
    // And the file still loads, because the damage is invisible structurally.
    EXPECT_NO_THROW(static_cast<void>(read_vector_file(path)));
}

// ---------------------------------------------------------------------------
// Corruption. Each of these must fail loudly, not read garbage.
// ---------------------------------------------------------------------------

TEST(VectorFileCorruption, RejectsAFileThatIsNotOurs) {
    const TempDir dir;
    const auto path = dir.file("not-a-vector-file.bin");
    {
        std::ofstream file(path, std::ios::binary);
        // Comfortably longer than a header, so this reaches the magic check
        // rather than tripping the too-small check first.
        file << "This is an ordinary text file. It is not a VectorDB vector file, "
                "and it is long enough that the reader gets as far as inspecting "
                "the magic number before giving up.";
    }

    EXPECT_THROW(static_cast<void>(read_vector_file(path)), CorruptionError);
    try {
        static_cast<void>(read_vector_file(path));
    } catch (const CorruptionError& error) {
        EXPECT_NE(std::string(error.what()).find("magic"), std::string::npos);
    }
}

TEST(VectorFileCorruption, RejectsAFileTooSmallToHoldAHeader) {
    const TempDir dir;
    const auto path = dir.file("tiny.bin");
    {
        std::ofstream file(path, std::ios::binary);
        file << "VDBVEC";
    }
    EXPECT_THROW(static_cast<void>(read_vector_file(path)), CorruptionError);
}

TEST(VectorFileCorruption, RejectsAnUnsupportedFormatVersion) {
    const TempDir dir;
    const auto path = dir.file("vectors.bin");
    write_vector_file(path, make_store(4, 10));

    // Version sits at offset 8. Bump it beyond what this build understands.
    corrupt_byte(path, 8, 99);

    EXPECT_THROW(static_cast<void>(read_vector_file(path)), UnsupportedVersionError);
    try {
        static_cast<void>(read_vector_file(path));
    } catch (const UnsupportedVersionError& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("99"), std::string::npos) << message;
        EXPECT_NE(message.find("newer"), std::string::npos) << message;
    }
}

// Version is checked before the header CRC, so that a file from a future
// version reports "too new" rather than the much less helpful "checksum
// mismatch" it would also technically fail.
TEST(VectorFileCorruption, ReportsVersionBeforeChecksum) {
    const TempDir dir;
    const auto path = dir.file("vectors.bin");
    write_vector_file(path, make_store(4, 10));
    corrupt_byte(path, 8, 42);

    EXPECT_THROW(static_cast<void>(read_vector_file(path)), UnsupportedVersionError);
}

TEST(VectorFileCorruption, RejectsADamagedHeader) {
    const TempDir dir;
    const auto path = dir.file("vectors.bin");
    write_vector_file(path, make_store(4, 10));

    // Dimension sits at offset 12. Changing it invalidates the header CRC.
    corrupt_byte(path, 12, 200);

    try {
        static_cast<void>(read_vector_file(path));
        FAIL() << "expected CorruptionError";
    } catch (const CorruptionError& error) {
        EXPECT_NE(std::string(error.what()).find("checksum"), std::string::npos)
            << error.what();
    }
}

TEST(VectorFileCorruption, RejectsATruncatedFile) {
    const TempDir dir;
    const auto path = dir.file("vectors.bin");
    write_vector_file(path, make_store(16, 100));

    const auto full = std::filesystem::file_size(path);
    truncate_to(path, full - 40);

    try {
        static_cast<void>(read_vector_file(path));
        FAIL() << "expected CorruptionError";
    } catch (const CorruptionError& error) {
        EXPECT_NE(std::string(error.what()).find("truncated"), std::string::npos)
            << error.what();
    }
}

TEST(VectorFileCorruption, RejectsAFileWithTrailingData) {
    const TempDir dir;
    const auto path = dir.file("vectors.bin");
    write_vector_file(path, make_store(4, 10));

    {
        std::ofstream file(path, std::ios::binary | std::ios::app);
        file << "extra bytes that the header does not account for";
    }

    try {
        static_cast<void>(read_vector_file(path));
        FAIL() << "expected CorruptionError";
    } catch (const CorruptionError& error) {
        EXPECT_NE(std::string(error.what()).find("trailing"), std::string::npos)
            << error.what();
    }
}

// The header CRC covers the counts, so a hand-forged header that claims a huge
// slot count must fail the checksum before anything is sized from it. This test
// forges the counts *and* leaves the CRC stale, which is the realistic
// corruption case.
TEST(VectorFileCorruption, RejectsAnAbsurdSlotCountWithoutAttemptingToAllocateIt) {
    const TempDir dir;
    const auto path = dir.file("vectors.bin");
    write_vector_file(path, make_store(4, 10));

    // slot_count sits at offset 16. Set the top byte, making it enormous.
    corrupt_byte(path, 23, 0xFF);

    EXPECT_THROW(static_cast<void>(read_vector_file(path)), CorruptionError);
}

TEST(VectorFileCorruption, RejectsAMissingFile) {
    const TempDir dir;
    EXPECT_THROW(static_cast<void>(read_vector_file(dir.file("nope.bin"))), IoError);
    EXPECT_THROW(static_cast<void>(read_vector_file_header(dir.file("nope.bin"))), IoError);
}

}  // namespace
}  // namespace vectordb
