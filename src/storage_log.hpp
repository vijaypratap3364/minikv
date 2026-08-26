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

struct LocatedRecord {
    Record record;
    AppendResult location;
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
    [[nodiscard]] LocatedRecord read_at(std::uint64_t offset) const;
    [[nodiscard]] Record read(const AppendResult& location) const;
    [[nodiscard]] std::uint64_t size() const noexcept;

private:
    std::filesystem::path path_;
    std::ofstream append_stream_;
    mutable std::ifstream read_stream_;
    std::uint64_t next_offset_{0};
};

}  // namespace minikv::detail
