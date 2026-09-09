#pragma once
#include <meminfo/common/types.h>
#include <vector>
#include <list>
#include <unordered_map>
#include <mutex>
#include <cstdint>
#include <optional>

namespace meminfo {
namespace client {

struct CacheBlock {
    handle_t handle;
    std::vector<uint8_t> data;
    bool dirty;
};

class LRUCache {
public:
    explicit LRUCache(size_t max_bytes);
    ~LRUCache() = default;

    // Returns true if the block was successfully put (or updated)
    // Returns false if the block itself is larger than max_bytes
    bool put(handle_t handle, std::vector<uint8_t> data, bool dirty = true);

    // Retrieves data. Returns nullopt if not found. Updates LRU.
    std::optional<std::vector<uint8_t>> get(handle_t handle);

    // Removes a block completely (e.g. on free)
    bool remove(handle_t handle);

    // Evicts the least recently used block. 
    // Returns nullopt if cache is empty.
    std::optional<CacheBlock> evict_one();

    // Current cache size in bytes
    size_t current_size() const;

    // Max cache size in bytes
    size_t max_size() const;

    // Total items in cache
    size_t count() const;
    
    // Checks if eviction is needed based on a hypothetical new allocation
    bool needs_eviction(size_t additional_bytes) const;

private:
    size_t max_bytes_;
    size_t current_bytes_ = 0;
    
    mutable std::mutex mutex_;
    std::list<CacheBlock> list_;
    std::unordered_map<handle_t, std::list<CacheBlock>::iterator> map_;
};

} // namespace client
} // namespace meminfo
