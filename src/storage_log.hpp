#pragma once

#include "record.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace minikv::detail {

struct AppendResult {
    std::uint64_t offset;
    std::uint64_t size;
};

class StorageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class StorageLog {
public:
    explicit StorageLog(std::filesystem::path path);

    StorageLog(const StorageLog&) = delete;
    StorageLog& operator=(const StorageLog&) = delete;
    StorageLog(StorageLog&&) = default;
    StorageLog& operator=(StorageLog&&) = default;

    [[nodiscard]] AppendResult append(const Record& record);

private:
    std::filesystem::path path_;
    std::ofstream stream_;
    std::uint64_t next_offset_{0};
};

}  // namespace minikv::detail
