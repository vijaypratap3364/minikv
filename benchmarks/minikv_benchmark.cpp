#include "minikv/minikv.hpp"

#include <algorithm>
#include <barrier>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <ctime>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;

constexpr std::size_t default_key_count = 10'000;
constexpr std::size_t default_value_size = 256;
constexpr std::size_t default_operation_count = 20'000;
constexpr std::uint64_t default_segment_size = 64U * 1024U * 1024U;
constexpr std::uint64_t default_seed = 0x4D494E494B56ULL;

struct Configuration {
    std::string workload;
    std::string distribution{"uniform"};
    std::string label{"unlabelled"};
    std::size_t key_count{default_key_count};
    std::size_t value_size{default_value_size};
    std::size_t operation_count{default_operation_count};
    std::size_t reader_threads{1};
    std::size_t writer_threads{1};
    std::uint64_t seed{default_seed};
    std::uint64_t segment_size{default_segment_size};
    minikv::DurabilityMode durability{minikv::DurabilityMode::Buffered};
    std::optional<std::filesystem::path> database_path;
    std::optional<std::filesystem::path> output_path;
    bool keep_database{false};
};

struct BenchmarkResult {
    std::uint64_t operations{0};
    Nanoseconds elapsed{0};
    std::vector<std::uint64_t> latencies_ns;
    std::uintmax_t file_size_before{0};
    std::uintmax_t file_size_after{0};
    double recovery_ms{0.0};
    double compaction_ms{0.0};
};

[[nodiscard]] std::string usage() {
    return R"(Usage: minikv_benchmark --workload NAME [options]

Workloads:
  sequential-put, random-get, update-heavy, mixed, delete-heavy,
  recovery, compaction, concurrent

Options:
  --keys N              Number of keys in the working set
  --value-size N        Value bytes per PUT
  --operations N        Timed logical operations
  --readers N           Reader threads for concurrent
  --writers N           Writer threads for concurrent
  --distribution NAME   uniform or hot (90% in 1% of keys)
  --seed N              Deterministic unsigned random seed
  --segment-size N      Segment rollover target in bytes
  --durability MODE     buffered or sync
  --database PATH       New database path (must not already exist)
  --keep-database       Do not remove the benchmark database
  --output PATH         Append a machine-readable CSV row
  --label TEXT          Label recorded in CSV, such as before or after
  --help                 Show this text
)";
}

template <typename Integer>
[[nodiscard]] Integer parse_unsigned(std::string_view text,
                                     std::string_view option) {
    if (text.empty() || text.front() == '-') {
        throw std::invalid_argument(std::string(option) +
                                    " expects an unsigned integer");
    }
    std::size_t consumed = 0;
    unsigned long long parsed = 0;
    try {
        parsed = std::stoull(std::string(text), &consumed, 10);
    } catch (const std::exception&) {
        throw std::invalid_argument(std::string(option) +
                                    " expects an unsigned integer");
    }
    if (consumed != text.size() ||
        parsed > static_cast<unsigned long long>(
                     std::numeric_limits<Integer>::max())) {
        throw std::invalid_argument(std::string(option) +
                                    " is outside the supported range");
    }
    return static_cast<Integer>(parsed);
}

[[nodiscard]] std::string_view require_value(int argc,
                                             char* argv[],
                                             int& index) {
    if (index + 1 >= argc) {
        throw std::invalid_argument(std::string(argv[index]) +
                                    " requires a value");
    }
    ++index;
    return argv[index];
}

[[nodiscard]] Configuration parse_arguments(int argc, char* argv[]) {
    Configuration configuration;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option = argv[index];
        if (option == "--help") {
            std::cout << usage();
            std::exit(0);
        }
        if (option == "--workload") {
            configuration.workload = require_value(argc, argv, index);
        } else if (option == "--keys") {
            configuration.key_count = parse_unsigned<std::size_t>(
                require_value(argc, argv, index), option);
        } else if (option == "--value-size") {
            configuration.value_size = parse_unsigned<std::size_t>(
                require_value(argc, argv, index), option);
        } else if (option == "--operations") {
            configuration.operation_count = parse_unsigned<std::size_t>(
                require_value(argc, argv, index), option);
        } else if (option == "--readers") {
            configuration.reader_threads = parse_unsigned<std::size_t>(
                require_value(argc, argv, index), option);
        } else if (option == "--writers") {
            configuration.writer_threads = parse_unsigned<std::size_t>(
                require_value(argc, argv, index), option);
        } else if (option == "--seed") {
            configuration.seed = parse_unsigned<std::uint64_t>(
                require_value(argc, argv, index), option);
        } else if (option == "--segment-size") {
            configuration.segment_size = parse_unsigned<std::uint64_t>(
                require_value(argc, argv, index), option);
        } else if (option == "--distribution") {
            configuration.distribution = require_value(argc, argv, index);
        } else if (option == "--durability") {
            const auto mode = require_value(argc, argv, index);
            if (mode == "buffered") {
                configuration.durability = minikv::DurabilityMode::Buffered;
            } else if (mode == "sync") {
                configuration.durability = minikv::DurabilityMode::Sync;
            } else {
                throw std::invalid_argument(
                    "--durability must be buffered or sync");
            }
        } else if (option == "--database") {
            configuration.database_path =
                std::filesystem::path(require_value(argc, argv, index));
        } else if (option == "--output") {
            configuration.output_path =
                std::filesystem::path(require_value(argc, argv, index));
        } else if (option == "--label") {
            configuration.label = require_value(argc, argv, index);
        } else if (option == "--keep-database") {
            configuration.keep_database = true;
        } else {
            throw std::invalid_argument("unknown option: " +
                                        std::string(option));
        }
    }

    const std::vector<std::string_view> workloads{
        "sequential-put", "random-get",  "update-heavy", "mixed",
        "delete-heavy",  "recovery",    "compaction",   "concurrent"};
    if (std::ranges::find(workloads, configuration.workload) ==
        workloads.end()) {
        throw std::invalid_argument("--workload must name a listed workload");
    }
    if (configuration.key_count == 0) {
        throw std::invalid_argument("--keys must be greater than zero");
    }
    if (configuration.operation_count == 0) {
        throw std::invalid_argument("--operations must be greater than zero");
    }
    if (configuration.segment_size == 0) {
        throw std::invalid_argument("--segment-size must be greater than zero");
    }
    if (configuration.value_size > 4U * 1024U * 1024U) {
        throw std::invalid_argument("--value-size exceeds MiniKV's 4 MiB limit");
    }
    if (configuration.distribution != "uniform" &&
        configuration.distribution != "hot") {
        throw std::invalid_argument(
            "--distribution must be uniform or hot");
    }
    if (configuration.workload == "concurrent" &&
        configuration.reader_threads + configuration.writer_threads == 0) {
        throw std::invalid_argument(
            "concurrent workload requires at least one thread");
    }
    return configuration;
}

class BenchmarkDatabase {
public:
    explicit BenchmarkDatabase(const Configuration& configuration)
        : keep_(configuration.keep_database) {
        if (configuration.database_path) {
            path_ = *configuration.database_path;
            if (std::filesystem::exists(path_)) {
                throw std::runtime_error(
                    "benchmark database path already exists: " +
                    path_.string());
            }
            return;
        }

        const auto nonce = static_cast<std::uint64_t>(
            Clock::now().time_since_epoch().count());
        const auto root = std::filesystem::temp_directory_path();
        for (std::size_t attempt = 0; attempt < 1'000; ++attempt) {
            auto candidate = root / ("minikv-benchmark-" +
                                     std::to_string(nonce) + "-" +
                                     std::to_string(attempt));
            if (!std::filesystem::exists(candidate)) {
                path_ = std::move(candidate);
                return;
            }
        }
        throw std::runtime_error(
            "could not choose a temporary benchmark database path");
    }

    ~BenchmarkDatabase() {
        if (!keep_ && !path_.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }

    BenchmarkDatabase(const BenchmarkDatabase&) = delete;
    BenchmarkDatabase& operator=(const BenchmarkDatabase&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
    bool keep_;
};

[[nodiscard]] minikv::MiniKVOptions store_options(
    const Configuration& configuration) {
    return minikv::MiniKVOptions{
        configuration.durability, configuration.segment_size};
}

[[nodiscard]] std::string make_key(std::size_t index) {
    std::ostringstream output;
    output << "key-" << std::setfill('0') << std::setw(12) << index;
    return output.str();
}

[[nodiscard]] std::string make_value(std::size_t size,
                                     std::uint64_t salt = 0) {
    std::string value(size, '\0');
    for (std::size_t index = 0; index < size; ++index) {
        value[index] = static_cast<char>('a' + ((index + salt) % 26U));
    }
    return value;
}

[[nodiscard]] std::vector<std::string> make_keys(std::size_t count) {
    std::vector<std::string> keys;
    keys.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        keys.push_back(make_key(index));
    }
    return keys;
}

[[nodiscard]] std::size_t choose_key(std::mt19937_64& random,
                                     const Configuration& configuration) {
    if (configuration.distribution == "hot" && (random() % 10U) != 0) {
        const auto hot_key_count =
            std::max<std::size_t>(1, configuration.key_count / 100U);
        return static_cast<std::size_t>(random() % hot_key_count);
    }
    return static_cast<std::size_t>(random() % configuration.key_count);
}

[[nodiscard]] std::vector<std::size_t> make_key_sequence(
    const Configuration& configuration,
    std::size_t count,
    std::uint64_t seed_offset = 0) {
    std::mt19937_64 random(configuration.seed + seed_offset);
    std::vector<std::size_t> sequence;
    sequence.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        sequence.push_back(choose_key(random, configuration));
    }
    return sequence;
}

template <typename Function>
void measure_operation(BenchmarkResult& result, Function&& function) {
    const auto start = Clock::now();
    std::forward<Function>(function)();
    const auto finish = Clock::now();
    result.latencies_ns.push_back(
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<Nanoseconds>(finish - start).count()));
}

void prefill(minikv::MiniKV& store,
             const std::vector<std::string>& keys,
             const std::string& value) {
    for (const auto& key : keys) {
        store.put(key, value);
    }
}

[[nodiscard]] std::uintmax_t database_data_size(
    const std::filesystem::path& database_path) {
    std::uintmax_t size = 0;
    if (!std::filesystem::exists(database_path)) {
        return size;
    }
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(database_path)) {
        if (entry.is_regular_file() && entry.path().extension() == ".dat") {
            size += entry.file_size();
        }
    }
    return size;
}

[[nodiscard]] BenchmarkResult run_sequential_put(
    const Configuration& configuration,
    const std::filesystem::path& database_path) {
    minikv::MiniKV store(database_path, store_options(configuration));
    const auto value = make_value(configuration.value_size);
    const auto keys = make_keys(configuration.operation_count);
    BenchmarkResult result;
    result.latencies_ns.reserve(configuration.operation_count);
    const auto start = Clock::now();
    for (const auto& key : keys) {
        measure_operation(result, [&] { store.put(key, value); });
    }
    result.elapsed = std::chrono::duration_cast<Nanoseconds>(Clock::now() - start);
    result.operations = configuration.operation_count;
    result.file_size_after = database_data_size(database_path);
    return result;
}

[[nodiscard]] BenchmarkResult run_random_get(
    const Configuration& configuration,
    const std::filesystem::path& database_path) {
    minikv::MiniKV store(database_path, store_options(configuration));
    const auto keys = make_keys(configuration.key_count);
    const auto value = make_value(configuration.value_size);
    prefill(store, keys, value);
    const auto sequence =
        make_key_sequence(configuration, configuration.operation_count);

    BenchmarkResult result;
    result.latencies_ns.reserve(configuration.operation_count);
    const auto start = Clock::now();
    for (const auto key_index : sequence) {
        measure_operation(result, [&] {
            const auto found = store.get(keys[key_index]);
            if (!found || *found != value) {
                throw std::runtime_error("random GET returned a wrong value");
            }
        });
    }
    result.elapsed = std::chrono::duration_cast<Nanoseconds>(Clock::now() - start);
    result.operations = configuration.operation_count;
    result.file_size_after = database_data_size(database_path);
    return result;
}

[[nodiscard]] BenchmarkResult run_update_heavy(
    const Configuration& configuration,
    const std::filesystem::path& database_path) {
    minikv::MiniKV store(database_path, store_options(configuration));
    const auto keys = make_keys(configuration.key_count);
    const auto initial_value = make_value(configuration.value_size);
    const auto updated_value = make_value(configuration.value_size, 7);
    prefill(store, keys, initial_value);
    const auto sequence =
        make_key_sequence(configuration, configuration.operation_count);

    BenchmarkResult result;
    result.latencies_ns.reserve(configuration.operation_count);
    const auto start = Clock::now();
    for (const auto key_index : sequence) {
        measure_operation(
            result, [&] { store.put(keys[key_index], updated_value); });
    }
    result.elapsed = std::chrono::duration_cast<Nanoseconds>(Clock::now() - start);
    result.operations = configuration.operation_count;
    result.file_size_after = database_data_size(database_path);
    return result;
}

[[nodiscard]] BenchmarkResult run_mixed(
    const Configuration& configuration,
    const std::filesystem::path& database_path) {
    minikv::MiniKV store(database_path, store_options(configuration));
    const auto keys = make_keys(configuration.key_count);
    const auto initial_value = make_value(configuration.value_size);
    const auto updated_value = make_value(configuration.value_size, 11);
    prefill(store, keys, initial_value);
    const auto sequence =
        make_key_sequence(configuration, configuration.operation_count);
    std::mt19937_64 random(configuration.seed ^ 0xA11CEULL);
    std::vector<bool> writes;
    writes.reserve(configuration.operation_count);
    for (std::size_t index = 0; index < configuration.operation_count;
         ++index) {
        writes.push_back((random() % 5U) == 0);
    }

    BenchmarkResult result;
    result.latencies_ns.reserve(configuration.operation_count);
    const auto start = Clock::now();
    for (std::size_t index = 0; index < sequence.size(); ++index) {
        const auto key_index = sequence[index];
        if (writes[index]) {
            measure_operation(
                result, [&] { store.put(keys[key_index], updated_value); });
        } else {
            measure_operation(result, [&] {
                if (!store.get(keys[key_index])) {
                    throw std::runtime_error("mixed GET missed an existing key");
                }
            });
        }
    }
    result.elapsed = std::chrono::duration_cast<Nanoseconds>(Clock::now() - start);
    result.operations = configuration.operation_count;
    result.file_size_after = database_data_size(database_path);
    return result;
}

[[nodiscard]] BenchmarkResult run_delete_heavy(
    const Configuration& configuration,
    const std::filesystem::path& database_path) {
    minikv::MiniKV store(database_path, store_options(configuration));
    const auto keys = make_keys(configuration.key_count);
    const auto value = make_value(configuration.value_size);
    prefill(store, keys, value);
    const auto sequence =
        make_key_sequence(configuration, configuration.operation_count);
    std::vector<bool> present(configuration.key_count, true);
    std::mt19937_64 random(configuration.seed ^ 0xDE1E7EULL);
    std::vector<bool> deletes;
    deletes.reserve(configuration.operation_count);
    for (std::size_t index = 0; index < configuration.operation_count;
         ++index) {
        deletes.push_back((random() % 5U) != 0);
    }

    BenchmarkResult result;
    result.latencies_ns.reserve(configuration.operation_count);
    const auto start = Clock::now();
    for (std::size_t index = 0; index < sequence.size(); ++index) {
        const auto key_index = sequence[index];
        if (deletes[index]) {
            measure_operation(result, [&] {
                static_cast<void>(store.erase(keys[key_index]));
            });
            present[key_index] = false;
        } else {
            measure_operation(
                result, [&] { store.put(keys[key_index], value); });
            present[key_index] = true;
        }
    }
    result.elapsed = std::chrono::duration_cast<Nanoseconds>(Clock::now() - start);
    result.operations = configuration.operation_count;
    result.file_size_after = database_data_size(database_path);
    return result;
}

[[nodiscard]] BenchmarkResult run_recovery(
    const Configuration& configuration,
    const std::filesystem::path& database_path) {
    const auto keys = make_keys(configuration.key_count);
    const auto value = make_value(configuration.value_size);
    {
        minikv::MiniKV store(database_path, store_options(configuration));
        prefill(store, keys, value);
        const auto sequence =
            make_key_sequence(configuration, configuration.operation_count);
        for (const auto key_index : sequence) {
            store.put(keys[key_index], value);
        }
    }

    BenchmarkResult result;
    result.file_size_before = database_data_size(database_path);
    const auto start = Clock::now();
    {
        minikv::MiniKV reopened(database_path, store_options(configuration));
        if (reopened.size() != configuration.key_count) {
            throw std::runtime_error("recovery rebuilt an unexpected key count");
        }
    }
    result.elapsed = std::chrono::duration_cast<Nanoseconds>(Clock::now() - start);
    result.recovery_ms =
        std::chrono::duration<double, std::milli>(result.elapsed).count();
    result.operations = configuration.key_count + configuration.operation_count;
    result.file_size_after = database_data_size(database_path);
    return result;
}

[[nodiscard]] BenchmarkResult run_compaction(
    const Configuration& configuration,
    const std::filesystem::path& database_path) {
    minikv::MiniKV store(database_path, store_options(configuration));
    const auto keys = make_keys(configuration.key_count);
    const auto initial_value = make_value(configuration.value_size);
    const auto updated_value = make_value(configuration.value_size, 17);
    prefill(store, keys, initial_value);
    const auto sequence =
        make_key_sequence(configuration, configuration.operation_count);
    for (const auto key_index : sequence) {
        store.put(keys[key_index], updated_value);
    }
    for (std::size_t index = 0; index < configuration.key_count; index += 4) {
        static_cast<void>(store.erase(keys[index]));
    }

    BenchmarkResult result;
    result.file_size_before = database_data_size(database_path);
    const auto start = Clock::now();
    store.compact();
    result.elapsed = std::chrono::duration_cast<Nanoseconds>(Clock::now() - start);
    result.compaction_ms =
        std::chrono::duration<double, std::milli>(result.elapsed).count();
    result.operations = 1;
    result.file_size_after = database_data_size(database_path);
    return result;
}

[[nodiscard]] BenchmarkResult run_concurrent(
    const Configuration& configuration,
    const std::filesystem::path& database_path) {
    minikv::MiniKV store(database_path, store_options(configuration));
    const auto keys = make_keys(configuration.key_count);
    const auto initial_value = make_value(configuration.value_size);
    const auto updated_value = make_value(configuration.value_size, 23);
    prefill(store, keys, initial_value);

    const auto thread_count =
        configuration.reader_threads + configuration.writer_threads;
    std::barrier start_barrier(static_cast<std::ptrdiff_t>(thread_count + 1));
    std::vector<std::vector<std::uint64_t>> thread_latencies(thread_count);
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    std::exception_ptr failure;
    std::mutex failure_mutex;

    const auto operations_for_thread = [&](std::size_t thread_index) {
        return configuration.operation_count / thread_count +
               (thread_index < configuration.operation_count % thread_count
                    ? 1U
                    : 0U);
    };
    const auto run_thread = [&](std::size_t thread_index, bool writer) {
        const auto operation_count = operations_for_thread(thread_index);
        auto sequence = make_key_sequence(
            configuration, operation_count, 1'000U + thread_index);
        auto& latencies = thread_latencies[thread_index];
        latencies.reserve(operation_count);
        start_barrier.arrive_and_wait();
        try {
            for (const auto key_index : sequence) {
                const auto start = Clock::now();
                if (writer) {
                    store.put(keys[key_index], updated_value);
                } else if (!store.get(keys[key_index])) {
                    throw std::runtime_error(
                        "concurrent GET missed an existing key");
                }
                const auto finish = Clock::now();
                latencies.push_back(static_cast<std::uint64_t>(
                    std::chrono::duration_cast<Nanoseconds>(finish - start)
                        .count()));
            }
        } catch (...) {
            std::lock_guard lock(failure_mutex);
            if (!failure) {
                failure = std::current_exception();
            }
        }
    };

    for (std::size_t index = 0; index < configuration.reader_threads;
         ++index) {
        threads.emplace_back(run_thread, index, false);
    }
    for (std::size_t index = 0; index < configuration.writer_threads;
         ++index) {
        threads.emplace_back(run_thread,
                             configuration.reader_threads + index,
                             true);
    }

    start_barrier.arrive_and_wait();
    const auto start = Clock::now();
    for (auto& thread : threads) {
        thread.join();
    }
    const auto elapsed = Clock::now() - start;
    if (failure) {
        std::rethrow_exception(failure);
    }

    BenchmarkResult result;
    result.operations = configuration.operation_count;
    result.elapsed = std::chrono::duration_cast<Nanoseconds>(elapsed);
    result.latencies_ns.reserve(configuration.operation_count);
    for (auto& latencies : thread_latencies) {
        result.latencies_ns.insert(result.latencies_ns.end(),
                                   latencies.begin(),
                                   latencies.end());
    }
    result.file_size_after = database_data_size(database_path);
    return result;
}

[[nodiscard]] BenchmarkResult run_workload(
    const Configuration& configuration,
    const std::filesystem::path& database_path) {
    if (configuration.workload == "sequential-put") {
        return run_sequential_put(configuration, database_path);
    }
    if (configuration.workload == "random-get") {
        return run_random_get(configuration, database_path);
    }
    if (configuration.workload == "update-heavy") {
        return run_update_heavy(configuration, database_path);
    }
    if (configuration.workload == "mixed") {
        return run_mixed(configuration, database_path);
    }
    if (configuration.workload == "delete-heavy") {
        return run_delete_heavy(configuration, database_path);
    }
    if (configuration.workload == "recovery") {
        return run_recovery(configuration, database_path);
    }
    if (configuration.workload == "compaction") {
        return run_compaction(configuration, database_path);
    }
    return run_concurrent(configuration, database_path);
}

[[nodiscard]] double percentile_us(std::vector<std::uint64_t> values,
                                   double percentile) {
    if (values.empty()) {
        return 0.0;
    }
    std::ranges::sort(values);
    const auto rank = static_cast<std::size_t>(
        std::ceil(percentile * static_cast<double>(values.size())));
    const auto index = std::clamp<std::size_t>(rank, 1, values.size()) - 1;
    return static_cast<double>(values[index]) / 1'000.0;
}

[[nodiscard]] double elapsed_seconds(const BenchmarkResult& result) {
    return std::chrono::duration<double>(result.elapsed).count();
}

[[nodiscard]] std::string durability_name(minikv::DurabilityMode mode) {
    return mode == minikv::DurabilityMode::Sync ? "sync" : "buffered";
}

[[nodiscard]] std::string utc_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

[[nodiscard]] std::string csv_escape(std::string_view value) {
    std::string escaped{"\""};
    for (const char character : value) {
        if (character == '\"') {
            escaped.push_back('\"');
        }
        escaped.push_back(character);
    }
    escaped.push_back('\"');
    return escaped;
}

void append_csv(const std::filesystem::path& output_path,
                const Configuration& configuration,
                const BenchmarkResult& result) {
    if (!output_path.parent_path().empty()) {
        std::filesystem::create_directories(output_path.parent_path());
    }
    const auto write_header = !std::filesystem::exists(output_path) ||
                              std::filesystem::file_size(output_path) == 0;
    std::ofstream output(output_path, std::ios::app);
    if (!output.is_open()) {
        throw std::runtime_error("could not open benchmark CSV: " +
                                 output_path.string());
    }
    if (write_header) {
        output << "timestamp_utc,label,workload,git_sha,os,compiler,build_type,"
                  "hardware_threads,durability,distribution,keys,value_bytes,"
                  "configured_operations,completed_operations,reader_threads,"
                  "writer_threads,seed,segment_bytes,elapsed_ms,operations_per_"
                  "second,p50_us,p95_us,p99_us,file_bytes_before,file_bytes_"
                  "after,recovery_ms,compaction_ms\n";
    }

    const auto seconds = elapsed_seconds(result);
    const auto operations_per_second =
        seconds == 0.0 ? 0.0
                       : static_cast<double>(result.operations) / seconds;
    output << csv_escape(utc_timestamp()) << ','
           << csv_escape(configuration.label) << ','
           << csv_escape(configuration.workload) << ','
           << csv_escape(MINIKV_BENCHMARK_GIT_SHA) << ','
           << csv_escape(MINIKV_BENCHMARK_OS) << ','
           << csv_escape(MINIKV_BENCHMARK_COMPILER) << ','
           << csv_escape(MINIKV_BENCHMARK_BUILD_TYPE) << ','
           << std::thread::hardware_concurrency() << ','
           << csv_escape(durability_name(configuration.durability)) << ','
           << csv_escape(configuration.distribution) << ','
           << configuration.key_count << ',' << configuration.value_size << ','
           << configuration.operation_count << ',' << result.operations << ','
           << configuration.reader_threads << ','
           << configuration.writer_threads << ',' << configuration.seed << ','
           << configuration.segment_size << ','
           << std::chrono::duration<double, std::milli>(result.elapsed).count()
           << ',' << operations_per_second << ','
           << percentile_us(result.latencies_ns, 0.50) << ','
           << percentile_us(result.latencies_ns, 0.95) << ','
           << percentile_us(result.latencies_ns, 0.99) << ','
           << result.file_size_before << ',' << result.file_size_after << ','
           << result.recovery_ms << ',' << result.compaction_ms << '\n';
    output.close();
    if (!output) {
        throw std::runtime_error("could not write benchmark CSV: " +
                                 output_path.string());
    }
}

void print_result(const Configuration& configuration,
                  const BenchmarkResult& result,
                  const std::filesystem::path& database_path) {
    const auto seconds = elapsed_seconds(result);
    const auto operations_per_second =
        seconds == 0.0 ? 0.0
                       : static_cast<double>(result.operations) / seconds;
    std::cout << std::fixed << std::setprecision(3)
              << "workload=" << configuration.workload << '\n'
              << "operations=" << result.operations << '\n'
              << "elapsed_ms="
              << std::chrono::duration<double, std::milli>(result.elapsed).count()
              << '\n'
              << "operations_per_second=" << operations_per_second << '\n'
              << "p50_us=" << percentile_us(result.latencies_ns, 0.50) << '\n'
              << "p95_us=" << percentile_us(result.latencies_ns, 0.95) << '\n'
              << "p99_us=" << percentile_us(result.latencies_ns, 0.99) << '\n'
              << "file_bytes_before=" << result.file_size_before << '\n'
              << "file_bytes_after=" << result.file_size_after << '\n'
              << "recovery_ms=" << result.recovery_ms << '\n'
              << "compaction_ms=" << result.compaction_ms << '\n'
              << "database=" << database_path.string() << '\n';
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        const auto configuration = parse_arguments(argc, argv);
        BenchmarkDatabase database(configuration);
        const auto result = run_workload(configuration, database.path());
        print_result(configuration, result, database.path());
        if (configuration.output_path) {
            append_csv(*configuration.output_path, configuration, result);
            std::cout << "csv=" << configuration.output_path->string() << '\n';
        }
        if (configuration.keep_database) {
            std::cout << "database_kept=true\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "benchmark error: " << error.what() << '\n';
        return 1;
    }
}
