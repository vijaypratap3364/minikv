#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace minikv {

namespace detail {
class StorageLog;
}

class MiniKV {
public:
    using Key = std::string;
    using Value = std::string;

    explicit MiniKV(std::filesystem::path log_path);
    ~MiniKV();

    MiniKV(const MiniKV&) = delete;
    MiniKV& operator=(const MiniKV&) = delete;
    MiniKV(MiniKV&&) noexcept;
    MiniKV& operator=(MiniKV&&) noexcept;

    // Inserts a new key or replaces the value stored for an existing key.
    // Throws without changing the in-memory state if the log append fails.
    void put(Key key, Value value);

    // Returns a copy of the value, or std::nullopt when the key is absent.
    [[nodiscard]] std::optional<Value> get(const Key& key) const;

    // Removes only in-memory state during Stage 2. Persistent deletion is
    // intentionally deferred until Stage 4.
    [[nodiscard]] bool erase(const Key& key);

    [[nodiscard]] bool contains(const Key& key) const;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

private:
    std::unique_ptr<detail::StorageLog> storage_log_;
    std::unordered_map<Key, Value> entries_;
};

}  // namespace minikv
