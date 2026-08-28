// SPDX-License-Identifier: MIT
#include <cerrno>
#include <string>
#include <utility>

#include <vectordb/core/error.hpp>
#include <vectordb/persistence/file_io.hpp>

#if defined(_WIN32)
#define VECTORDB_HAVE_POSIX_IO 0
#else
#define VECTORDB_HAVE_POSIX_IO 1
#include <sys/mman.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <unistd.h>
#endif

namespace vectordb {
namespace {

#if VECTORDB_HAVE_POSIX_IO

/// Writes the whole buffer, looping until it is done.
///
/// `write()` is permitted to write fewer bytes than asked — on a signal, on a
/// pipe, on some filesystems for very large writes. Treating a short write as
/// success is a classic data-loss bug, and it is invisible in testing because
/// small writes almost never come up short.
void write_all(int fd,
               const std::byte* data,
               std::size_t size,
               const std::filesystem::path& path) {
    std::size_t written = 0;
    while (written < size) {
        const ::ssize_t result = ::write(fd, data + written, size - written);
        if (result < 0) {
            if (errno == EINTR) {
                continue;  // interrupted by a signal; not an error
            }
            throw IoError::from_errno("write", path, errno);
        }
        written += static_cast<std::size_t>(result);
    }
}

void fsync_or_throw(int fd, const std::filesystem::path& path) {
#if defined(__APPLE__)
    // On macOS, fsync() only pushes data to the drive's *own* cache. F_FULLFSYNC
    // asks the drive to commit to stable media. It is much slower and it is what
    // "durable" actually means here.
    if (::fcntl(fd, F_FULLFSYNC) == -1) {
        // Not every filesystem implements it (network mounts, some virtualised
        // volumes). Falling back to fsync is better than failing the write, but
        // the weaker guarantee is documented in docs/persistence.md.
        if (::fsync(fd) == -1) {
            throw IoError::from_errno("fsync", path, errno);
        }
    }
#else
    if (::fsync(fd) == -1) {
        throw IoError::from_errno("fsync", path, errno);
    }
#endif
}

/// Syncs the *directory* so that the rename itself is durable.
void fsync_directory(const std::filesystem::path& directory) {
    const int fd = ::open(directory.c_str(), O_RDONLY);
    if (fd == -1) {
        throw IoError::from_errno("open directory", directory, errno);
    }
    FileDescriptor guard(fd);
    // Some filesystems refuse fsync on a directory. That is not a reason to
    // fail the whole write, so this one is best-effort and deliberately silent.
    ::fsync(fd);
}

#endif  // VECTORDB_HAVE_POSIX_IO

}  // namespace

// ---------------------------------------------------------------------------
// FileDescriptor
// ---------------------------------------------------------------------------

FileDescriptor::~FileDescriptor() {
#if VECTORDB_HAVE_POSIX_IO
    if (fd_ >= 0) {
        // A destructor must not throw, so a failing close here is unreportable.
        // That is precisely why close() exists as an explicit method: any code
        // that needs to know writes succeeded calls it before destruction.
        ::close(fd_);
    }
#endif
}

FileDescriptor::FileDescriptor(FileDescriptor&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)) {}

FileDescriptor& FileDescriptor::operator=(FileDescriptor&& other) noexcept {
    if (this != &other) {
#if VECTORDB_HAVE_POSIX_IO
        if (fd_ >= 0) {
            ::close(fd_);
        }
#endif
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

void FileDescriptor::close() {
#if VECTORDB_HAVE_POSIX_IO
    if (fd_ >= 0) {
        const int fd = std::exchange(fd_, -1);
        if (::close(fd) == -1) {
            throw IoError::from_errno("close", "<fd>", errno);
        }
    }
#endif
}

// ---------------------------------------------------------------------------
// Atomic write
// ---------------------------------------------------------------------------

void write_file_atomically(const std::filesystem::path& path,
                           std::span<const std::span<const std::byte>> chunks) {
#if !VECTORDB_HAVE_POSIX_IO
    throw NotImplementedError(
        "atomic file replacement is implemented for POSIX platforms only");
#else
    const std::filesystem::path temporary = path.string() + ".tmp";

    {
        const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd == -1) {
            throw IoError::from_errno("open for write", temporary, errno);
        }
        FileDescriptor file(fd);

        for (const std::span<const std::byte>& chunk : chunks) {
            if (!chunk.empty()) {
                write_all(fd, chunk.data(), chunk.size(), temporary);
            }
        }

        // The bytes must be durable *before* the rename, or a crash can leave
        // the directory pointing at a file whose contents never made it.
        fsync_or_throw(fd, temporary);
        file.close();
    }

    if (::rename(temporary.c_str(), path.c_str()) == -1) {
        const int saved = errno;
        ::unlink(temporary.c_str());
        throw IoError::from_errno("rename", path, saved);
    }

    // And the rename itself lives in the directory's metadata, which has its
    // own caching. This is the step everyone forgets.
    fsync_directory(path.parent_path().empty() ? std::filesystem::path(".")
                                               : path.parent_path());
#endif
}

void write_file_atomically(const std::filesystem::path& path, std::span<const std::byte> data) {
    const std::span<const std::byte> one[] = {data};
    write_file_atomically(path, one);
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

std::vector<std::byte> read_file(const std::filesystem::path& path, std::uint64_t max_bytes) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error) {
        throw IoError("cannot stat '" + path.string() + "': " + error.message());
    }
    // Checked before the allocation, not after. A caller pointed at a 40 GB
    // file by mistake gets an error, not an out-of-memory kill.
    if (static_cast<std::uint64_t>(size) > max_bytes) {
        throw CorruptionError("'" + path.string() + "' is " + std::to_string(size) +
                              " bytes, which exceeds the " + std::to_string(max_bytes) +
                              " byte limit for this artefact");
    }

#if !VECTORDB_HAVE_POSIX_IO
    throw NotImplementedError("file reading is implemented for POSIX platforms only");
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd == -1) {
        throw IoError::from_errno("open", path, errno);
    }
    FileDescriptor file(fd);

    std::vector<std::byte> buffer(static_cast<std::size_t>(size));
    std::size_t read_total = 0;
    while (read_total < buffer.size()) {
        const ::ssize_t result =
            ::read(fd, buffer.data() + read_total, buffer.size() - read_total);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw IoError::from_errno("read", path, errno);
        }
        if (result == 0) {
            // The file shrank between stat and read — another process is
            // writing it. Reporting corruption is more useful than returning a
            // silently short buffer.
            throw CorruptionError(
                "'" + path.string() + "' ended after " + std::to_string(read_total) + " of " +
                std::to_string(buffer.size()) + " bytes; is it being written concurrently?");
        }
        read_total += static_cast<std::size_t>(result);
    }
    return buffer;
#endif
}

std::vector<std::byte> read_file_prefix(const std::filesystem::path& path, std::size_t count) {
#if !VECTORDB_HAVE_POSIX_IO
    throw NotImplementedError("file reading is implemented for POSIX platforms only");
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd == -1) {
        throw IoError::from_errno("open", path, errno);
    }
    FileDescriptor file(fd);

    std::vector<std::byte> buffer(count);
    std::size_t read_total = 0;
    while (read_total < count) {
        const ::ssize_t result = ::read(fd, buffer.data() + read_total, count - read_total);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw IoError::from_errno("read", path, errno);
        }
        if (result == 0) {
            throw CorruptionError("'" + path.string() + "' is only " +
                                  std::to_string(read_total) + " bytes; at least " +
                                  std::to_string(count) + " were required for its header");
        }
        read_total += static_cast<std::size_t>(result);
    }
    return buffer;
#endif
}

// ---------------------------------------------------------------------------
// Memory mapping
// ---------------------------------------------------------------------------

bool mmap_supported() noexcept {
    return VECTORDB_HAVE_POSIX_IO != 0;
}

MappedFile::MappedFile(const std::filesystem::path& path) {
#if !VECTORDB_HAVE_POSIX_IO
    throw NotImplementedError("memory mapping is implemented for POSIX platforms only");
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd == -1) {
        throw IoError::from_errno("open for mapping", path, errno);
    }
    FileDescriptor file(fd);

    struct ::stat info{};
    if (::fstat(fd, &info) == -1) {
        throw IoError::from_errno("fstat", path, errno);
    }
    if (info.st_size == 0) {
        // mmap of a zero-length file fails with EINVAL. An empty mapping is a
        // perfectly reasonable thing for a caller to hold, so represent it as
        // one rather than as an error.
        return;
    }

    void* address =
        ::mmap(nullptr, static_cast<std::size_t>(info.st_size), PROT_READ, MAP_SHARED, fd, 0);
    if (address == MAP_FAILED) {
        throw IoError::from_errno("mmap", path, errno);
    }

    // The descriptor may be closed immediately: the mapping keeps its own
    // reference to the underlying file, which is one of mmap's quiet
    // conveniences.
    data_ = address;
    size_ = static_cast<std::size_t>(info.st_size);
#endif
}

void MappedFile::release() noexcept {
#if VECTORDB_HAVE_POSIX_IO
    if (data_ != nullptr) {
        ::munmap(const_cast<void*>(data_), size_);
    }
#endif
    data_ = nullptr;
    size_ = 0;
}

MappedFile::~MappedFile() {
    release();
}

MappedFile::MappedFile(MappedFile&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        release();
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

void MappedFile::advise_sequential() const noexcept {
#if VECTORDB_HAVE_POSIX_IO
    if (data_ != nullptr) {
        ::madvise(const_cast<void*>(data_), size_, MADV_SEQUENTIAL);
    }
#endif
}

void MappedFile::advise_random() const noexcept {
#if VECTORDB_HAVE_POSIX_IO
    if (data_ != nullptr) {
        ::madvise(const_cast<void*>(data_), size_, MADV_RANDOM);
    }
#endif
}

}  // namespace vectordb
