// SPDX-License-Identifier: MIT
//
// These tests assert on *message content*, not just on exception type.
// A message that omits the offending values is a defect: the whole point of
// the hierarchy is that a user can act on what they read.
#include <gtest/gtest.h>

#include <cerrno>
#include <string>

#include <vectordb/core/error.hpp>

namespace vectordb {
namespace {

TEST(DimensionMismatchError, MessageNamesBothDimensions) {
    const DimensionMismatchError error(768, 384);
    const std::string message = error.what();

    EXPECT_EQ(error.expected(), 768U);
    EXPECT_EQ(error.actual(), 384U);
    EXPECT_NE(message.find("768"), std::string::npos);
    EXPECT_NE(message.find("384"), std::string::npos);
}

TEST(DimensionMismatchError, IsCatchableAsInvalidArgumentAndAsError) {
    try {
        throw DimensionMismatchError(4, 5);
    } catch (const InvalidArgumentError& error) {
        SUCCEED() << error.what();
        return;
    } catch (...) {
        FAIL() << "must be catchable as InvalidArgumentError";
    }
    FAIL() << "expected an exception";
}

TEST(DuplicateIdError, MessageNamesTheId) {
    const DuplicateIdError error(4242);
    EXPECT_EQ(error.id(), 4242U);
    EXPECT_NE(std::string(error.what()).find("4242"), std::string::npos);
}

TEST(IoError, FromErrnoIncludesOperationPathAndReason) {
    const IoError error = IoError::from_errno("open", "/tmp/does-not-exist", ENOENT);
    const std::string message = error.what();

    EXPECT_NE(message.find("open"), std::string::npos);
    EXPECT_NE(message.find("/tmp/does-not-exist"), std::string::npos);
    // Whatever the platform calls ENOENT, it should not be an empty string.
    EXPECT_GT(message.size(), std::string("open failed for '/tmp/does-not-exist': ").size());
}

TEST(UnsupportedVersionError, IsACorruptionErrorAndNamesBothVersions) {
    const UnsupportedVersionError error("hnsw index", 9, 1);
    const std::string message = error.what();

    EXPECT_NE(message.find("hnsw index"), std::string::npos);
    EXPECT_NE(message.find('9'), std::string::npos);

    const CorruptionError* as_corruption = &error;
    EXPECT_NE(as_corruption, nullptr);
}

}  // namespace
}  // namespace vectordb
