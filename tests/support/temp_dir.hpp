// SPDX-License-Identifier: MIT
//
// A scratch directory that cleans itself up, so a failing test cannot leave
// megabytes of vectors behind in /tmp.
#pragma once

#include <atomic>
#include <filesystem>
#include <string>

namespace vectordb::testing {

class TempDir {
public:
    explicit TempDir(std::string_view label = "vectordb") {
        static std::atomic<unsigned> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                (std::string(label) + "-test-" + std::to_string(::getpid()) + "-" +
                 std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(path_);
    }

    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    [[nodiscard]] std::filesystem::path file(std::string_view name) const {
        return path_ / name;
    }

private:
    std::filesystem::path path_;
};

}  // namespace vectordb::testing
