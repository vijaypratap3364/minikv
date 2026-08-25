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
    Delete = 2,
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

class RecordError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] EncodedRecord encode_record(const Record& record);
[[nodiscard]] DecodedRecord decode_record(std::span<const char> input);

}  // namespace minikv::detail
