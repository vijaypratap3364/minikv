#include "record.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

namespace minikv::detail {
namespace {

constexpr std::array<char, 4> record_magic{'M', 'K', 'V', 'R'};
constexpr std::size_t version_offset = 4;
constexpr std::size_t operation_offset = 5;
constexpr std::size_t reserved_offset = 6;
constexpr std::size_t key_length_offset = 8;
constexpr std::size_t value_length_offset = 12;

[[nodiscard]] char byte_as_char(std::uint8_t value) noexcept {
    return std::bit_cast<char>(value);
}

[[nodiscard]] std::uint8_t char_as_byte(char value) noexcept {
    return std::bit_cast<std::uint8_t>(value);
}

void append_u32_little_endian(EncodedRecord& output, std::uint32_t value) {
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        output.push_back(
            byte_as_char(static_cast<std::uint8_t>((value >> shift) & 0xFFU)));
    }
}

[[nodiscard]] std::uint32_t read_u32_little_endian(
    std::span<const char> input,
    std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(char_as_byte(input[offset++]))
                 << shift;
    }
    return value;
}

void validate_record(const Record& record) {
    switch (record.operation) {
        case Operation::Put:
            break;
        case Operation::Delete:
            if (!record.value.empty()) {
                throw RecordError("delete records cannot contain a value");
            }
            break;
        default:
            throw RecordError("record has an invalid operation");
    }

    if (record.key.size() > maximum_key_size) {
        throw RecordError("record key exceeds the maximum size");
    }
    if (record.value.size() > maximum_value_size) {
        throw RecordError("record value exceeds the maximum size");
    }
}

[[nodiscard]] Operation decode_operation(std::uint8_t encoded_operation) {
    switch (encoded_operation) {
        case static_cast<std::uint8_t>(Operation::Put):
            return Operation::Put;
        case static_cast<std::uint8_t>(Operation::Delete):
            return Operation::Delete;
        default:
            throw RecordError("record has an unknown operation");
    }
}

}  // namespace

EncodedRecord encode_record(const Record& record) {
    validate_record(record);

    const auto key_length = static_cast<std::uint32_t>(record.key.size());
    const auto value_length = static_cast<std::uint32_t>(record.value.size());

    EncodedRecord output;
    output.reserve(record_header_size + record.key.size() + record.value.size());
    output.insert(output.end(), record_magic.begin(), record_magic.end());
    output.push_back(byte_as_char(record_format_version));
    output.push_back(
        byte_as_char(static_cast<std::uint8_t>(record.operation)));
    output.push_back('\0');
    output.push_back('\0');
    append_u32_little_endian(output, key_length);
    append_u32_little_endian(output, value_length);
    output.insert(output.end(), record.key.begin(), record.key.end());
    output.insert(output.end(), record.value.begin(), record.value.end());
    return output;
}

DecodedRecord decode_record(std::span<const char> input) {
    if (input.size() < record_header_size) {
        throw RecordError("record header is truncated");
    }

    for (std::size_t index = 0; index < record_magic.size(); ++index) {
        if (input[index] != record_magic[index]) {
            throw RecordError("record magic is invalid");
        }
    }

    if (char_as_byte(input[version_offset]) != record_format_version) {
        throw RecordError("record version is unsupported");
    }

    const auto operation =
        decode_operation(char_as_byte(input[operation_offset]));
    if (input[reserved_offset] != '\0' || input[reserved_offset + 1] != '\0') {
        throw RecordError("record reserved bytes must be zero");
    }

    const auto key_length =
        read_u32_little_endian(input, key_length_offset);
    const auto value_length =
        read_u32_little_endian(input, value_length_offset);
    if (key_length > maximum_key_size) {
        throw RecordError("encoded key length exceeds the maximum size");
    }
    if (value_length > maximum_value_size) {
        throw RecordError("encoded value length exceeds the maximum size");
    }
    if (operation == Operation::Delete && value_length != 0) {
        throw RecordError("delete record has a nonzero value length");
    }

    const auto payload_size = static_cast<std::size_t>(key_length) +
                              static_cast<std::size_t>(value_length);
    const auto encoded_size = record_header_size + payload_size;
    if (input.size() < encoded_size) {
        throw RecordError("record payload is truncated");
    }

    std::string key(input.data() + record_header_size, key_length);
    std::string value(input.data() + record_header_size + key_length,
                      value_length);
    return DecodedRecord{
        Record{operation, std::move(key), std::move(value)}, encoded_size};
}

}  // namespace minikv::detail
