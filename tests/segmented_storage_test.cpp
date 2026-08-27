#include "minikv/minikv.hpp"

#include "record.hpp"
#include "storage_log.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace {

using minikv::test::TestRunner;
using minikv::test::TemporaryDirectory;

[[nodiscard]] minikv::MiniKVOptions small_segments(std::uint64_t size) {
    return minikv::MiniKVOptions{
        minikv::DurabilityMode::Buffered, size};
}

void test_rollover_and_immutable_segments(
    TestRunner& tests,
    const std::filesystem::path& database_path) {
    const minikv::detail::Record first_record{
        minikv::detail::Operation::Put, "A", "1"};
    const auto threshold = static_cast<std::uint64_t>(
        minikv::detail::encode_record(first_record).size());

    minikv::MiniKV store(database_path, small_segments(threshold));
    tests.expect(std::filesystem::is_regular_file(database_path / "CURRENT"),
                 "a database manifest selects the active generation");
    store.put(first_record.key, first_record.value);
    const auto first_segment =
        minikv::test::active_segment_path(database_path);
    const auto immutable_bytes = minikv::test::read_file(first_segment);

    store.put("B", "2");
    store.put("C", "3");
    const auto segments = minikv::test::segment_paths(database_path);
    tests.expect(segments.size() == 3,
                 "the configured size threshold rolls active segments");
    tests.expect(minikv::test::read_file(first_segment) == immutable_bytes,
                 "rollover leaves an older segment byte-for-byte immutable");
    tests.expect(segments.back() ==
                     minikv::test::active_segment_path(database_path),
                 "new writes target the highest numbered active segment");
}

void test_updates_across_segments_survive_restart(
    TestRunner& tests,
    const std::filesystem::path& database_path) {
    const auto threshold = static_cast<std::uint64_t>(
        minikv::detail::encode_record(minikv::detail::Record{
            minikv::detail::Operation::Put, "A", "1"})
            .size());
    const auto options = small_segments(threshold);
    {
        minikv::MiniKV store(database_path, options);
        store.put("A", "1");
        store.put("B", "2");
        store.put("A", "3");
        store.put("A", "4");
        store.put("A", "5");
        tests.expect(minikv::test::segment_paths(database_path).size() == 5,
                     "repeated updates span multiple immutable segments");
    }

    minikv::MiniKV reopened(database_path, options);
    tests.expect(reopened.get("A") == std::optional<std::string>{"5"},
                 "the newest cross-segment update wins after restart");
    tests.expect(reopened.get("B") == std::optional<std::string>{"2"},
                 "an older live record remains addressable by segment");
    tests.expect(reopened.size() == 2,
                 "recovery rebuilds one live entry per cross-segment key");
}

void test_tombstone_across_segments_survives_restart(
    TestRunner& tests,
    const std::filesystem::path& database_path) {
    const auto threshold = static_cast<std::uint64_t>(
        minikv::detail::encode_record(minikv::detail::Record{
            minikv::detail::Operation::Put, "deleted", "value"})
            .size());
    const auto options = small_segments(threshold);
    {
        minikv::MiniKV store(database_path, options);
        store.put("deleted", "value");
        store.put("kept", "value");
        tests.expect(store.erase("deleted"),
                     "a tombstone can be appended after rollover");
        tests.expect(minikv::test::segment_paths(database_path).size() >= 2,
                     "the tombstone workload uses multiple segments");
    }

    minikv::MiniKV reopened(database_path, options);
    tests.expect(!reopened.get("deleted").has_value(),
                 "a cross-segment tombstone keeps the key deleted");
    tests.expect(reopened.get("kept") ==
                     std::optional<std::string>{"value"},
                 "recovery keeps unrelated cross-segment values");
}

void test_oversized_single_record_uses_one_segment(
    TestRunner& tests,
    const std::filesystem::path& database_path) {
    minikv::MiniKV store(database_path, small_segments(1));
    store.put("larger-than-threshold", "still-valid");
    tests.expect(minikv::test::segment_paths(database_path).size() == 1,
                 "one valid record may exceed the rollover target");
    tests.expect(store.get("larger-than-threshold") ==
                     std::optional<std::string>{"still-valid"},
                 "a record larger than the target remains readable");
}

void test_incomplete_immutable_segment_is_not_repaired(
    TestRunner& tests,
    const std::filesystem::path& database_path) {
    auto incomplete = minikv::detail::encode_record(
        minikv::detail::Record{
            minikv::detail::Operation::Put, "first", "value"});
    incomplete.pop_back();
    minikv::test::initialize_database_with_segment(database_path, incomplete);

    const auto generation = minikv::test::current_generation_path(database_path);
    minikv::test::write_file(
        generation / minikv::detail::segment_file_name(2),
        minikv::detail::encode_record(minikv::detail::Record{
            minikv::detail::Operation::Put, "second", "value"}));
    const auto original_size = std::filesystem::file_size(
        generation / minikv::detail::segment_file_name(1));

    tests.expect_throws<minikv::detail::StorageError>(
        [&] { minikv::MiniKV store(database_path); },
        "an incomplete immutable segment is controlled corruption");
    tests.expect(std::filesystem::file_size(
                     generation / minikv::detail::segment_file_name(1)) ==
                     original_size,
                 "recovery never truncates an established immutable segment");
}

void test_invalid_segment_size_is_rejected(
    TestRunner& tests,
    const std::filesystem::path& database_path) {
    tests.expect_throws<minikv::detail::StorageError>(
        [&] {
            minikv::MiniKV store(
                database_path,
                minikv::MiniKVOptions{
                    minikv::DurabilityMode::Buffered, 0});
        },
        "a zero segment rollover size is rejected");
}

}  // namespace

int main() {
    TestRunner tests;
    TemporaryDirectory temporary_directory;

    test_rollover_and_immutable_segments(
        tests, temporary_directory.path() / "rollover.minikv");
    test_updates_across_segments_survive_restart(
        tests, temporary_directory.path() / "updates.minikv");
    test_tombstone_across_segments_survives_restart(
        tests, temporary_directory.path() / "tombstone.minikv");
    test_oversized_single_record_uses_one_segment(
        tests, temporary_directory.path() / "oversized.minikv");
    test_incomplete_immutable_segment_is_not_repaired(
        tests, temporary_directory.path() / "immutable-corruption.minikv");
    test_invalid_segment_size_is_rejected(
        tests, temporary_directory.path() / "invalid-size.minikv");

    return tests.finish();
}
