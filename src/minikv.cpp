#include "minikv/minikv.hpp"

#include "record.hpp"
#include "segmented_storage.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <utility>

namespace minikv {

MiniKV::MiniKV(std::filesystem::path database_path, MiniKVOptions options)
    : storage_(std::make_unique<detail::SegmentedStorage>(
          std::move(database_path),
          options.durability_mode,
          options.maximum_segment_size)) {
    recover();
}

MiniKV::MiniKV(std::filesystem::path database_path,
               DurabilityMode durability_mode)
    : MiniKV(std::move(database_path),
             MiniKVOptions{durability_mode, 64U * 1024U * 1024U}) {}

MiniKV::~MiniKV() = default;

MiniKV::MiniKV(MiniKV&& other) {
    std::unique_lock lock(other.mutex_);
    storage_ = std::move(other.storage_);
    index_ = std::move(other.index_);
}

MiniKV& MiniKV::operator=(MiniKV&& other) {
    if (this == &other) {
        return *this;
    }

    std::scoped_lock lock(mutex_, other.mutex_);
    storage_ = std::move(other.storage_);
    index_ = std::move(other.index_);
    return *this;
}

void MiniKV::put(Key key, Value value) {
    std::lock_guard lock(mutex_);
    detail::Record record{
        detail::Operation::Put, std::move(key), std::move(value)};
    const auto location = storage_->append(record);
    index_.insert_or_assign(
        std::move(record.key),
        IndexEntry{
            location.segment_id, location.offset, location.record_size});
}

std::optional<MiniKV::Value> MiniKV::get(const Key& key) const {
    std::lock_guard lock(mutex_);
    const auto entry = index_.find(key);
    if (entry == index_.end()) {
        return std::nullopt;
    }

    auto record = storage_->read(detail::SegmentLocation{
        entry->second.segment_id,
        entry->second.offset,
        entry->second.record_size});
    if (record.operation != detail::Operation::Put || record.key != key) {
        throw detail::StorageError(
            "in-memory index points to an unexpected record");
    }
    return std::move(record.value);
}

bool MiniKV::erase(const Key& key) {
    std::lock_guard lock(mutex_);
    const auto entry = index_.find(key);
    if (entry == index_.end()) {
        return false;
    }

    const detail::Record tombstone{detail::Operation::Delete, key, {}};
    static_cast<void>(storage_->append(tombstone));
    index_.erase(entry);
    return true;
}

bool MiniKV::contains(const Key& key) const {
    std::lock_guard lock(mutex_);
    return index_.contains(key);
}

std::size_t MiniKV::size() const {
    std::lock_guard lock(mutex_);
    return index_.size();
}

bool MiniKV::empty() const {
    std::lock_guard lock(mutex_);
    return index_.empty();
}

void MiniKV::recover() {
    for (const auto segment_id : storage_->segment_ids()) {
        std::uint64_t offset = 0;
        while (offset < storage_->segment_size(segment_id)) {
            try {
                auto located = storage_->read_at(segment_id, offset);
                switch (located.record.operation) {
                    case detail::Operation::Put:
                        index_.insert_or_assign(
                            std::move(located.record.key),
                            IndexEntry{located.location.segment_id,
                                       located.location.offset,
                                       located.location.record_size});
                        break;
                    case detail::Operation::Delete:
                        index_.erase(located.record.key);
                        break;
                }
                offset += located.location.record_size;
            } catch (const detail::IncompleteRecordError&) {
                if (segment_id != storage_->active_segment_id()) {
                    throw detail::StorageError(
                        "an immutable segment contains an incomplete record");
                }
                storage_->truncate_active(offset);
                break;
            }
        }
    }
}

}  // namespace minikv
