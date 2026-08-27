#include "segmented_storage.hpp"

#include "minikv/minikv.hpp"
#include "platform_sync.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
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
    }
}

SegmentLocation SegmentedStorage::append(const Record& record) {
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
    const auto path = generation_path();
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
    segments_.reserve(files.size());
    for (std::size_t index = 0; index < files.size(); ++index) {
        const auto& [id, segment_file_path] = files[index];
        if (id != expected_id) {
            throw StorageError(
                "active generation has missing or duplicate segment ids");
        }
        const auto mode = index + 1 == files.size()
                              ? StorageLogMode::Append
                              : StorageLogMode::ReadOnly;
        segments_.push_back(Segment{
            id,
            segment_file_path,
            std::make_unique<StorageLog>(
                segment_file_path, durability_mode_, mode)});
        ++expected_id;
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
    return database_path_ / generation_directory_name(generation_id_);
}

std::filesystem::path SegmentedStorage::segment_path(SegmentId id) const {
    return generation_path() / segment_file_name(id);
}

}  // namespace minikv::detail
