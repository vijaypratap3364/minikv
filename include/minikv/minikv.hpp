#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>

namespace minikv {

class MiniKV {
public:
    using Key = std::string;
    using Value = std::string;

    // Inserts a new key or replaces the value stored for an existing key.
    void put(Key key, Value value);

    // Returns a copy of the value, or std::nullopt when the key is absent.
    [[nodiscard]] std::optional<Value> get(const Key& key) const;

    // Returns true only when an existing key was removed.
    [[nodiscard]] bool erase(const Key& key);

    [[nodiscard]] bool contains(const Key& key) const;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

private:
    std::unordered_map<Key, Value> entries_;
};

}  // namespace minikv
