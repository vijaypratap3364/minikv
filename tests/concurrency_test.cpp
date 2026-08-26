#include "minikv/minikv.hpp"

#include "test_support.hpp"

#include <atomic>
#include <barrier>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using minikv::test::TestRunner;
using minikv::test::TemporaryDirectory;

void join_all(std::vector<std::thread>& threads) {
    for (auto& thread : threads) {
        thread.join();
    }
}

void test_many_concurrent_gets(TestRunner& tests,
                               const std::filesystem::path& log_path) {
    constexpr std::size_t key_count = 64;
    constexpr std::size_t reader_count = 8;
    constexpr std::size_t rounds = 20;

    minikv::MiniKV store(log_path);
    for (std::size_t key = 0; key < key_count; ++key) {
        store.put("read:" + std::to_string(key),
                  "value:" + std::to_string(key));
    }

    std::barrier start(static_cast<std::ptrdiff_t>(reader_count + 1));
    std::atomic<bool> reads_are_correct{true};
    std::vector<std::thread> readers;
    readers.reserve(reader_count);
    for (std::size_t reader = 0; reader < reader_count; ++reader) {
        readers.emplace_back([&] {
            start.arrive_and_wait();
            try {
                for (std::size_t round = 0; round < rounds; ++round) {
                    for (std::size_t key = 0; key < key_count; ++key) {
                        const auto expected =
                            "value:" + std::to_string(key);
                        if (store.get("read:" + std::to_string(key)) !=
                            std::optional<std::string>{expected}) {
                            reads_are_correct.store(
                                false, std::memory_order_relaxed);
                        }
                    }
                }
            } catch (...) {
                reads_are_correct.store(false, std::memory_order_relaxed);
            }
        });
    }

    start.arrive_and_wait();
    join_all(readers);
    tests.expect(reads_are_correct.load(std::memory_order_relaxed),
                 "many concurrent GETs return intact values");
}

void test_concurrent_puts_to_different_keys(
    TestRunner& tests,
    const std::filesystem::path& log_path) {
    constexpr std::size_t writer_count = 8;
    constexpr std::size_t keys_per_writer = 32;

    minikv::MiniKV store(log_path);
    std::barrier start(static_cast<std::ptrdiff_t>(writer_count + 1));
    std::atomic<bool> writes_succeeded{true};
    std::vector<std::thread> writers;
    writers.reserve(writer_count);
    for (std::size_t writer = 0; writer < writer_count; ++writer) {
        writers.emplace_back([&, writer] {
            start.arrive_and_wait();
            try {
                for (std::size_t item = 0; item < keys_per_writer; ++item) {
                    const auto suffix = std::to_string(writer) + ":" +
                                        std::to_string(item);
                    store.put("different:" + suffix, "value:" + suffix);
                }
            } catch (...) {
                writes_succeeded.store(false, std::memory_order_relaxed);
            }
        });
    }

    start.arrive_and_wait();
    join_all(writers);
    tests.expect(writes_succeeded.load(std::memory_order_relaxed),
                 "concurrent PUTs to different keys complete");
    tests.expect(store.size() == writer_count * keys_per_writer,
                 "concurrent PUTs retain every distinct key");

    bool all_values_match = true;
    for (std::size_t writer = 0; writer < writer_count; ++writer) {
        for (std::size_t item = 0; item < keys_per_writer; ++item) {
            const auto suffix =
                std::to_string(writer) + ":" + std::to_string(item);
            if (store.get("different:" + suffix) !=
                std::optional<std::string>{"value:" + suffix}) {
                all_values_match = false;
            }
        }
    }
    tests.expect(all_values_match,
                 "concurrent distinct-key PUT values remain readable");
}

void test_concurrent_puts_to_same_key_and_restart(
    TestRunner& tests,
    const std::filesystem::path& log_path) {
    constexpr std::size_t writer_count = 8;
    constexpr std::size_t writes_per_thread = 40;
    std::optional<std::string> final_value;

    {
        minikv::MiniKV store(log_path);
        std::barrier start(static_cast<std::ptrdiff_t>(writer_count + 1));
        std::atomic<bool> writes_succeeded{true};
        std::vector<std::thread> writers;
        writers.reserve(writer_count);
        for (std::size_t writer = 0; writer < writer_count; ++writer) {
            writers.emplace_back([&, writer] {
                start.arrive_and_wait();
                try {
                    for (std::size_t write = 0;
                         write < writes_per_thread;
                         ++write) {
                        store.put("contended",
                                  std::to_string(writer) + ":" +
                                      std::to_string(write));
                    }
                } catch (...) {
                    writes_succeeded.store(false, std::memory_order_relaxed);
                }
            });
        }

        start.arrive_and_wait();
        join_all(writers);
        tests.expect(writes_succeeded.load(std::memory_order_relaxed),
                     "concurrent PUTs to one key complete");
        final_value = store.get("contended");

        bool value_came_from_a_writer = false;
        if (final_value) {
            for (std::size_t writer = 0;
                 writer < writer_count && !value_came_from_a_writer;
                 ++writer) {
                for (std::size_t write = 0;
                     write < writes_per_thread;
                     ++write) {
                    value_came_from_a_writer =
                        *final_value == std::to_string(writer) + ":" +
                                            std::to_string(write);
                    if (value_came_from_a_writer) {
                        break;
                    }
                }
            }
        }
        tests.expect(value_came_from_a_writer,
                     "same-key PUT contention leaves one complete value");
    }

    minikv::MiniKV reopened(log_path);
    tests.expect(reopened.get("contended") == final_value,
                 "the same-key winner survives restart");
}

void test_get_while_put_occurs(TestRunner& tests,
                               const std::filesystem::path& log_path) {
    constexpr std::size_t reader_count = 4;
    constexpr std::size_t rounds = 40;
    minikv::MiniKV store(log_path);
    store.put("changing", "0");

    std::barrier phase_start(
        static_cast<std::ptrdiff_t>(reader_count + 1));
    std::barrier phase_end(static_cast<std::ptrdiff_t>(reader_count + 1));
    std::atomic<bool> observations_are_valid{true};
    std::vector<std::thread> threads;
    threads.reserve(reader_count + 1);

    threads.emplace_back([&] {
        for (std::size_t round = 1; round <= rounds; ++round) {
            phase_start.arrive_and_wait();
            try {
                store.put("changing", std::to_string(round));
            } catch (...) {
                observations_are_valid.store(false, std::memory_order_relaxed);
            }
            phase_end.arrive_and_wait();
        }
    });
    for (std::size_t reader = 0; reader < reader_count; ++reader) {
        threads.emplace_back([&] {
            for (std::size_t round = 1; round <= rounds; ++round) {
                phase_start.arrive_and_wait();
                try {
                    const auto observed = store.get("changing");
                    const auto before = std::to_string(round - 1);
                    const auto after = std::to_string(round);
                    if (!observed ||
                        (*observed != before && *observed != after)) {
                        observations_are_valid.store(
                            false, std::memory_order_relaxed);
                    }
                } catch (...) {
                    observations_are_valid.store(
                        false, std::memory_order_relaxed);
                }
                phase_end.arrive_and_wait();
            }
        });
    }

    join_all(threads);
    tests.expect(observations_are_valid.load(std::memory_order_relaxed),
                 "GET concurrent with PUT sees an old or new complete value");
    tests.expect(store.get("changing") ==
                     std::optional<std::string>{std::to_string(rounds)},
                 "the final concurrent PUT is visible");
}

void test_delete_while_get_occurs(TestRunner& tests,
                                  const std::filesystem::path& log_path) {
    constexpr std::size_t reader_count = 4;
    constexpr std::size_t rounds = 40;
    minikv::MiniKV store(log_path);
    store.put("toggle", "stable");

    std::barrier phase_start(
        static_cast<std::ptrdiff_t>(reader_count + 1));
    std::barrier phase_end(static_cast<std::ptrdiff_t>(reader_count + 1));
    std::atomic<bool> observations_are_valid{true};
    std::vector<std::thread> threads;
    threads.reserve(reader_count + 1);

    threads.emplace_back([&] {
        for (std::size_t round = 1; round <= rounds; ++round) {
            phase_start.arrive_and_wait();
            try {
                if (round % 2 == 0) {
                    store.put("toggle", "stable");
                } else {
                    static_cast<void>(store.erase("toggle"));
                }
            } catch (...) {
                observations_are_valid.store(false, std::memory_order_relaxed);
            }
            phase_end.arrive_and_wait();
        }
    });
    for (std::size_t reader = 0; reader < reader_count; ++reader) {
        threads.emplace_back([&] {
            for (std::size_t round = 1; round <= rounds; ++round) {
                phase_start.arrive_and_wait();
                try {
                    const auto observed = store.get("toggle");
                    if (observed && *observed != "stable") {
                        observations_are_valid.store(
                            false, std::memory_order_relaxed);
                    }
                } catch (...) {
                    observations_are_valid.store(
                        false, std::memory_order_relaxed);
                }
                phase_end.arrive_and_wait();
            }
        });
    }

    join_all(threads);
    tests.expect(observations_are_valid.load(std::memory_order_relaxed),
                 "GET concurrent with DELETE sees value or not-found");
    tests.expect(store.get("toggle") ==
                     std::optional<std::string>{"stable"},
                 "the final toggle PUT is visible");
}

void test_put_delete_contention_survives_restart(
    TestRunner& tests,
    const std::filesystem::path& log_path) {
    constexpr std::size_t worker_count = 8;
    constexpr std::size_t key_count = 6;
    constexpr std::size_t operations_per_worker = 60;
    std::vector<std::optional<std::string>> final_values(key_count);

    {
        minikv::MiniKV store(log_path);
        std::barrier start(static_cast<std::ptrdiff_t>(worker_count + 1));
        std::atomic<bool> operations_succeeded{true};
        std::vector<std::thread> workers;
        workers.reserve(worker_count);
        for (std::size_t worker = 0; worker < worker_count; ++worker) {
            workers.emplace_back([&, worker] {
                start.arrive_and_wait();
                try {
                    for (std::size_t operation = 0;
                         operation < operations_per_worker;
                         ++operation) {
                        const auto key = "mixed:" +
                                         std::to_string(
                                             (worker + operation) % key_count);
                        if ((worker + operation) % 3 == 0) {
                            static_cast<void>(store.erase(key));
                        } else {
                            store.put(key,
                                      std::to_string(worker) + ":" +
                                          std::to_string(operation));
                        }
                    }
                } catch (...) {
                    operations_succeeded.store(
                        false, std::memory_order_relaxed);
                }
            });
        }

        start.arrive_and_wait();
        join_all(workers);
        tests.expect(operations_succeeded.load(std::memory_order_relaxed),
                     "PUT and DELETE contention completes");
        for (std::size_t key = 0; key < key_count; ++key) {
            final_values[key] = store.get("mixed:" + std::to_string(key));
        }
    }

    minikv::MiniKV reopened(log_path);
    bool restart_matches = true;
    for (std::size_t key = 0; key < key_count; ++key) {
        if (reopened.get("mixed:" + std::to_string(key)) !=
            final_values[key]) {
            restart_matches = false;
        }
    }
    tests.expect(restart_matches,
                 "restart reproduces the state after concurrent mutations");
}

}  // namespace

int main() {
    TestRunner tests;
    TemporaryDirectory temporary_directory;

    test_many_concurrent_gets(
        tests, temporary_directory.path() / "concurrent-gets.minikv");
    test_concurrent_puts_to_different_keys(
        tests, temporary_directory.path() / "different-keys.minikv");
    test_concurrent_puts_to_same_key_and_restart(
        tests, temporary_directory.path() / "same-key.minikv");
    test_get_while_put_occurs(
        tests, temporary_directory.path() / "get-put.minikv");
    test_delete_while_get_occurs(
        tests, temporary_directory.path() / "delete-get.minikv");
    test_put_delete_contention_survives_restart(
        tests, temporary_directory.path() / "put-delete.minikv");

    return tests.finish();
}
