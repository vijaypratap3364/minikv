#include "minikv/minikv.hpp"

#include "record.hpp"
#include "segmented_storage.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using minikv::test::TestRunner;
using minikv::test::TemporaryDirectory;

constexpr std::uint64_t compact_segment_target = 256;

class SimulatedCompactionInterruption : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] minikv::MiniKVOptions compact_options() {
    return minikv::MiniKVOptions{
        minikv::DurabilityMode::Buffered, compact_segment_target};
}

void test_compaction_preserves_state_and_reclaims_space(
    TestRunner& tests,
    const std::filesystem::path& database_path) {
    const auto options = compact_options();
    const std::string kept_value(512, 'K');
    std::string newest_hot_value;
    std::uintmax_t before_size = 0;
    std::uintmax_t after_size = 0;

    {
        minikv::MiniKV store(database_path, options);
        for (std::size_t version = 0; version < 50; ++version) {
            newest_hot_value =
                std::string(512, static_cast<char>('a' + version % 26));
            store.put("hot", newest_hot_value);
        }
        store.put("kept", kept_value);
        store.put("deleted", std::string(512, 'D'));
        tests.expect(store.erase("deleted"),
                     "the overwrite-heavy fixture ends with a tombstone");

        before_size = minikv::test::database_data_size(database_path);
        const auto before_segments =
            minikv::test::segment_paths(database_path).size();
        store.compact();
        after_size = minikv::test::database_data_size(database_path);

        const auto expected_after =
            minikv::detail::encode_record(minikv::detail::Record{
                minikv::detail::Operation::Put, "hot", newest_hot_value})
                .size() +
            minikv::detail::encode_record(minikv::detail::Record{
                minikv::detail::Operation::Put, "kept", kept_value})
                .size();
        tests.expect(after_size == expected_after,
                     "compaction writes exactly the two newest live PUTs");
        tests.expect(after_size < before_size,
                     "measured segment bytes decrease after compaction");
        tests.expect(minikv::test::segment_paths(database_path).size() <
                         before_segments,
                     "compaction reduces the measured segment count");
        tests.expect(store.get("hot") ==
                         std::optional<std::string>{newest_hot_value},
                     "compaction preserves the newest overwritten value");
        tests.expect(store.get("kept") ==
                         std::optional<std::string>{kept_value},
                     "compaction preserves an unrelated live value");
        tests.expect(!store.get("deleted").has_value(),
                     "compaction does not resurrect a deleted value");
        tests.expect(minikv::test::generation_paths(database_path).size() == 1,
                     "successful compaction removes the obsolete generation");
    }

    std::cout << "Measured overwrite-heavy segment bytes: " << before_size
              << " before, " << after_size << " after\n";

    minikv::MiniKV reopened(database_path, options);
    tests.expect(reopened.get("hot") ==
                     std::optional<std::string>{newest_hot_value},
                 "the newest value survives restart after compaction");
    tests.expect(reopened.get("kept") ==
                     std::optional<std::string>{kept_value},
                 "an unrelated value survives restart after compaction");
    tests.expect(!reopened.get("deleted").has_value(),
                 "a deleted value stays deleted after compacted restart");
}

void test_empty_compaction_discards_obsolete_tombstone(
    TestRunner& tests,
    const std::filesystem::path& database_path) {
    const auto options = compact_options();
    {
        minikv::MiniKV store(database_path, options);
        store.put("gone", "value");
        tests.expect(store.erase("gone"),
                     "the empty compaction fixture deletes its only key");
        tests.expect(minikv::test::database_data_size(database_path) > 0,
                     "PUT and tombstone occupy bytes before compaction");
        store.compact();
        tests.expect(minikv::test::database_data_size(database_path) == 0,
                     "an empty live set needs no tombstone or value bytes");
        tests.expect(minikv::test::segment_paths(database_path).size() == 1,
                     "an empty compacted generation retains one active segment");
    }

    minikv::MiniKV reopened(database_path, options);
    tests.expect(reopened.empty(),
                 "a tombstone-free empty generation recovers as empty");
}

void test_writes_continue_after_compaction(
    TestRunner& tests,
    const std::filesystem::path& database_path) {
    const auto options = compact_options();
    {
        minikv::MiniKV store(database_path, options);
        store.put("A", "1");
        store.put("A", "2");
        store.compact();
        store.put("B", "3");
        tests.expect(store.erase("A"),
                     "DELETE can append after a generation switch");
        store.put("A", "4");
    }

    minikv::MiniKV reopened(database_path, options);
    tests.expect(reopened.get("A") == std::optional<std::string>{"4"},
                 "reinserted data after compaction survives restart");
    tests.expect(reopened.get("B") == std::optional<std::string>{"3"},
                 "new data after compaction survives restart");
}

void test_sync_mode_compaction(
    TestRunner& tests,
    const std::filesystem::path& database_path) {
    const minikv::MiniKVOptions options{
        minikv::DurabilityMode::Sync, compact_segment_target};
    {
        minikv::MiniKV store(database_path, options);
        store.put("durable", "old");
        store.put("durable", "new");
        store.compact();
        tests.expect(store.get("durable") ==
                         std::optional<std::string>{"new"},
                     "Sync compaction switches to the newest live value");
    }

    minikv::MiniKV reopened(database_path, options);
    tests.expect(reopened.get("durable") ==
                     std::optional<std::string>{"new"},
                 "Sync compaction survives a durable-mode restart");
}

[[nodiscard]] std::string_view phase_name(
    minikv::detail::CompactionPhase phase) {
    switch (phase) {
        case minikv::detail::CompactionPhase::TemporaryGenerationComplete:
            return "temporary-output";
        case minikv::detail::CompactionPhase::GenerationInstalled:
            return "generation-installed";
        case minikv::detail::CompactionPhase::ManifestCommitted:
            return "manifest-committed";
    }
    return "unknown";
}

void test_interrupted_compaction_at_phase(
    TestRunner& tests,
    const std::filesystem::path& database_path,
    minikv::detail::CompactionPhase target_phase) {
    const auto options = compact_options();
    {
        minikv::MiniKV store(database_path, options);
        store.put("A", "old");
        store.put("A", "new");
        store.put("B", "kept");
        store.put("deleted", "value");
        static_cast<void>(store.erase("deleted"));
    }

    const auto original_generation =
        minikv::test::current_generation_path(database_path);
    const auto original_generation_id =
        minikv::test::current_generation_id(database_path);
    const std::vector<minikv::detail::Record> live_records{
        {minikv::detail::Operation::Put, "A", "new"},
        {minikv::detail::Operation::Put, "B", "kept"},
    };
    {
        minikv::detail::SegmentedStorage storage(
            database_path,
            options.durability_mode,
            options.maximum_segment_size);
        tests.expect_throws<SimulatedCompactionInterruption>(
            [&] {
                static_cast<void>(storage.compact(
                    live_records,
                    [target_phase](minikv::detail::CompactionPhase phase) {
                        if (phase == target_phase) {
                            throw SimulatedCompactionInterruption(
                                "simulated compaction interruption");
                        }
                    }));
            },
            "fault injection interrupts compaction at a named boundary");
    }

    tests.expect(std::filesystem::is_directory(original_generation),
                 "interrupted compaction has not deleted the old generation");
    const auto selected_generation =
        minikv::test::current_generation_id(database_path);
    tests.expect(
        target_phase == minikv::detail::CompactionPhase::ManifestCommitted
            ? selected_generation == original_generation_id + 1
            : selected_generation == original_generation_id,
        "CURRENT changes only at the compaction commit boundary");

    minikv::MiniKV reopened(database_path, options);
    tests.expect(reopened.get("A") == std::optional<std::string>{"new"},
                 "interrupted compaction preserves the newest value");
    tests.expect(reopened.get("B") == std::optional<std::string>{"kept"},
                 "interrupted compaction preserves other live values");
    tests.expect(!reopened.get("deleted").has_value(),
                 "interrupted compaction preserves logical deletion");
    tests.expect(minikv::test::generation_paths(database_path).size() == 1,
                 "restart cleans generations not selected by CURRENT");
    std::cout << "Recovered simulated interruption at "
              << phase_name(target_phase) << '\n';
}

void test_interrupted_compaction(TestRunner& tests,
                                 const std::filesystem::path& directory) {
    for (const auto phase : {
             minikv::detail::CompactionPhase::TemporaryGenerationComplete,
             minikv::detail::CompactionPhase::GenerationInstalled,
             minikv::detail::CompactionPhase::ManifestCommitted}) {
        test_interrupted_compaction_at_phase(
            tests,
            directory / (std::string(phase_name(phase)) + ".minikv"),
            phase);
    }
}

}  // namespace

int main() {
    TestRunner tests;
    TemporaryDirectory temporary_directory;

    test_compaction_preserves_state_and_reclaims_space(
        tests, temporary_directory.path() / "reclaim.minikv");
    test_empty_compaction_discards_obsolete_tombstone(
        tests, temporary_directory.path() / "empty.minikv");
    test_writes_continue_after_compaction(
        tests, temporary_directory.path() / "continue.minikv");
    test_sync_mode_compaction(
        tests, temporary_directory.path() / "sync.minikv");
    test_interrupted_compaction(tests, temporary_directory.path());

    return tests.finish();
}
