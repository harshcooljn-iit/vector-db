// SPDX-License-Identifier: MIT
//
// File primitives: RAII descriptors, atomic replacement, memory mapping.
//
// Learning notes: learnings/10-cpp-systems-foundations/06-system-calls.md
//                 learnings/80-database-engineering/02-crash-safety.md
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace vectordb {

/// An owning file descriptor.
///
/// `std::ofstream` would be simpler, but it gives no portable way to `fsync`,
/// and without `fsync` "the write returned" means only "the kernel has it in a
/// page cache", not "it is on the disk". A crash between those two states loses
/// data that the program believes it saved. Crash safety needs the raw
/// descriptor, so the raw descriptor gets an RAII wrapper.
class FileDescriptor {
public:
    FileDescriptor() noexcept = default;

    explicit FileDescriptor(int fd) noexcept : fd_(fd) {}

    ~FileDescriptor();

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept;
    FileDescriptor& operator=(FileDescriptor&& other) noexcept;

    [[nodiscard]] int get() const noexcept { return fd_; }

    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

    /// Closes early, reporting failure. The destructor cannot throw, so a
    /// caller who needs to know that `close` succeeded must call this.
    /// @throws IoError
    void close();

private:
    int fd_ = -1;
};

/// Writes `chunks` to `path` such that a reader ever sees the complete old file
/// or the complete new one — never a partial write.
///
/// The sequence, and why each step is load-bearing:
///
/// 1. write everything to `path.tmp`
/// 2. `fsync(tmp)`     — the bytes reach the disk, not just the page cache
/// 3. `rename(tmp, path)` — atomic on POSIX: an observer sees one or the other
/// 4. `fsync(directory)`  — the *rename* itself reaches the disk
///
/// Step 4 is the one everybody forgets. `rename` modifies the directory entry,
/// which is its own metadata block with its own caching. Without syncing the
/// directory, a crash can leave the new file's contents durable but the
/// directory still pointing at the old name.
///
/// Taking a list of chunks rather than one buffer means a 3 GB vector file does
/// not have to be assembled in memory before being written.
///
/// @throws IoError
void write_file_atomically(const std::filesystem::path& path,
                           std::span<const std::span<const std::byte>> chunks);

/// Convenience overload for a single buffer.
void write_file_atomically(const std::filesystem::path& path, std::span<const std::byte> data);

/// Reads an entire file into memory.
///
/// @param max_bytes  refuse anything larger. A caller pointed at the wrong file
///                   should get an error, not an out-of-memory kill.
/// @throws IoError, CorruptionError
[[nodiscard]] std::vector<std::byte> read_file(const std::filesystem::path& path,
                                               std::uint64_t max_bytes);

/// Reads the first `count` bytes — enough to validate a header before deciding
/// whether the rest of the file is worth touching.
/// @throws IoError, CorruptionError
[[nodiscard]] std::vector<std::byte> read_file_prefix(const std::filesystem::path& path,
                                                      std::size_t count);

/// A read-only memory mapping of a whole file.
///
/// The file's bytes become an address range. Nothing is read until it is
/// touched; the OS faults pages in on demand and can evict them under pressure
/// without ever going back to us. For a vector store that is larger than RAM,
/// this is the difference between "cannot open" and "the working set stays
/// resident and the rest lives on disk".
///
/// It works here only because the in-memory layout and the on-disk layout of
/// the float payload are byte-identical, which is exactly what
/// docs/decisions/ADR-0004-contiguous-storage.md gave up padding to preserve.
///
/// Learning note: learnings/50-storage/03-memory-mapped-files.md
class MappedFile {
public:
    MappedFile() noexcept = default;

    /// @throws IoError if the file cannot be opened or mapped
    explicit MappedFile(const std::filesystem::path& path);

    ~MappedFile();

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {static_cast<const std::byte*>(data_), size_};
    }

    [[nodiscard]] const void* data() const noexcept { return data_; }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    [[nodiscard]] bool valid() const noexcept { return data_ != nullptr; }

    /// Hints that the whole mapping will be read front to back, so the kernel
    /// may read ahead aggressively. Advisory: ignoring it is always correct.
    void advise_sequential() const noexcept;

    /// Hints that access will be scattered — which is what an HNSW graph
    /// traversal does — so read-ahead is wasted bandwidth.
    void advise_random() const noexcept;

private:
    void release() noexcept;

    const void* data_ = nullptr;
    std::size_t size_ = 0;
};

/// True if memory mapping is compiled in and usable on this platform.
[[nodiscard]] bool mmap_supported() noexcept;

}  // namespace vectordb
