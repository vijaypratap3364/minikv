#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace minikv {

enum class DurabilityMode {
    Buffered,
    Sync,
};

struct MiniKVOptions {
    DurabilityMode durability_mode{DurabilityMode::Buffered};
    std::uint64_t maximum_segment_size{64U * 1024U * 1024U};
};

namespace detail {
class SegmentedStorage;
}

class MiniKV {
public:
    using Key = std::string;
    using Value = std::string;

    // database_path names a MiniKV directory. Buffered flushes each record into
    // the OS caching path. Sync additionally requests a platform durable flush
    // before a mutation returns.
    explicit MiniKV(
        std::filesystem::path database_path,
        MiniKVOptions options = {});
    MiniKV(std::filesystem::path database_path,
           DurabilityMode durability_mode);
    ~MiniKV();

    MiniKV(const MiniKV&) = delete;
    MiniKV& operator=(const MiniKV&) = delete;
    MiniKV(MiniKV&& other);
    MiniKV& operator=(MiniKV&& other);

    // Inserts a new key or replaces the value stored for an existing key.
    // Throws without changing the in-memory state if the log append fails.
    void put(Key key, Value value);

    // Returns a copy of the value, or std::nullopt when the key is absent.
    [[nodiscard]] std::optional<Value> get(const Key& key) const;

    // Appends a tombstone before removing an existing key from the index.
    // Returns false without writing when the key is already absent.
    [[nodiscard]] bool erase(const Key& key);

    [[nodiscard]] bool contains(const Key& key) const;
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] bool empty() const;

private:
    struct IndexEntry {
        std::uint64_t segment_id;
        std::uint64_t offset;
        std::uint64_t record_size;
    };

    void recover();

    mutable std::mutex mutex_;
    std::unique_ptr<detail::SegmentedStorage> storage_;
    std::unordered_map<Key, IndexEntry> index_;
};

}  // namespace minikv
