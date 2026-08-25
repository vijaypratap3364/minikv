#pragma once

#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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

}  // namespace minikv::test
