#include "storage_log.hpp"

#include "minikv/minikv.hpp"
#include "platform_sync.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

namespace minikv::detail {

StorageLog::StorageLog(std::filesystem::path path,
                       DurabilityMode durability_mode)
    : path_(std::move(path)), durability_mode_(durability_mode) {
    if (durability_mode_ != DurabilityMode::Buffered &&
        durability_mode_ != DurabilityMode::Sync) {
        throw StorageError("storage log has an invalid durability mode");
    }
    open_streams();

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
    if (append_failed_) {
        throw StorageError(
            "storage log cannot append after an earlier append failure");
    }

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
    append_stream_.write(encoded.data(),
                         static_cast<std::streamsize>(encoded.size()));
    append_stream_.flush();
    if (!append_stream_) {
        append_failed_ = true;
        throw StorageError("could not append to storage log: " +
                           path_.string());
    }
    try {
        sync_if_requested();
    } catch (...) {
        append_failed_ = true;
        throw;
    }

    next_offset_ += encoded_size;
    return AppendResult{offset, encoded_size};
}

LocatedRecord StorageLog::read_at(std::uint64_t offset) const {
    if (offset >
        static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        throw StorageError("storage log offset is too large to seek");
    }

    read_stream_.clear();
    read_stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!read_stream_) {
        throw StorageError("could not seek in storage log: " + path_.string());
    }

    std::array<char, record_header_size> header_bytes{};
    read_stream_.read(header_bytes.data(),
                      static_cast<std::streamsize>(header_bytes.size()));
    if (read_stream_.gcount() !=
        static_cast<std::streamsize>(header_bytes.size())) {
        if (read_stream_.bad()) {
            throw StorageError("could not read storage log header: " +
                               path_.string());
        }
        throw IncompleteRecordError("record header is incomplete");
    }

    const auto header = decode_record_header(header_bytes);
    EncodedRecord encoded(header.encoded_size);
    std::ranges::copy(header_bytes, encoded.begin());

    const auto payload_size = header.encoded_size - record_header_size;
    if (payload_size != 0) {
        read_stream_.read(
            encoded.data() + record_header_size,
            static_cast<std::streamsize>(payload_size));
        if (read_stream_.gcount() !=
            static_cast<std::streamsize>(payload_size)) {
            if (read_stream_.bad()) {
                throw StorageError("could not read storage log payload: " +
                                   path_.string());
            }
            throw IncompleteRecordError(
                "record payload or checksum is incomplete");
        }
    }

    auto decoded = decode_record(encoded);
    return LocatedRecord{
        std::move(decoded.record),
        AppendResult{offset, static_cast<std::uint64_t>(decoded.bytes_consumed)}};
}

Record StorageLog::read(const AppendResult& location) const {
    auto located = read_at(location.offset);
    if (located.location.size != location.size) {
        throw StorageError("indexed record size does not match the storage log");
    }
    return std::move(located.record);
}

std::uint64_t StorageLog::size() const noexcept {
    return next_offset_;
}

void StorageLog::truncate(std::uint64_t size) {
    if (size > next_offset_) {
        throw StorageError("cannot extend storage log during tail recovery");
    }

    append_stream_.close();
    read_stream_.close();

    std::error_code error;
    std::filesystem::resize_file(
        path_, static_cast<std::uintmax_t>(size), error);
    if (error) {
        throw StorageError("could not truncate incomplete storage log tail: " +
                           error.message());
    }

    next_offset_ = size;
    sync_if_requested();
    open_streams();
}

void StorageLog::open_streams() {
    append_stream_.clear();
    read_stream_.clear();
    append_stream_.open(
        path_, std::ios::binary | std::ios::out | std::ios::app);
    read_stream_.open(path_, std::ios::binary | std::ios::in);
    if (!append_stream_.is_open() || !read_stream_.is_open()) {
        throw StorageError("could not open storage log: " + path_.string());
    }
}

void StorageLog::sync_if_requested() const {
    if (durability_mode_ == DurabilityMode::Buffered) {
        return;
    }

    try {
        sync_file_to_storage(path_);
    } catch (const std::system_error& error) {
        throw StorageError("could not durably sync storage log: " +
                           std::string(error.what()));
    }
}

}  // namespace minikv::detail
