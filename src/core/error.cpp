// SPDX-License-Identifier: MIT
#include <array>
#include <cstring>
#include <string>

#include <vectordb/core/error.hpp>

namespace vectordb {
namespace {

std::string describe_errno(int error_number) {
    // strerror_r has two incompatible signatures across platforms; the buffer
    // form used here is the POSIX one available on macOS and glibc with
    // _POSIX_C_SOURCE >= 200112L. On failure we fall back to the raw number,
    // which is still more useful than nothing.
    std::array<char, 256> buffer{};
#if defined(_WIN32)
    if (strerror_s(buffer.data(), buffer.size(), error_number) == 0) {
        return std::string(buffer.data());
    }
#else
    if (::strerror_r(error_number, buffer.data(), buffer.size()) == 0) {
        return std::string(buffer.data());
    }
#endif
    return "errno " + std::to_string(error_number);
}

}  // namespace

DimensionMismatchError::DimensionMismatchError(Dimension expected, Dimension actual)
    : InvalidArgumentError(
          "vector dimension mismatch: database dimension = " + std::to_string(expected) +
          ", input dimension = " + std::to_string(actual)),
      expected_(expected),
      actual_(actual) {}

DuplicateIdError::DuplicateIdError(VectorId id)
    : Error("vector id " + std::to_string(id) + " already exists (duplicate policy = reject)"),
      id_(id) {}

IoError IoError::from_errno(std::string_view operation,
                            const std::filesystem::path& path,
                            int error_number) {
    std::string message;
    message.append(operation);
    message += " failed for '";
    message += path.string();
    message += "': ";
    message += describe_errno(error_number);
    return IoError(message);
}

UnsupportedVersionError::UnsupportedVersionError(std::string_view artefact,
                                                 std::uint32_t found,
                                                 std::uint32_t supported)
    : CorruptionError(std::string(artefact) + ": unsupported format version " +
                      std::to_string(found) + " (this build understands up to " +
                      std::to_string(supported) +
                      "); the file was written by a newer VectorDB") {}

}  // namespace vectordb
