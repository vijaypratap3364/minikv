#include "minikv/minikv.hpp"

#include "record.hpp"
#include "storage_log.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <utility>

namespace minikv {

MiniKV::MiniKV(std::filesystem::path log_path,
               DurabilityMode durability_mode)
    : storage_log_(std::make_unique<detail::StorageLog>(
          std::move(log_path), durability_mode)) {
    recover();
}

MiniKV::~MiniKV() = default;
MiniKV::MiniKV(MiniKV&&) noexcept = default;
MiniKV& MiniKV::operator=(MiniKV&&) noexcept = default;

void MiniKV::put(Key key, Value value) {
    detail::Record record{
        detail::Operation::Put, std::move(key), std::move(value)};
    const auto location = storage_log_->append(record);
    index_.insert_or_assign(
        std::move(record.key), IndexEntry{location.offset, location.size});
}

std::optional<MiniKV::Value> MiniKV::get(const Key& key) const {
    const auto entry = index_.find(key);
    if (entry == index_.end()) {
        return std::nullopt;
    }

    auto record = storage_log_->read(detail::AppendResult{
        entry->second.offset, entry->second.record_size});
    if (record.operation != detail::Operation::Put || record.key != key) {
        throw detail::StorageError(
            "in-memory index points to an unexpected record");
    }
    return std::move(record.value);
}

bool MiniKV::erase(const Key& key) {
    const auto entry = index_.find(key);
    if (entry == index_.end()) {
        return false;
    }

    const detail::Record tombstone{detail::Operation::Delete, key, {}};
    static_cast<void>(storage_log_->append(tombstone));
    index_.erase(entry);
    return true;
}

bool MiniKV::contains(const Key& key) const {
    return index_.contains(key);
}

std::size_t MiniKV::size() const noexcept {
    return index_.size();
}

bool MiniKV::empty() const noexcept {
    return index_.empty();
}

void MiniKV::recover() {
    std::uint64_t offset = 0;
    while (offset < storage_log_->size()) {
        try {
            auto located = storage_log_->read_at(offset);
            switch (located.record.operation) {
                case detail::Operation::Put:
                    index_.insert_or_assign(
                        std::move(located.record.key),
                        IndexEntry{located.location.offset,
                                   located.location.size});
                    break;
                case detail::Operation::Delete:
                    index_.erase(located.record.key);
                    break;
            }
            offset += located.location.size;
        } catch (const detail::IncompleteRecordError&) {
            storage_log_->truncate(offset);
            break;
        }
    }
}

}  // namespace minikv
