#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace minikv::detail {

inline constexpr std::uint8_t record_format_version = 1;
inline constexpr std::size_t record_header_size = 16;
inline constexpr std::size_t maximum_key_size = 64U * 1024U;
inline constexpr std::size_t maximum_value_size = 4U * 1024U * 1024U;

enum class Operation : std::uint8_t {
    Put = 1,
};

struct Record {
    Operation operation;
    std::string key;
    std::string value;

    bool operator==(const Record&) const = default;
};

using EncodedRecord = std::vector<char>;

struct DecodedRecord {
    Record record;
    std::size_t bytes_consumed;
};

struct DecodedRecordHeader {
    Operation operation;
    std::uint32_t key_length;
    std::uint32_t value_length;
    std::size_t encoded_size;
};

class RecordError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] EncodedRecord encode_record(const Record& record);
[[nodiscard]] DecodedRecordHeader decode_record_header(
    std::span<const char> input);
[[nodiscard]] DecodedRecord decode_record(std::span<const char> input);

}  // namespace minikv::detail
