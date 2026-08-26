#include "minikv/minikv.hpp"
#include "minikv/version.hpp"

#include "record.hpp"
#include "test_support.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using minikv::test::TestRunner;
using minikv::test::TemporaryDirectory;

void test_insert_read_and_overwrite(TestRunner& tests,
                                    const std::filesystem::path& log_path) {
    minikv::MiniKV store(log_path);

    tests.expect(store.empty(), "a new store is empty");
    tests.expect(store.size() == 0, "a new store has size zero");

    store.put("language", "C++20");
    tests.expect(!store.empty(), "put makes the store non-empty");
    tests.expect(store.size() == 1, "put inserts one key");
    tests.expect(store.contains("language"), "contains finds an existing key");
    tests.expect(
        store.get("language") == std::optional<std::string>{"C++20"},
        "get returns the inserted value");

    store.put("language", "modern C++");
    tests.expect(store.size() == 1, "overwrite does not add another key");
    tests.expect(
        store.get("language") == std::optional<std::string>{"modern C++"},
        "overwrite replaces the previous value");
}

void test_missing_and_delete(TestRunner& tests,
                             const std::filesystem::path& log_path) {
    minikv::MiniKV store(log_path);

    tests.expect(!store.contains("missing"), "contains rejects a missing key");
    tests.expect(!store.get("missing").has_value(),
                 "get returns nullopt for a missing key");
    const auto initial_log_size = std::filesystem::file_size(log_path);
    tests.expect(!store.erase("missing"),
                 "erase returns false for a missing key");
    tests.expect(std::filesystem::file_size(log_path) == initial_log_size,
                 "erasing a missing key does not append a record");

    store.put("temporary", "value");
    const auto log_size_before_delete = std::filesystem::file_size(log_path);
    tests.expect(store.erase("temporary"),
                 "erase returns true for an existing key");
    tests.expect(std::filesystem::file_size(log_path) > log_size_before_delete,
                 "erasing an existing key appends a tombstone");
    tests.expect(!store.contains("temporary"),
                 "an erased key is no longer contained");
    tests.expect(!store.get("temporary").has_value(),
                 "get cannot read an erased key");
    tests.expect(store.empty(), "erasing the only key empties the store");
    const auto log_size_after_delete = std::filesystem::file_size(log_path);
    tests.expect(!store.erase("temporary"),
                 "erasing the same key twice returns false");
    tests.expect(std::filesystem::file_size(log_path) == log_size_after_delete,
                 "repeated deletion does not append another tombstone");
}

void test_empty_and_binary_data(TestRunner& tests,
                                const std::filesystem::path& log_path) {
    minikv::MiniKV store(log_path);

    store.put("", "empty key");
    tests.expect(
        store.get("") == std::optional<std::string>{"empty key"},
        "an empty key is valid");

    store.put("empty value", "");
    const auto empty_value = store.get("empty value");
    tests.expect(empty_value.has_value(),
                 "an empty value is distinct from a missing key");
    tests.expect(empty_value == std::optional<std::string>{""},
                 "get preserves an empty value");

    const std::string binary_key{"key\0tail", 8};
    const std::string binary_value{"left\0right", 10};
    store.put(binary_key, binary_value);
    tests.expect(store.contains(binary_key),
                 "keys may contain embedded NUL bytes");
    tests.expect(store.get(binary_key) ==
                     std::optional<std::string>{binary_value},
                 "values preserve embedded NUL bytes");
}

void test_independent_instances(TestRunner& tests,
                                const std::filesystem::path& directory) {
    minikv::MiniKV first(directory / "first.minikv");
    minikv::MiniKV second(directory / "second.minikv");

    first.put("shared name", "first value");
    second.put("shared name", "second value");

    tests.expect(first.get("shared name") ==
                     std::optional<std::string>{"first value"},
                 "the first instance owns its value");
    tests.expect(second.get("shared name") ==
                     std::optional<std::string>{"second value"},
                 "the second instance owns its value");

    tests.expect(first.erase("shared name"),
                 "the first instance can erase its key");
    tests.expect(second.contains("shared name"),
                 "erasing from one instance does not affect another");
}

void test_mutations_are_appended(TestRunner& tests,
                                 const std::filesystem::path& log_path) {
    {
        minikv::MiniKV store(log_path);
        store.put("name", "Vijay");
        store.put("name", "MiniKV");
        tests.expect(store.erase("name"),
                     "persistent delete succeeds after puts");
        tests.expect(!store.contains("name"),
                     "delete removes the current value");
    }

    const auto bytes = minikv::test::read_file(log_path);
    const std::span<const char> input(bytes.data(), bytes.size());
    std::size_t offset = 0;

    const auto first = minikv::detail::decode_record(input.subspan(offset));
    offset += first.bytes_consumed;
    tests.expect(first.record == minikv::detail::Record{
                                     minikv::detail::Operation::Put,
                                     "name",
                                     "Vijay"},
                 "first put is encoded in the log");

    const auto second = minikv::detail::decode_record(input.subspan(offset));
    offset += second.bytes_consumed;
    tests.expect(second.record == minikv::detail::Record{
                                      minikv::detail::Operation::Put,
                                      "name",
                                      "MiniKV"},
                 "overwrite appends another put record");

    const auto third = minikv::detail::decode_record(input.subspan(offset));
    offset += third.bytes_consumed;
    tests.expect(third.record == minikv::detail::Record{
                                     minikv::detail::Operation::Delete,
                                     "name",
                                     {}},
                 "delete appends a tombstone record");

    tests.expect(offset == bytes.size(),
                 "the log contains exactly the three mutations");
}

void test_rejected_put_does_not_change_state(
    TestRunner& tests,
    const std::filesystem::path& log_path) {
    minikv::MiniKV store(log_path);
    const std::string oversized_key(minikv::detail::maximum_key_size + 1, 'k');

    tests.expect_throws<minikv::detail::RecordError>(
        [&] { store.put(oversized_key, "value"); },
        "put reports an encoding failure");
    tests.expect(store.empty(),
                 "a rejected put does not update the in-memory map");
    tests.expect(std::filesystem::file_size(log_path) == 0,
                 "a rejected put does not append log bytes");
}

void test_restart_and_newest_record_wins(
    TestRunner& tests,
    const std::filesystem::path& log_path) {
    {
        minikv::MiniKV store(log_path);
        store.put("A", "1");
        store.put("B", "2");
        store.put("A", "3");
    }

    minikv::MiniKV reopened(log_path);
    tests.expect(reopened.size() == 2,
                 "recovery rebuilds one index entry per key");
    tests.expect(reopened.get("A") == std::optional<std::string>{"3"},
                 "the newest record wins after restart");
    tests.expect(reopened.get("B") == std::optional<std::string>{"2"},
                 "an unchanged key survives restart");
}

void test_many_keys_and_updates_survive_restart(
    TestRunner& tests,
    const std::filesystem::path& log_path) {
    constexpr std::size_t key_count = 256;
    {
        minikv::MiniKV store(log_path);
        for (std::size_t index = 0; index < key_count; ++index) {
            store.put("key:" + std::to_string(index),
                      "value:" + std::to_string(index));
        }
        for (std::size_t index = 0; index < key_count; index += 7) {
            store.put("key:" + std::to_string(index),
                      "updated:" + std::to_string(index));
        }
    }

    minikv::MiniKV reopened(log_path);
    tests.expect(reopened.size() == key_count,
                 "recovery rebuilds all unique keys");
    for (std::size_t index = 0; index < key_count; ++index) {
        const auto expected = index % 7 == 0
                                  ? "updated:" + std::to_string(index)
                                  : "value:" + std::to_string(index);
        tests.expect(reopened.get("key:" + std::to_string(index)) ==
                         std::optional<std::string>{expected},
                     "many-key recovery returns the newest value");
    }
}

void test_empty_and_nonexistent_databases(
    TestRunner& tests,
    const std::filesystem::path& directory) {
    const auto nonexistent = directory / "nonexistent.minikv";
    tests.expect(!std::filesystem::exists(nonexistent),
                 "nonexistent database starts absent");
    {
        minikv::MiniKV store(nonexistent);
        tests.expect(store.empty(),
                     "a nonexistent database opens as an empty store");
    }
    tests.expect(std::filesystem::exists(nonexistent),
                 "opening a nonexistent database creates its log");

    const auto empty = directory / "empty.minikv";
    minikv::test::write_file(empty, {});
    minikv::MiniKV store(empty);
    tests.expect(store.empty(), "an existing empty database recovers cleanly");
}

void test_empty_and_binary_values_survive_restart(
    TestRunner& tests,
    const std::filesystem::path& log_path) {
    const std::string binary_key{"key\0tail", 8};
    const std::string binary_value{"left\0right", 10};
    {
        minikv::MiniKV store(log_path);
        store.put("", "empty key");
        store.put("empty value", "");
        store.put(binary_key, binary_value);
    }

    minikv::MiniKV reopened(log_path);
    tests.expect(reopened.get("") ==
                     std::optional<std::string>{"empty key"},
                 "an empty key survives restart");
    tests.expect(reopened.get("empty value") ==
                     std::optional<std::string>{""},
                 "an empty value survives restart");
    tests.expect(reopened.get(binary_key) ==
                     std::optional<std::string>{binary_value},
                 "binary key and value bytes survive restart");
}

void test_persistent_delete_survives_restart(
    TestRunner& tests,
    const std::filesystem::path& log_path) {
    {
        minikv::MiniKV store(log_path);
        store.put("temporary", "old value");
        store.put("temporary", "new value");
        tests.expect(store.erase("temporary"),
                     "delete removes the updated key");
        tests.expect(!store.get("temporary").has_value(),
                     "deleted key is absent before restart");
    }

    minikv::MiniKV reopened(log_path);
    tests.expect(!reopened.get("temporary").has_value(),
                 "tombstone keeps an updated key deleted after restart");
    tests.expect(!reopened.contains("temporary"),
                 "recovery does not index a tombstoned key");
    tests.expect(reopened.empty(),
                 "recovery excludes deleted keys from the store size");
}

void test_delete_then_reinsert(
    TestRunner& tests,
    const std::filesystem::path& log_path) {
    {
        minikv::MiniKV store(log_path);
        store.put("A", "1");
        tests.expect(store.erase("A"),
                     "a key can be deleted before reinsertion");
        store.put("A", "2");
        tests.expect(store.get("A") == std::optional<std::string>{"2"},
                     "a re-PUT after delete is visible immediately");
    }

    minikv::MiniKV reopened(log_path);
    tests.expect(reopened.get("A") == std::optional<std::string>{"2"},
                 "a re-PUT after tombstone survives restart");
    tests.expect(reopened.size() == 1,
                 "reinsertion rebuilds one live index entry");
}

void test_durability_modes(
    TestRunner& tests,
    const std::filesystem::path& directory) {
    for (const auto mode : {minikv::DurabilityMode::Buffered,
                            minikv::DurabilityMode::Sync}) {
        const auto log_path =
            directory /
            (mode == minikv::DurabilityMode::Buffered
                 ? "buffered-durability.minikv"
                 : "sync-durability.minikv");
        {
            minikv::MiniKV store(log_path, mode);
            store.put("kept", "value");
            store.put("deleted", "value");
            tests.expect(store.erase("deleted"),
                         "each durability mode persists tombstones");
        }

        minikv::MiniKV reopened(log_path, mode);
        tests.expect(reopened.get("kept") ==
                         std::optional<std::string>{"value"},
                     "each durability mode recovers PUT records");
        tests.expect(!reopened.get("deleted").has_value(),
                     "each durability mode recovers DELETE records");
    }
}

void test_torn_tail_recovery(
    TestRunner& tests,
    const std::filesystem::path& directory) {
    const auto first = minikv::detail::encode_record(
        minikv::detail::Record{minikv::detail::Operation::Put, "A", "1"});
    const auto second = minikv::detail::encode_record(
        minikv::detail::Record{minikv::detail::Operation::Put, "B", "2"});
    const std::string torn_key{"candidate-key"};
    const std::string torn_value{"candidate-value"};
    const auto candidate = minikv::detail::encode_record(
        minikv::detail::Record{
            minikv::detail::Operation::Put, torn_key, torn_value});

    std::vector<char> valid_prefix = first;
    valid_prefix.insert(valid_prefix.end(), second.begin(), second.end());
    const auto valid_prefix_size = valid_prefix.size();

    struct TornCase {
        const char* name;
        std::size_t prefix_size;
        minikv::DurabilityMode mode;
    };
    const std::vector<TornCase> cases{
        {"partial-header", 7, minikv::DurabilityMode::Buffered},
        {"complete-header", minikv::detail::record_header_size,
         minikv::DurabilityMode::Buffered},
        {"partial-key", minikv::detail::record_header_size + 3,
         minikv::DurabilityMode::Buffered},
        {"partial-value",
         minikv::detail::record_header_size + torn_key.size() + 4,
         minikv::DurabilityMode::Buffered},
        {"before-checksum",
         candidate.size() - minikv::detail::record_checksum_size,
         minikv::DurabilityMode::Sync},
    };

    for (const auto& fault : cases) {
        auto bytes = valid_prefix;
        bytes.insert(bytes.end(),
                     candidate.begin(),
                     candidate.begin() +
                         static_cast<std::ptrdiff_t>(fault.prefix_size));
        const auto log_path =
            directory / (std::string(fault.name) + ".minikv");
        minikv::test::write_file(log_path, bytes);

        {
            minikv::MiniKV recovered(log_path, fault.mode);
            tests.expect(recovered.get("A") ==
                             std::optional<std::string>{"1"},
                         "torn-tail recovery preserves the first record");
            tests.expect(recovered.get("B") ==
                             std::optional<std::string>{"2"},
                         "torn-tail recovery preserves the second record");
            tests.expect(!recovered.get(torn_key).has_value(),
                         "torn-tail recovery does not expose a partial record");
            tests.expect(std::filesystem::file_size(log_path) ==
                             valid_prefix_size,
                         "torn-tail recovery truncates to the verified offset");
            recovered.put("after-recovery", fault.name);
        }

        minikv::MiniKV reopened(log_path, fault.mode);
        tests.expect(reopened.get("A") ==
                         std::optional<std::string>{"1"} &&
                         reopened.get("B") ==
                             std::optional<std::string>{"2"},
                     "earlier records remain recoverable after repair and append");
        tests.expect(reopened.get("after-recovery") ==
                         std::optional<std::string>{fault.name},
                     "the repaired log accepts and recovers a new append");
    }

    auto complete_bytes = valid_prefix;
    complete_bytes.insert(
        complete_bytes.end(), candidate.begin(), candidate.end());
    const auto complete_path = directory / "complete-record.minikv";
    minikv::test::write_file(complete_path, complete_bytes);

    minikv::MiniKV complete(complete_path);
    tests.expect(complete.get(torn_key) ==
                     std::optional<std::string>{torn_value},
                 "a complete checksum-valid final record is recovered");
    tests.expect(std::filesystem::file_size(complete_path) ==
                     complete_bytes.size(),
                 "recovery does not truncate a complete final record");
}

void test_corruption_fails_recovery(
    TestRunner& tests,
    const std::filesystem::path& directory) {
    auto invalid_magic = minikv::detail::encode_record(
        minikv::detail::Record{minikv::detail::Operation::Put, "key", "value"});
    invalid_magic[0] = 'X';
    const auto invalid_magic_path = directory / "invalid-magic.minikv";
    minikv::test::write_file(invalid_magic_path, invalid_magic);
    tests.expect_throws<minikv::detail::InvalidRecordError>(
        [&] { minikv::MiniKV store(invalid_magic_path); },
        "recovery distinguishes invalid record format");

    auto valid_prefix = minikv::detail::encode_record(
        minikv::detail::Record{minikv::detail::Operation::Put, "first", "one"});
    auto checksum_mismatch = minikv::detail::encode_record(
        minikv::detail::Record{minikv::detail::Operation::Put, "second", "two"});
    checksum_mismatch[minikv::detail::record_header_size] ^= 0x01;
    auto valid_suffix = minikv::detail::encode_record(
        minikv::detail::Record{minikv::detail::Operation::Put, "third", "three"});
    valid_prefix.insert(
        valid_prefix.end(), checksum_mismatch.begin(), checksum_mismatch.end());
    valid_prefix.insert(
        valid_prefix.end(), valid_suffix.begin(), valid_suffix.end());
    const auto checksum_path = directory / "checksum-mismatch.minikv";
    minikv::test::write_file(checksum_path, valid_prefix);
    const auto corrupt_file_size = std::filesystem::file_size(checksum_path);
    tests.expect_throws<minikv::detail::ChecksumMismatchError>(
        [&] { minikv::MiniKV store(checksum_path); },
        "recovery rejects checksum corruption between valid records");
    tests.expect(std::filesystem::file_size(checksum_path) ==
                     corrupt_file_size,
                 "recovery never truncates complete corrupted data");
}

}  // namespace

int main() {
    TestRunner tests;
    TemporaryDirectory temporary_directory;

    tests.expect(minikv::version() == "0.1.0-dev",
                 "the library exposes its version");
    test_insert_read_and_overwrite(
        tests, temporary_directory.path() / "insert.minikv");
    test_missing_and_delete(
        tests, temporary_directory.path() / "delete.minikv");
    test_empty_and_binary_data(
        tests, temporary_directory.path() / "binary.minikv");
    test_independent_instances(tests, temporary_directory.path());
    test_mutations_are_appended(
        tests, temporary_directory.path() / "mutations.minikv");
    test_rejected_put_does_not_change_state(
        tests, temporary_directory.path() / "rejected.minikv");
    test_restart_and_newest_record_wins(
        tests, temporary_directory.path() / "restart.minikv");
    test_many_keys_and_updates_survive_restart(
        tests, temporary_directory.path() / "many-keys.minikv");
    test_empty_and_nonexistent_databases(tests, temporary_directory.path());
    test_empty_and_binary_values_survive_restart(
        tests, temporary_directory.path() / "binary-restart.minikv");
    test_persistent_delete_survives_restart(
        tests, temporary_directory.path() / "delete-restart.minikv");
    test_delete_then_reinsert(
        tests, temporary_directory.path() / "delete-reinsert.minikv");
    test_durability_modes(tests, temporary_directory.path());
    test_torn_tail_recovery(tests, temporary_directory.path());
    test_corruption_fails_recovery(tests, temporary_directory.path());

    return tests.finish();
}
