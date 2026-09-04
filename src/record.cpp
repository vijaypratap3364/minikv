#include "record.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <utility>

namespace minikv::detail {
namespace {

constexpr std::array<char, 4> record_magic{'M', 'K', 'V', 'R'};
constexpr std::size_t version_offset = 4;
constexpr std::size_t operation_offset = 5;
constexpr std::size_t reserved_offset = 6;
constexpr std::size_t key_length_offset = 8;
constexpr std::size_t value_length_offset = 12;
constexpr std::uint32_t crc32_reversed_polynomial = 0xEDB88320U;

[[nodiscard]] char byte_as_char(std::uint8_t value) noexcept {
    return std::bit_cast<char>(value);
}

[[nodiscard]] std::uint8_t char_as_byte(char value) noexcept {
    return std::bit_cast<std::uint8_t>(value);
}

[[nodiscard]] consteval std::array<std::uint32_t, 256> make_crc32_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t index = 0; index < table.size(); ++index) {
        auto remainder = index;
        for (unsigned int bit = 0; bit < 8; ++bit) {
            remainder = (remainder & 1U) != 0
                            ? (remainder >> 1U) ^ crc32_reversed_polynomial
                            : remainder >> 1U;
        }
        table[index] = remainder;
    }
    return table;
}

constexpr auto crc32_table = make_crc32_table();

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
                throw InvalidRecordError(
                    "DELETE tombstone must have an empty value");
            }
            break;
        default:
            throw InvalidRecordError("record has an invalid operation");
    }

    if (record.key.size() > maximum_key_size) {
        throw InvalidRecordError("record key exceeds the maximum size");
    }
    if (record.value.size() > maximum_value_size) {
        throw InvalidRecordError("record value exceeds the maximum size");
    }
}

[[nodiscard]] Operation decode_operation(std::uint8_t encoded_operation) {
    switch (encoded_operation) {
        case static_cast<std::uint8_t>(Operation::Put):
            return Operation::Put;
        case static_cast<std::uint8_t>(Operation::Delete):
            return Operation::Delete;
        default:
            throw InvalidRecordError("record has an unknown operation");
    }
}

}  // namespace

std::uint32_t crc32(std::span<const char> input) noexcept {
    std::uint32_t checksum = 0xFFFFFFFFU;
    for (const char encoded_byte : input) {
        const auto table_index = static_cast<std::uint8_t>(
            checksum ^ char_as_byte(encoded_byte));
        checksum = crc32_table[table_index] ^ (checksum >> 8U);
    }
    return checksum ^ 0xFFFFFFFFU;
}

EncodedRecord encode_record(const Record& record) {
    validate_record(record);

    const auto key_length = static_cast<std::uint32_t>(record.key.size());
    const auto value_length = static_cast<std::uint32_t>(record.value.size());

    EncodedRecord output;
    output.reserve(record_header_size + record.key.size() +
                   record.value.size() + record_checksum_size);
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
    append_u32_little_endian(output, crc32(output));
    return output;
}

DecodedRecordHeader decode_record_header(std::span<const char> input) {
    if (input.size() < record_header_size) {
        throw IncompleteRecordError("record header is incomplete");
    }

    for (std::size_t index = 0; index < record_magic.size(); ++index) {
        if (input[index] != record_magic[index]) {
            throw InvalidRecordError("record magic is invalid");
        }
    }

    if (char_as_byte(input[version_offset]) != record_format_version) {
        throw InvalidRecordError("record version is unsupported");
    }

    const auto operation =
        decode_operation(char_as_byte(input[operation_offset]));
    if (input[reserved_offset] != '\0' || input[reserved_offset + 1] != '\0') {
        throw InvalidRecordError("record reserved bytes must be zero");
    }

    const auto key_length =
        read_u32_little_endian(input, key_length_offset);
    const auto value_length =
        read_u32_little_endian(input, value_length_offset);
    if (key_length > maximum_key_size) {
        throw InvalidRecordError(
            "encoded key length exceeds the maximum size");
    }
    if (value_length > maximum_value_size) {
        throw InvalidRecordError(
            "encoded value length exceeds the maximum size");
    }
    if (operation == Operation::Delete && value_length != 0) {
        throw InvalidRecordError(
            "DELETE tombstone must have a zero value length");
    }
    const auto payload_size = static_cast<std::size_t>(key_length) +
                              static_cast<std::size_t>(value_length);
    const auto encoded_size =
        record_header_size + payload_size + record_checksum_size;
    return DecodedRecordHeader{
        operation, key_length, value_length, encoded_size};
}

DecodedRecord decode_record(std::span<const char> input) {
    const auto header = decode_record_header(input);
    if (input.size() < header.encoded_size) {
        throw IncompleteRecordError("record payload or checksum is incomplete");
    }

    const auto checksum_offset = header.encoded_size - record_checksum_size;
    const auto stored_checksum =
        read_u32_little_endian(input, checksum_offset);
    const auto computed_checksum = crc32(input.first(checksum_offset));
    if (stored_checksum != computed_checksum) {
        throw ChecksumMismatchError("record checksum does not match");
    }

    std::string key(input.data() + record_header_size, header.key_length);
    std::string value(input.data() + record_header_size + header.key_length,
                      header.value_length);
    return DecodedRecord{
        Record{header.operation, std::move(key), std::move(value)},
        header.encoded_size};
}

}  // namespace minikv::detail
