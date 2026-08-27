#include "segmented_storage.hpp"

#include "minikv/minikv.hpp"
#include "platform_sync.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace minikv::detail {
namespace {

constexpr std::string_view manifest_file_name = "CURRENT";
constexpr std::string_view manifest_temporary_file_name = "CURRENT.tmp";
constexpr std::string_view manifest_magic = "MINIKV-MANIFEST";
constexpr std::uint32_t manifest_version = 1;
constexpr std::string_view generation_prefix = "generation-";
constexpr std::string_view segment_prefix = "segment-";
constexpr std::string_view segment_suffix = ".dat";
constexpr std::size_t formatted_id_width = 20;

[[nodiscard]] std::string format_id(std::uint64_t id) {
    std::ostringstream output;
    output << std::setfill('0') << std::setw(formatted_id_width) << id;
    return output.str();
}

[[nodiscard]] SegmentId parse_segment_file_name(
    std::string_view file_name) {
    const auto expected_size =
        segment_prefix.size() + formatted_id_width + segment_suffix.size();
    if (file_name.size() != expected_size ||
        !file_name.starts_with(segment_prefix) ||
        !file_name.ends_with(segment_suffix)) {
        throw StorageError("generation contains an unexpected file: " +
                           std::string(file_name));
    }

    const auto digits = file_name.substr(segment_prefix.size(),
                                         formatted_id_width);
    SegmentId id = 0;
    const auto [end, error] =
        std::from_chars(digits.data(), digits.data() + digits.size(), id);
    if (error != std::errc{} || end != digits.data() + digits.size() ||
        id == 0) {
        throw StorageError("segment file has an invalid identifier: " +
                           std::string(file_name));
    }
    return id;
}

struct ParsedGenerationName {
    GenerationId id;
    bool temporary;
};

[[nodiscard]] std::optional<ParsedGenerationName> parse_generation_name(
    std::string_view directory_name) {
    bool temporary = false;
    if (directory_name.ends_with(".tmp")) {
        temporary = true;
        directory_name.remove_suffix(4);
    }
    if (directory_name.size() !=
            generation_prefix.size() + formatted_id_width ||
        !directory_name.starts_with(generation_prefix)) {
        return std::nullopt;
    }

    const auto digits = directory_name.substr(generation_prefix.size());
    GenerationId id = 0;
    const auto [end, error] =
        std::from_chars(digits.data(), digits.data() + digits.size(), id);
    if (error != std::errc{} || end != digits.data() + digits.size() ||
        id == 0) {
        return std::nullopt;
    }
    return ParsedGenerationName{id, temporary};
}

[[nodiscard]] bool is_sync_mode(DurabilityMode mode) noexcept {
    return mode == DurabilityMode::Sync;
}

}  // namespace

std::string generation_directory_name(GenerationId id) {
    return std::string(generation_prefix) + format_id(id);
}

std::string segment_file_name(SegmentId id) {
    return std::string(segment_prefix) + format_id(id) +
           std::string(segment_suffix);
}

SegmentedStorage::SegmentedStorage(std::filesystem::path database_path,
                                   DurabilityMode durability_mode,
                                   std::uint64_t maximum_segment_size)
    : database_path_(std::move(database_path)),
      durability_mode_(durability_mode),
      maximum_segment_size_(maximum_segment_size) {
    if (durability_mode_ != DurabilityMode::Buffered &&
        durability_mode_ != DurabilityMode::Sync) {
        throw StorageError("segmented storage has an invalid durability mode");
    }
    if (maximum_segment_size_ == 0) {
        throw StorageError("maximum segment size must be greater than zero");
    }

    std::error_code error;
    const auto exists = std::filesystem::exists(database_path_, error);
    if (error) {
        throw StorageError("could not inspect database path: " +
                           error.message());
    }
    if (exists && !std::filesystem::is_directory(database_path_, error)) {
        if (error) {
            throw StorageError("could not inspect database path type: " +
                               error.message());
        }
        throw StorageError("MiniKV database path must be a directory");
    }
    if (!exists &&
        !std::filesystem::create_directories(database_path_, error)) {
        if (error) {
            throw StorageError("could not create database directory: " +
                               error.message());
        }
    }

    const auto manifest_path = database_path_ / manifest_file_name;
    const auto manifest_exists =
        std::filesystem::exists(manifest_path, error);
    if (error) {
        throw StorageError("could not inspect database manifest: " +
                           error.message());
    }
    if (!manifest_exists) {
        if (std::filesystem::directory_iterator(database_path_) !=
            std::filesystem::directory_iterator{}) {
            throw StorageError(
                "database directory is nonempty but has no CURRENT manifest");
        }
        initialize_database();
    } else {
        load_manifest();
        load_segments();
        cleanup_obsolete_generations();
    }
}

SegmentLocation SegmentedStorage::append(const Record& record) {
    if (compaction_failed_) {
        throw StorageError(
            "segmented storage cannot append after a compaction failure");
    }
    const auto encoded = encode_record(record);
    const auto encoded_size = static_cast<std::uint64_t>(encoded.size());
    const auto current_size = active_segment().log->size();
    if (current_size != 0 &&
        (encoded_size > maximum_segment_size_ ||
         current_size > maximum_segment_size_ - encoded_size)) {
        roll_over();
    }

    const auto appended = active_segment().log->append_encoded(encoded);
    return SegmentLocation{
        active_segment().id, appended.offset, appended.size};
}

SegmentedLocatedRecord SegmentedStorage::read_at(
    SegmentId segment_id,
    std::uint64_t offset) const {
    const auto& segment = find_segment(segment_id);
    auto located = segment.log->read_at(offset);
    return SegmentedLocatedRecord{
        std::move(located.record),
        SegmentLocation{segment_id,
                        located.location.offset,
                        located.location.size}};
}

Record SegmentedStorage::read(const SegmentLocation& location) const {
    auto located = read_at(location.segment_id, location.offset);
    if (located.location.record_size != location.record_size) {
        throw StorageError(
            "indexed record size does not match its storage segment");
    }
    return std::move(located.record);
}

CompactionResult SegmentedStorage::compact(
    const std::vector<Record>& live_records,
    const CompactionFaultInjector& fault_injector) {
    if (compaction_failed_) {
        throw StorageError(
            "segmented storage cannot compact after a compaction failure");
    }
    if (generation_id_ == std::numeric_limits<GenerationId>::max()) {
        throw StorageError("generation identifier would overflow");
    }

    cleanup_obsolete_generations();
    const auto next_generation_id = generation_id_ + 1;
    const auto final_path = generation_path(next_generation_id);
    auto temporary_path = final_path;
    temporary_path += ".tmp";

    std::error_code error;
    if (!std::filesystem::create_directory(temporary_path, error)) {
        throw StorageError("could not create temporary compacted generation: " +
                           error.message());
    }

    std::vector<SegmentLocation> locations;
    locations.reserve(live_records.size());
    SegmentId output_segment_id = 1;
    auto output_path =
        temporary_path / segment_file_name(output_segment_id);
    auto output_log = std::make_unique<StorageLog>(
        output_path, durability_mode_, StorageLogMode::Append);

    const auto close_output = [&] {
        output_log->seal();
        output_log.reset();
        if (is_sync_mode(durability_mode_)) {
            try {
                sync_file_to_storage(output_path);
            } catch (const std::system_error& sync_error) {
                throw StorageError("could not sync compacted segment: " +
                                   std::string(sync_error.what()));
            }
        }
    };

    for (const auto& record : live_records) {
        if (record.operation != Operation::Put) {
            throw StorageError("compaction input must contain only live PUTs");
        }
        const auto encoded = encode_record(record);
        const auto encoded_size =
            static_cast<std::uint64_t>(encoded.size());
        const auto current_size = output_log->size();
        if (current_size != 0 &&
            (encoded_size > maximum_segment_size_ ||
             current_size > maximum_segment_size_ - encoded_size)) {
            close_output();
            if (output_segment_id ==
                std::numeric_limits<SegmentId>::max()) {
                throw StorageError(
                    "compacted segment identifier would overflow");
            }
            ++output_segment_id;
            output_path =
                temporary_path / segment_file_name(output_segment_id);
            output_log = std::make_unique<StorageLog>(
                output_path, durability_mode_, StorageLogMode::Append);
        }

        const auto appended = output_log->append_encoded(encoded);
        locations.push_back(SegmentLocation{
            output_segment_id, appended.offset, appended.size});
    }
    close_output();

    if (fault_injector) {
        fault_injector(CompactionPhase::TemporaryGenerationComplete);
    }

    try {
        install_directory_atomically(
            temporary_path, final_path, is_sync_mode(durability_mode_));
    } catch (const std::system_error& install_error) {
        throw StorageError("could not install compacted generation: " +
                           std::string(install_error.what()));
    }

    if (fault_injector) {
        fault_injector(CompactionPhase::GenerationInstalled);
    }

    auto replacement_segments = open_segments(final_path);
    for (std::size_t index = 0; index < live_records.size(); ++index) {
        const auto& location = locations[index];
        const auto segment = std::ranges::lower_bound(
            replacement_segments, location.segment_id, {}, &Segment::id);
        if (segment == replacement_segments.end() ||
            segment->id != location.segment_id ||
            segment->log->read(AppendResult{
                location.offset, location.record_size}) != live_records[index]) {
            throw StorageError(
                "compacted generation failed read-back validation");
        }
    }
    try {
        write_manifest(next_generation_id);
    } catch (...) {
        // A manifest replace followed by a failed durable directory sync has an
        // ambiguous outcome. Keep old reads available, but reject later writes
        // until restart selects and validates the authoritative generation.
        compaction_failed_ = true;
        throw;
    }

    if (fault_injector) {
        fault_injector(CompactionPhase::ManifestCommitted);
    }

    const auto old_generation_path = generation_path();
    segments_.swap(replacement_segments);
    generation_id_ = next_generation_id;
    replacement_segments.clear();

    std::filesystem::remove_all(old_generation_path, error);
    // The manifest and in-memory view already select the new generation. A
    // cleanup failure must not turn that committed compaction into an apparent
    // failure with stale index locations; restart retries obsolete cleanup.

    return CompactionResult{std::move(locations)};
}

std::vector<SegmentId> SegmentedStorage::segment_ids() const {
    std::vector<SegmentId> ids;
    ids.reserve(segments_.size());
    for (const auto& segment : segments_) {
        ids.push_back(segment.id);
    }
    return ids;
}

SegmentId SegmentedStorage::active_segment_id() const {
    return segments_.back().id;
}

std::uint64_t SegmentedStorage::segment_size(SegmentId segment_id) const {
    return find_segment(segment_id).log->size();
}

void SegmentedStorage::truncate_active(std::uint64_t size) {
    active_segment().log->truncate(size);
}

void SegmentedStorage::initialize_database() {
    generation_id_ = 1;
    std::error_code error;
    if (!std::filesystem::create_directory(generation_path(), error)) {
        throw StorageError("could not create initial generation: " +
                           error.message());
    }

    auto log = std::make_unique<StorageLog>(
        segment_path(1), durability_mode_, StorageLogMode::Append);
    if (is_sync_mode(durability_mode_)) {
        try {
            sync_file_to_storage(segment_path(1));
        } catch (const std::system_error& sync_error) {
            throw StorageError("could not sync initial segment: " +
                               std::string(sync_error.what()));
        }
    }
    segments_.push_back(Segment{1, segment_path(1), std::move(log)});
    write_manifest(generation_id_);
}

void SegmentedStorage::load_manifest() {
    const auto path = database_path_ / manifest_file_name;
    std::ifstream input(path);
    if (!input.is_open()) {
        throw StorageError("could not open database manifest");
    }

    std::string magic;
    std::uint32_t version = 0;
    GenerationId generation = 0;
    std::string extra;
    if (!(input >> magic >> version >> generation) ||
        (input >> extra) || magic != manifest_magic ||
        version != manifest_version || generation == 0) {
        throw StorageError("database manifest is malformed or unsupported");
    }
    generation_id_ = generation;
}

void SegmentedStorage::write_manifest(GenerationId generation_id) {
    const auto temporary_path =
        database_path_ / manifest_temporary_file_name;
    const auto manifest_path = database_path_ / manifest_file_name;

    std::ofstream output(temporary_path, std::ios::trunc);
    if (!output.is_open()) {
        throw StorageError("could not create temporary database manifest");
    }
    output << manifest_magic << ' ' << manifest_version << ' '
           << generation_id << '\n';
    output.close();
    if (!output) {
        throw StorageError("could not write temporary database manifest");
    }

    try {
        if (is_sync_mode(durability_mode_)) {
            sync_file_to_storage(temporary_path);
        }
        replace_file_atomically(temporary_path,
                                manifest_path,
                                is_sync_mode(durability_mode_));
    } catch (const std::system_error& error) {
        throw StorageError("could not install database manifest: " +
                           std::string(error.what()));
    }
}

void SegmentedStorage::load_segments() {
    segments_ = open_segments(generation_path());
}

std::vector<SegmentedStorage::Segment> SegmentedStorage::open_segments(
    const std::filesystem::path& path) const {
    std::error_code error;
    if (!std::filesystem::is_directory(path, error)) {
        if (error) {
            throw StorageError("could not inspect active generation: " +
                               error.message());
        }
        throw StorageError("active generation directory is missing");
    }

    std::vector<std::pair<SegmentId, std::filesystem::path>> files;
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        if (!entry.is_regular_file()) {
            throw StorageError("active generation contains a non-file entry");
        }
        files.emplace_back(
            parse_segment_file_name(entry.path().filename().string()),
            entry.path());
    }
    std::ranges::sort(files, {}, &std::pair<SegmentId,
                                            std::filesystem::path>::first);
    if (files.empty()) {
        throw StorageError("active generation contains no segment files");
    }

    SegmentId expected_id = 1;
    std::vector<Segment> opened_segments;
    opened_segments.reserve(files.size());
    for (std::size_t index = 0; index < files.size(); ++index) {
        const auto& [id, segment_file_path] = files[index];
        if (id != expected_id) {
            throw StorageError(
                "active generation has missing or duplicate segment ids");
        }
        const auto mode = index + 1 == files.size()
                              ? StorageLogMode::Append
                              : StorageLogMode::ReadOnly;
        opened_segments.push_back(Segment{
            id,
            segment_file_path,
            std::make_unique<StorageLog>(
                segment_file_path, durability_mode_, mode)});
        ++expected_id;
    }
    return opened_segments;
}

void SegmentedStorage::cleanup_obsolete_generations() {
    std::vector<std::filesystem::path> obsolete_paths;
    for (const auto& entry :
         std::filesystem::directory_iterator(database_path_)) {
        const auto name = entry.path().filename().string();
        if (entry.is_regular_file() &&
            name == manifest_temporary_file_name) {
            obsolete_paths.push_back(entry.path());
            continue;
        }
        if (!entry.is_directory()) {
            continue;
        }
        const auto parsed = parse_generation_name(name);
        if (parsed &&
            (parsed->temporary || parsed->id != generation_id_)) {
            obsolete_paths.push_back(entry.path());
        }
    }

    for (const auto& path : obsolete_paths) {
        std::error_code error;
        std::filesystem::remove_all(path, error);
        if (error) {
            throw StorageError("could not clean obsolete generation: " +
                               error.message());
        }
    }
}

void SegmentedStorage::roll_over() {
    if (active_segment().id == std::numeric_limits<SegmentId>::max()) {
        throw StorageError("segment identifier would overflow");
    }
    const auto next_id = active_segment().id + 1;
    const auto path = segment_path(next_id);
    std::error_code error;
    if (std::filesystem::exists(path, error) || error) {
        throw StorageError("next segment path is not available");
    }

    auto next_log = std::make_unique<StorageLog>(
        path, durability_mode_, StorageLogMode::Append);
    active_segment().log->seal();
    segments_.push_back(Segment{next_id, path, std::move(next_log)});
}

const SegmentedStorage::Segment& SegmentedStorage::find_segment(
    SegmentId id) const {
    const auto entry = std::ranges::lower_bound(
        segments_, id, {}, &Segment::id);
    if (entry == segments_.end() || entry->id != id) {
        throw StorageError("indexed segment does not exist");
    }
    return *entry;
}

SegmentedStorage::Segment& SegmentedStorage::active_segment() {
    return segments_.back();
}

std::filesystem::path SegmentedStorage::generation_path() const {
    return generation_path(generation_id_);
}

std::filesystem::path SegmentedStorage::generation_path(
    GenerationId id) const {
    return database_path_ / generation_directory_name(id);
}

std::filesystem::path SegmentedStorage::segment_path(SegmentId id) const {
    return generation_path() / segment_file_name(id);
}

}  // namespace minikv::detail
