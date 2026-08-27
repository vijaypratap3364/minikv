#pragma once

#include "segmented_storage.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace minikv::test {

class TestRunner {
public:
    void expect(bool condition, std::string_view description) {
        ++checks_;
        if (!condition) {
            ++failures_;
            std::cerr << "FAIL: " << description << '\n';
        }
    }

    template <typename ExpectedException, typename Function>
    void expect_throws(Function&& function, std::string_view description) {
        ++checks_;
        try {
            std::forward<Function>(function)();
        } catch (const ExpectedException&) {
            return;
        } catch (const std::exception& error) {
            ++failures_;
            std::cerr << "FAIL: " << description
                      << " (unexpected exception: " << error.what() << ")\n";
            return;
        } catch (...) {
            ++failures_;
            std::cerr << "FAIL: " << description
                      << " (unexpected non-standard exception)\n";
            return;
        }

        ++failures_;
        std::cerr << "FAIL: " << description << " (no exception)\n";
    }

    [[nodiscard]] int finish() const {
        if (failures_ == 0) {
            std::cout << "All " << checks_ << " checks passed\n";
            return 0;
        }

        std::cerr << failures_ << " of " << checks_ << " checks failed\n";
        return 1;
    }

private:
    std::size_t checks_{0};
    std::size_t failures_{0};
};

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto temporary_root = std::filesystem::temp_directory_path();
        for (std::size_t attempt = 0; attempt < 1'000; ++attempt) {
            auto candidate =
                temporary_root / ("minikv-tests-" + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                path_ = std::move(candidate);
                return;
            }
            if (error) {
                throw std::runtime_error("could not create test directory: " +
                                         error.message());
            }
        }

        throw std::runtime_error("could not find a free test directory name");
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] inline std::vector<char> read_file(
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        throw std::runtime_error("could not open test file: " + path.string());
    }

    return std::vector<char>(std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>());
}

inline void write_file(const std::filesystem::path& path,
                       const std::vector<char>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        throw std::runtime_error("could not create test file: " + path.string());
    }
    if (!bytes.empty()) {
        output.write(bytes.data(),
                     static_cast<std::streamsize>(bytes.size()));
    }
    output.close();
    if (!output) {
        throw std::runtime_error("could not write test file: " + path.string());
    }
}

[[nodiscard]] inline std::uint64_t current_generation_id(
    const std::filesystem::path& database_path) {
    std::ifstream input(database_path / "CURRENT");
    std::string magic;
    std::uint32_t version = 0;
    std::uint64_t generation = 0;
    if (!(input >> magic >> version >> generation) ||
        magic != "MINIKV-MANIFEST" || version != 1 || generation == 0) {
        throw std::runtime_error("could not read test database manifest");
    }
    return generation;
}

[[nodiscard]] inline std::filesystem::path current_generation_path(
    const std::filesystem::path& database_path) {
    return database_path /
           minikv::detail::generation_directory_name(
               current_generation_id(database_path));
}

[[nodiscard]] inline std::vector<std::filesystem::path> segment_paths(
    const std::filesystem::path& database_path) {
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(
             current_generation_path(database_path))) {
        if (entry.is_regular_file() &&
            entry.path().extension() == ".dat") {
            paths.push_back(entry.path());
        }
    }
    std::ranges::sort(paths);
    return paths;
}

[[nodiscard]] inline std::filesystem::path active_segment_path(
    const std::filesystem::path& database_path) {
    const auto paths = segment_paths(database_path);
    if (paths.empty()) {
        throw std::runtime_error("test database has no active segment");
    }
    return paths.back();
}

[[nodiscard]] inline std::uintmax_t database_data_size(
    const std::filesystem::path& database_path) {
    std::uintmax_t size = 0;
    for (const auto& path : segment_paths(database_path)) {
        size += std::filesystem::file_size(path);
    }
    return size;
}

inline void initialize_database_with_segment(
    const std::filesystem::path& database_path,
    const std::vector<char>& bytes) {
    const auto generation_id = std::uint64_t{1};
    const auto generation_path =
        database_path /
        minikv::detail::generation_directory_name(generation_id);
    std::error_code error;
    if (!std::filesystem::create_directory(database_path, error) && error) {
        throw std::runtime_error("could not create test database: " +
                                 error.message());
    }
    if (!std::filesystem::create_directory(generation_path, error) && error) {
        throw std::runtime_error("could not create test generation: " +
                                 error.message());
    }
    write_file(generation_path / minikv::detail::segment_file_name(1), bytes);

    std::ofstream manifest(database_path / "CURRENT", std::ios::trunc);
    manifest << "MINIKV-MANIFEST 1 " << generation_id << '\n';
    manifest.close();
    if (!manifest) {
        throw std::runtime_error("could not create test database manifest");
    }
}

}  // namespace minikv::test
