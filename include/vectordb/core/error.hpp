// SPDX-License-Identifier: MIT
//
// The exception hierarchy used across VectorDB.
//
// Policy: engine code throws; the CLI is the only layer that catches broadly
// and turns an exception into an exit code plus a human-readable message.
// Every exception message must name the concrete values involved — "Error."
// is a bug, "dimension mismatch: database=768 input=384" is the bar.
#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

#include <vectordb/core/types.hpp>

namespace vectordb {

/// Base class for every error VectorDB raises deliberately.
class Error : public std::runtime_error {
public:
    explicit Error(const std::string& what) : std::runtime_error(what) {}
};

/// The caller asked for something the API forbids: a bad argument, an
/// out-of-range parameter, a malformed filter expression.
class InvalidArgumentError : public Error {
public:
    using Error::Error;
};

/// A vector whose dimension does not match the database's fixed dimension.
/// VectorDB never truncates or pads — see docs/decisions/ADR-0002-fixed-dimension.md.
class DimensionMismatchError : public InvalidArgumentError {
public:
    DimensionMismatchError(Dimension expected, Dimension actual);

    [[nodiscard]] Dimension expected() const noexcept { return expected_; }
    [[nodiscard]] Dimension actual() const noexcept { return actual_; }

private:
    Dimension expected_;
    Dimension actual_;
};

/// A vector contains NaN or an infinity. Rejected at insertion time so that
/// undefined ordering can never enter an index. See docs/decisions/ADR-0007-nan-policy.md.
class InvalidVectorError : public InvalidArgumentError {
public:
    using InvalidArgumentError::InvalidArgumentError;
};

/// A lookup for an id / database / key that does not exist.
class NotFoundError : public Error {
public:
    using Error::Error;
};

/// An insertion collided with an existing id under the "reject" duplicate policy.
class DuplicateIdError : public Error {
public:
    explicit DuplicateIdError(VectorId id);

    [[nodiscard]] VectorId id() const noexcept { return id_; }

private:
    VectorId id_;
};

/// Something went wrong talking to the filesystem: open, read, write, rename,
/// fsync. Carries the path and, where available, the errno text.
class IoError : public Error {
public:
    using Error::Error;

    /// Builds "<operation> failed for <path>: <strerror(errno)>".
    static IoError from_errno(std::string_view operation,
                              const std::filesystem::path& path,
                              int error_number);
};

/// A persisted artefact failed validation: bad magic, unsupported version,
/// an offset past end-of-file, a neighbour id outside the node range.
///
/// This is the error that must never be swallowed. A corrupt index is
/// recoverable (rebuild from the authoritative vector store); silently
/// searching a corrupt graph is not.
class CorruptionError : public Error {
public:
    using Error::Error;
};

/// The on-disk format is newer than this binary understands.
class UnsupportedVersionError : public CorruptionError {
public:
    UnsupportedVersionError(std::string_view artefact,
                            std::uint32_t found,
                            std::uint32_t supported);
};

/// The operation is understood but not implemented in this build/version.
class NotImplementedError : public Error {
public:
    using Error::Error;
};

/// Metadata-layer failure (SQLite). Carries the SQLite result code where known.
class MetadataError : public Error {
public:
    using Error::Error;
};

}  // namespace vectordb
