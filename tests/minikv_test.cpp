#include "minikv/minikv.hpp"
#include "minikv/version.hpp"

#include "record.hpp"
#include "test_support.hpp"

#include <filesystem>
#include <optional>
#include <span>
#include <string>

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
    tests.expect(std::filesystem::file_size(log_path) == log_size_before_delete,
                 "erasing an existing key remains in-memory only");
    tests.expect(!store.contains("temporary"),
                 "an erased key is no longer contained");
    tests.expect(!store.get("temporary").has_value(),
                 "get cannot read an erased key");
    tests.expect(store.empty(), "erasing the only key empties the store");
    tests.expect(!store.erase("temporary"),
                 "erasing the same key twice returns false");
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

void test_only_puts_are_appended(TestRunner& tests,
                                const std::filesystem::path& log_path) {
    {
        minikv::MiniKV store(log_path);
        store.put("name", "Vijay");
        store.put("name", "MiniKV");
        tests.expect(store.erase("name"),
                     "in-memory delete succeeds after persistent puts");
        tests.expect(!store.contains("name"),
                     "in-memory delete removes the current value");
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

    tests.expect(offset == bytes.size(),
                 "delete appends no third record during Stage 2");
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
    test_only_puts_are_appended(
        tests, temporary_directory.path() / "mutations.minikv");
    test_rejected_put_does_not_change_state(
        tests, temporary_directory.path() / "rejected.minikv");

    return tests.finish();
}
