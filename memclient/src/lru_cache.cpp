#include <meminfo/client/lru_cache.h>

namespace meminfo {
namespace client {

LRUCache::LRUCache(size_t max_bytes) : max_bytes_(max_bytes) {}

bool LRUCache::put(handle_t handle, std::vector<uint8_t> data, bool dirty) {
    if (data.size() > max_bytes_) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = map_.find(handle);
    if (it != map_.end()) {
        current_bytes_ -= it->second->data.size();
        list_.erase(it->second);
        map_.erase(it);
    }
    
    current_bytes_ += data.size();
    list_.push_front({handle, std::move(data), dirty});
    map_[handle] = list_.begin();
    
    return true;
}

std::optional<std::vector<uint8_t>> LRUCache::get(handle_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = map_.find(handle);
    if (it == map_.end()) {
        return std::nullopt;
    }
    
    // Move to front (most recently used)
    list_.splice(list_.begin(), list_, it->second);
    
    return it->second->data;
}

bool LRUCache::remove(handle_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = map_.find(handle);
    if (it == map_.end()) {
        return false;
    }
    
    current_bytes_ -= it->second->data.size();
    list_.erase(it->second);
    map_.erase(it);
    return true;
}

std::optional<CacheBlock> LRUCache::evict_one() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (list_.empty()) {
        return std::nullopt;
    }
    
    auto last = std::prev(list_.end());
    CacheBlock block = std::move(*last);
    
    current_bytes_ -= block.data.size();
    map_.erase(block.handle);
    list_.pop_back();
    
    return block;
}

size_t LRUCache::current_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_bytes_;
}

size_t LRUCache::max_size() const {
    return max_bytes_;
}

size_t LRUCache::count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return list_.size();
}

bool LRUCache::needs_eviction(size_t additional_bytes) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return (current_bytes_ + additional_bytes) > max_bytes_;
}

} // namespace client
} // namespace meminfo
