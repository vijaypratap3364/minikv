#include "storage_log.hpp"

#include <cstdint>
#include <limits>
#include <system_error>
#include <utility>

namespace minikv::detail {

StorageLog::StorageLog(std::filesystem::path path)
    : path_(std::move(path)),
      stream_(path_, std::ios::binary | std::ios::out | std::ios::app) {
    if (!stream_.is_open()) {
        throw StorageError("could not open storage log: " + path_.string());
    }

    std::error_code error;
    const auto file_size = std::filesystem::file_size(path_, error);
    if (error) {
        throw StorageError("could not determine storage log size: " +
                           error.message());
    }
    if (file_size > std::numeric_limits<std::uint64_t>::max()) {
        throw StorageError("storage log is too large to address");
    }

    next_offset_ = static_cast<std::uint64_t>(file_size);
}

AppendResult StorageLog::append(const Record& record) {
    const auto encoded = encode_record(record);
    if (encoded.size() >
        static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        throw StorageError("encoded record is too large to write");
    }
    if (encoded.size() >
        std::numeric_limits<std::uint64_t>::max() - next_offset_) {
        throw StorageError("storage log offset would overflow");
    }

    const auto offset = next_offset_;
    const auto encoded_size = static_cast<std::uint64_t>(encoded.size());
    stream_.write(encoded.data(),
                  static_cast<std::streamsize>(encoded.size()));
    stream_.flush();
    if (!stream_) {
        throw StorageError("could not append to storage log: " +
                           path_.string());
    }

    next_offset_ += encoded_size;
    return AppendResult{offset, encoded_size};
}

}  // namespace minikv::detail
