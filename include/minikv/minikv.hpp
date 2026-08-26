#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace minikv {

enum class DurabilityMode {
    Buffered,
    Sync,
};

namespace detail {
class StorageLog;
}

class MiniKV {
public:
    using Key = std::string;
    using Value = std::string;

    // Buffered flushes each record into the OS caching path. Sync additionally
    // requests a platform durable flush before a mutation returns.
    explicit MiniKV(
        std::filesystem::path log_path,
        DurabilityMode durability_mode = DurabilityMode::Buffered);
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

    // Appends a tombstone before removing an existing key from the index.
    // Returns false without writing when the key is already absent.
    [[nodiscard]] bool erase(const Key& key);

    [[nodiscard]] bool contains(const Key& key) const;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

private:
    struct IndexEntry {
        std::uint64_t offset;
        std::uint64_t record_size;
    };

    void recover();

    std::unique_ptr<detail::StorageLog> storage_log_;
    std::unordered_map<Key, IndexEntry> index_;
};

}  // namespace minikv
