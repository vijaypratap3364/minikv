#include "record.hpp"
#include "storage_log.hpp"
#include "test_support.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace {

using minikv::detail::AppendResult;
using minikv::detail::Operation;
using minikv::detail::Record;
using minikv::detail::RecordError;
using minikv::detail::StorageError;
using minikv::detail::StorageLog;
using minikv::test::TestRunner;
using minikv::test::TemporaryDirectory;

void test_append_and_offsets(TestRunner& tests,
                             const std::filesystem::path& log_path) {
    const std::vector<Record> expected{
        {Operation::Put, "first", "one"},
        {Operation::Put, "second", "two"},
        {Operation::Put, "third", "three"},
    };
    std::vector<AppendResult> locations;

    {
        StorageLog log(log_path);
        for (const auto& record : expected) {
            locations.push_back(log.append(record));
        }

        tests.expect(minikv::test::read_file(log_path).size() ==
                         locations.back().offset + locations.back().size,
                     "flushed appends are visible while the log is open");
    }

    tests.expect(locations.front().offset == 0,
                 "the first record starts at offset zero");
    for (std::size_t index = 1; index < locations.size(); ++index) {
        tests.expect(locations[index].offset ==
                         locations[index - 1].offset +
                             locations[index - 1].size,
                     "each record begins after the previous record");
    }

    const auto bytes = minikv::test::read_file(log_path);
    const std::span<const char> input(bytes.data(), bytes.size());
    std::size_t offset = 0;
    for (const auto& expected_record : expected) {
        const auto decoded =
            minikv::detail::decode_record(input.subspan(offset));
        tests.expect(decoded.record == expected_record,
                     "storage log preserves appended record order");
        offset += decoded.bytes_consumed;
    }
    tests.expect(offset == bytes.size(),
                 "storage log contains exactly the appended records");
}

void test_reopen_continues_at_end(TestRunner& tests,
                                  const std::filesystem::path& log_path) {
    AppendResult first{};
    {
        StorageLog log(log_path);
        first = log.append(Record{Operation::Put, "first", "one"});
    }

    AppendResult second{};
    {
        StorageLog log(log_path);
        second = log.append(Record{Operation::Put, "second", "two"});
    }

    tests.expect(second.offset == first.offset + first.size,
                 "a reopened log appends after existing bytes");
    tests.expect(std::filesystem::file_size(log_path) ==
                     second.offset + second.size,
                 "reopened append extends rather than overwrites the file");
}

void test_errors_do_not_create_logical_records(
    TestRunner& tests,
    const std::filesystem::path& directory) {
    tests.expect_throws<StorageError>(
        [&] {
            StorageLog log(directory / "missing" / "cannot-open.minikv");
        },
        "opening a log in a missing directory fails explicitly");

    const auto log_path = directory / "invalid-record.minikv";
    StorageLog log(log_path);
    tests.expect_throws<RecordError>(
        [&] {
            static_cast<void>(log.append(Record{
                Operation::Put,
                std::string(minikv::detail::maximum_key_size + 1, 'k'),
                {}}));
        },
        "an invalid record is rejected before append");
    tests.expect(std::filesystem::file_size(log_path) == 0,
                 "a rejected record leaves the log empty");
}

}  // namespace

int main() {
    TestRunner tests;
    TemporaryDirectory temporary_directory;

    test_append_and_offsets(
        tests, temporary_directory.path() / "append.minikv");
    test_reopen_continues_at_end(
        tests, temporary_directory.path() / "reopen.minikv");
    test_errors_do_not_create_logical_records(tests,
                                              temporary_directory.path());

    return tests.finish();
}
