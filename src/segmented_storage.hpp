#pragma once

#include "record.hpp"
#include "storage_log.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace minikv {
enum class DurabilityMode;
}

namespace minikv::detail {

using GenerationId = std::uint64_t;
using SegmentId = std::uint64_t;

struct SegmentLocation {
    SegmentId segment_id;
    std::uint64_t offset;
    std::uint64_t record_size;
};

struct SegmentedLocatedRecord {
    Record record;
    SegmentLocation location;
};

[[nodiscard]] std::string generation_directory_name(GenerationId id);
[[nodiscard]] std::string segment_file_name(SegmentId id);

class SegmentedStorage {
public:
    SegmentedStorage(std::filesystem::path database_path,
                     DurabilityMode durability_mode,
                     std::uint64_t maximum_segment_size);

    SegmentedStorage(const SegmentedStorage&) = delete;
    SegmentedStorage& operator=(const SegmentedStorage&) = delete;
    SegmentedStorage(SegmentedStorage&&) = default;
    SegmentedStorage& operator=(SegmentedStorage&&) = default;

    [[nodiscard]] SegmentLocation append(const Record& record);
    [[nodiscard]] SegmentedLocatedRecord read_at(
        SegmentId segment_id,
        std::uint64_t offset) const;
    [[nodiscard]] Record read(const SegmentLocation& location) const;

    [[nodiscard]] std::vector<SegmentId> segment_ids() const;
    [[nodiscard]] SegmentId active_segment_id() const;
    [[nodiscard]] std::uint64_t segment_size(SegmentId segment_id) const;
    void truncate_active(std::uint64_t size);

private:
    struct Segment {
        SegmentId id;
        std::filesystem::path path;
        std::unique_ptr<StorageLog> log;
    };

    void initialize_database();
    void load_manifest();
    void write_manifest(GenerationId generation_id);
    void load_segments();
    void roll_over();

    [[nodiscard]] const Segment& find_segment(SegmentId id) const;
    [[nodiscard]] Segment& active_segment();
    [[nodiscard]] std::filesystem::path generation_path() const;
    [[nodiscard]] std::filesystem::path segment_path(SegmentId id) const;

    std::filesystem::path database_path_;
    DurabilityMode durability_mode_;
    std::uint64_t maximum_segment_size_;
    GenerationId generation_id_{0};
    std::vector<Segment> segments_;
};

}  // namespace minikv::detail
