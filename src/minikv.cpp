#include "minikv/minikv.hpp"

#include <utility>

namespace minikv {

void MiniKV::put(Key key, Value value) {
    entries_.insert_or_assign(std::move(key), std::move(value));
}

std::optional<MiniKV::Value> MiniKV::get(const Key& key) const {
    const auto entry = entries_.find(key);
    if (entry == entries_.end()) {
        return std::nullopt;
    }

    return entry->second;
}

bool MiniKV::erase(const Key& key) {
    return entries_.erase(key) != 0;
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
