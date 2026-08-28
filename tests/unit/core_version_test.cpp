// SPDX-License-Identifier: MIT
#include <string>

#include <gtest/gtest.h>

#include <vectordb/core/version.hpp>

namespace vectordb {
namespace {

TEST(Version, ReportsTheSemanticVersion) {
    EXPECT_EQ(version_string(), "0.1.0");
}

// build_info() is embedded in every benchmark report so a measurement can be
// traced back to the build that produced it. If it stops naming the build
// type, a Debug number could be reported as if it were a Release number.
TEST(Version, BuildInfoNamesProjectCompilerAndBuildType) {
    const std::string info = build_info();

    EXPECT_NE(info.find("VectorDB"), std::string::npos);
    EXPECT_NE(info.find("compiler"), std::string::npos);
    EXPECT_NE(info.find("build type"), std::string::npos);
#if defined(NDEBUG)
    EXPECT_NE(info.find("Release"), std::string::npos);
#else
    EXPECT_NE(info.find("Debug"), std::string::npos);
#endif
}

}  // namespace
}  // namespace vectordb
