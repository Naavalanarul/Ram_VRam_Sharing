#pragma once

#include <cstdint>
#include <unordered_map>
#include <shared_mutex>
#include <atomic>

class DevicePointerTracker {
public:
    DevicePointerTracker() = default;
    ~DevicePointerTracker() = default;

    DevicePointerTracker(const DevicePointerTracker&) = delete;
    DevicePointerTracker& operator=(const DevicePointerTracker&) = delete;

    void* allocate(uint64_t remote_ptr, size_t size) {
        uint64_t synthetic = next_synthetic_.fetch_add(4096);
        synthetic = (synthetic + 4095) & ~4095ull;
        void* ptr = reinterpret_cast<void*>(synthetic);
        std::unique_lock<std::shared_mutex> lock(mutex_);
        map_[ptr] = {remote_ptr, size};
        return ptr;
    }

    bool free(void* synthetic) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto it = map_.find(synthetic);
        if (it == map_.end()) return false;
        map_.erase(it);
        return true;
    }

    bool get_remote(void* synthetic, uint64_t& out_remote, size_t* out_size = nullptr) const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = map_.find(synthetic);
        if (it == map_.end()) return false;
        out_remote = it->second.remote_ptr;
        if (out_size) *out_size = it->second.size;
        return true;
    }

private:
    struct Entry {
        uint64_t remote_ptr = 0;
        size_t size = 0;
    };

    mutable std::shared_mutex mutex_;
    std::unordered_map<void*, Entry> map_;
    std::atomic<uint64_t> next_synthetic_{0x10000000000ull};
};