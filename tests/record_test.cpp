#include "record.hpp"
#include "test_support.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace {

using minikv::detail::DecodedRecord;
using minikv::detail::EncodedRecord;
using minikv::detail::ChecksumMismatchError;
using minikv::detail::IncompleteRecordError;
using minikv::detail::InvalidRecordError;
using minikv::detail::Operation;
using minikv::detail::Record;
using minikv::test::TestRunner;

constexpr std::size_t version_offset = 4;
constexpr std::size_t operation_offset = 5;
constexpr std::size_t reserved_offset = 6;
constexpr std::size_t key_length_offset = 8;
constexpr std::size_t value_length_offset = 12;

[[nodiscard]] std::span<const char> as_span(const EncodedRecord& bytes) {
    return {bytes.data(), bytes.size()};
}

void write_u32_little_endian(EncodedRecord& bytes,
                             std::size_t offset,
                             std::uint32_t value) {
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        const auto byte =
            static_cast<std::uint8_t>((value >> shift) & 0xFFU);
        bytes[offset++] = std::bit_cast<char>(byte);
    }
}

void test_exact_version_two_layout(TestRunner& tests) {
    const auto encoded = minikv::detail::encode_record(
        Record{Operation::Put, "K", "V"});
    EncodedRecord expected{
        'M', 'K', 'V', 'R',
        '\2', '\1', '\0', '\0',
        '\1', '\0', '\0', '\0',
        '\1', '\0', '\0', '\0',
        'K', 'V',
    };
    for (const std::uint8_t checksum_byte : {0xB7U, 0x32U, 0xC5U, 0xF2U}) {
        expected.push_back(std::bit_cast<char>(checksum_byte));
    }

    tests.expect(encoded == expected,
                 "version 2 encoding matches the documented byte layout");

    const auto header = minikv::detail::decode_record_header(as_span(encoded));
    tests.expect(header.operation == Operation::Put,
                 "header decoder returns the PUT operation");
    tests.expect(header.key_length == 1,
                 "header decoder returns the key length");
    tests.expect(header.value_length == 1,
                 "header decoder returns the value length");
    tests.expect(header.encoded_size == encoded.size(),
                 "header decoder computes the complete record size");
}

void test_standard_crc32_vector(TestRunner& tests) {
    const std::string input{"123456789"};
    tests.expect(minikv::detail::crc32(
                     std::span<const char>(input.data(), input.size())) ==
                     0xCBF43926U,
                 "CRC-32 matches the standard check value");
}

void test_round_trip(TestRunner& tests) {
    const Record put{Operation::Put,
                     std::string{"key\0bytes", 9},
                     std::string{"value\0bytes", 11}};
    const auto encoded_put = minikv::detail::encode_record(put);
    const auto decoded_put = minikv::detail::decode_record(as_span(encoded_put));
    tests.expect(decoded_put.record == put,
                 "put record survives an encode/decode round trip");
    tests.expect(decoded_put.bytes_consumed == encoded_put.size(),
                 "put decoder reports the full record size");

    const Record tombstone{Operation::Delete,
                           std::string{"key\0bytes", 9},
                           {}};
    const auto encoded_tombstone = minikv::detail::encode_record(tombstone);
    const auto decoded_tombstone =
        minikv::detail::decode_record(as_span(encoded_tombstone));
    tests.expect(decoded_tombstone.record == tombstone,
                 "DELETE tombstone survives an encode/decode round trip");
}

void test_multiple_records(TestRunner& tests) {
    const Record first{Operation::Put, "first", "one"};
    const Record second{Operation::Delete, "first", {}};
    const Record third{Operation::Put, "third", "three"};

    EncodedRecord combined;
    for (const auto& record : {first, second, third}) {
        const auto encoded = minikv::detail::encode_record(record);
        combined.insert(combined.end(), encoded.begin(), encoded.end());
    }

    const std::span<const char> input(combined.data(), combined.size());
    std::size_t offset = 0;
    for (const auto& expected : {first, second, third}) {
        const DecodedRecord decoded =
            minikv::detail::decode_record(input.subspan(offset));
        tests.expect(decoded.record == expected,
                     "decoder reads the next record from a sequence");
        offset += decoded.bytes_consumed;
    }
    tests.expect(offset == combined.size(),
                 "multiple records consume the full input");
}

void test_empty_value(TestRunner& tests) {
    const Record record{Operation::Put, "empty", {}};
    const auto encoded = minikv::detail::encode_record(record);
    const auto decoded = minikv::detail::decode_record(as_span(encoded));
    tests.expect(decoded.record == record,
                 "put records preserve an empty value");
}

void test_maximum_lengths(TestRunner& tests) {
    const Record maximum{
        Operation::Put,
        std::string(minikv::detail::maximum_key_size, 'k'),
        std::string(minikv::detail::maximum_value_size, 'v')};
    const auto encoded = minikv::detail::encode_record(maximum);
    tests.expect(encoded.size() == minikv::detail::record_header_size +
                                       minikv::detail::maximum_key_size +
                                       minikv::detail::maximum_value_size +
                                       minikv::detail::record_checksum_size,
                 "maximum lengths produce the expected encoded size");
    const auto decoded = minikv::detail::decode_record(as_span(encoded));
    tests.expect(decoded.record == maximum,
                 "maximum allowed key and value lengths round trip");

    tests.expect_throws<InvalidRecordError>(
        [] {
            static_cast<void>(minikv::detail::encode_record(Record{
                Operation::Put,
                std::string(minikv::detail::maximum_key_size + 1, 'k'),
                {}}));
        },
        "encoder rejects a key above the maximum");
    tests.expect_throws<InvalidRecordError>(
        [] {
            static_cast<void>(minikv::detail::encode_record(Record{
                Operation::Put,
                "key",
                std::string(minikv::detail::maximum_value_size + 1, 'v')}));
        },
        "encoder rejects a value above the maximum");
}

void test_malformed_records(TestRunner& tests) {
    const auto valid =
        minikv::detail::encode_record(Record{Operation::Put, "key", "value"});

    auto invalid_magic = valid;
    invalid_magic[0] = 'X';
    tests.expect_throws<InvalidRecordError>(
        [&] {
            static_cast<void>(
                minikv::detail::decode_record(as_span(invalid_magic)));
        },
        "decoder rejects invalid magic");

    auto invalid_reserved = valid;
    invalid_reserved[reserved_offset] = '\1';
    tests.expect_throws<InvalidRecordError>(
        [&] {
            static_cast<void>(
                minikv::detail::decode_record(as_span(invalid_reserved)));
        },
        "decoder rejects nonzero reserved bytes");

    auto oversized_key = valid;
    write_u32_little_endian(
        oversized_key,
        key_length_offset,
        static_cast<std::uint32_t>(minikv::detail::maximum_key_size + 1));
    tests.expect_throws<InvalidRecordError>(
        [&] {
            static_cast<void>(
                minikv::detail::decode_record(as_span(oversized_key)));
        },
        "decoder rejects an oversized encoded key length");

    auto oversized_value = valid;
    write_u32_little_endian(
        oversized_value,
        value_length_offset,
        static_cast<std::uint32_t>(minikv::detail::maximum_value_size + 1));
    tests.expect_throws<InvalidRecordError>(
        [&] {
            static_cast<void>(
                minikv::detail::decode_record(as_span(oversized_value)));
        },
        "decoder rejects an oversized encoded value length");

    auto invalid_tombstone = valid;
    invalid_tombstone[operation_offset] =
        std::bit_cast<char>(static_cast<std::uint8_t>(Operation::Delete));
    tests.expect_throws<InvalidRecordError>(
        [&] {
            static_cast<void>(
                minikv::detail::decode_record(as_span(invalid_tombstone)));
        },
        "decoder rejects a tombstone with value bytes");

    tests.expect_throws<InvalidRecordError>(
        [] {
            static_cast<void>(minikv::detail::encode_record(
                Record{Operation::Delete, "key", "not allowed"}));
        },
        "encoder rejects a tombstone with a value");
}

void test_truncated_input(TestRunner& tests) {
    const auto encoded = minikv::detail::encode_record(
        Record{Operation::Put, "key", "value"});
    for (std::size_t prefix_size = 0; prefix_size < encoded.size();
         ++prefix_size) {
        tests.expect_throws<IncompleteRecordError>(
            [&] {
                static_cast<void>(minikv::detail::decode_record(
                    std::span<const char>(encoded.data(), prefix_size)));
            },
            "decoder rejects every truncated record prefix");
    }
}

void test_invalid_version_and_operation(TestRunner& tests) {
    const auto valid =
        minikv::detail::encode_record(Record{Operation::Put, "key", "value"});

    auto invalid_version = valid;
    invalid_version[version_offset] = '\1';
    tests.expect_throws<InvalidRecordError>(
        [&] {
            static_cast<void>(
                minikv::detail::decode_record(as_span(invalid_version)));
        },
        "decoder rejects an unsupported version");

    auto invalid_operation = valid;
    invalid_operation[operation_offset] =
        std::bit_cast<char>(static_cast<std::uint8_t>(0xFF));
    tests.expect_throws<InvalidRecordError>(
        [&] {
            static_cast<void>(
                minikv::detail::decode_record(as_span(invalid_operation)));
        },
        "decoder rejects an unknown operation");

    tests.expect_throws<InvalidRecordError>(
        [] {
            static_cast<void>(minikv::detail::encode_record(
                Record{static_cast<Operation>(0xFF), "key", "value"}));
        },
        "encoder rejects an unknown operation");
}

void test_checksum_mismatch(TestRunner& tests) {
    const auto valid =
        minikv::detail::encode_record(Record{Operation::Put, "key", "value"});

    auto corrupted_payload = valid;
    corrupted_payload[minikv::detail::record_header_size] ^= 0x01;
    tests.expect_throws<ChecksumMismatchError>(
        [&] {
            static_cast<void>(
                minikv::detail::decode_record(as_span(corrupted_payload)));
        },
        "decoder detects a corrupted payload byte");

    auto corrupted_checksum = valid;
    corrupted_checksum.back() ^= 0x01;
    tests.expect_throws<ChecksumMismatchError>(
        [&] {
            static_cast<void>(
                minikv::detail::decode_record(as_span(corrupted_checksum)));
        },
        "decoder detects a corrupted checksum byte");
}

}  // namespace

int main() {
    TestRunner tests;

    test_exact_version_two_layout(tests);
    test_standard_crc32_vector(tests);
    test_round_trip(tests);
    test_multiple_records(tests);
    test_empty_value(tests);
    test_maximum_lengths(tests);
    test_malformed_records(tests);
    test_truncated_input(tests);
    test_invalid_version_and_operation(tests);
    test_checksum_mismatch(tests);

    return tests.finish();
}
