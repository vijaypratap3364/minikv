#include "minikv/minikv.hpp"

#include "record.hpp"
#include "storage_log.hpp"

#include <filesystem>
#include <memory>
#include <utility>

namespace minikv {

MiniKV::MiniKV(std::filesystem::path log_path)
    : storage_log_(
          std::make_unique<detail::StorageLog>(std::move(log_path))) {}

MiniKV::~MiniKV() = default;
MiniKV::MiniKV(MiniKV&&) noexcept = default;
MiniKV& MiniKV::operator=(MiniKV&&) noexcept = default;

void MiniKV::put(Key key, Value value) {
    detail::Record record{
        detail::Operation::Put, std::move(key), std::move(value)};
    static_cast<void>(storage_log_->append(record));
    entries_.insert_or_assign(std::move(record.key), std::move(record.value));
}

std::optional<MiniKV::Value> MiniKV::get(const Key& key) const {
    const auto entry = entries_.find(key);
    if (entry == entries_.end()) {
        return std::nullopt;
    }

    return entry->second;
}

bool MiniKV::erase(const Key& key) {
    const auto entry = entries_.find(key);
    if (entry == entries_.end()) {
        return false;
    }

    const detail::Record record{detail::Operation::Delete, key, {}};
    static_cast<void>(storage_log_->append(record));
    entries_.erase(entry);
    return true;
}

bool MiniKV::contains(const Key& key) const {
    return entries_.contains(key);
}

std::size_t MiniKV::size() const noexcept {
    return entries_.size();
}

bool MiniKV::empty() const noexcept {
    return entries_.empty();
}

}  // namespace minikv
