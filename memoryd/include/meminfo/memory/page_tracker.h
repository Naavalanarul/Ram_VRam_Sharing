#pragma once
#include <meminfo/memory/slab_allocator.h>
#include <meminfo/common/types.h>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <stdexcept>

namespace meminfo {
namespace memory {

class PermissionDeniedError : public std::runtime_error {
public:
    PermissionDeniedError() : std::runtime_error("Permission denied: handle owned by another session") {}
};

class PageTracker {
public:
    explicit PageTracker(SlabAllocator* allocator);
    ~PageTracker() = default;

    // Allocate contiguous logical space, returns a handle
    handle_t allocate(size_t size);
    
    // Free a handle
    void free(handle_t handle, uint64_t owner_id);
    
    // Write data to a logical handle at logical offset
    void write(handle_t handle, size_t offset, const uint8_t* data, size_t size, uint64_t owner_id);
    
    // Read data from a logical handle at logical offset
    void read(handle_t handle, size_t offset, uint8_t* out_data, size_t size, uint64_t owner_id) const;
    
    // Assign a connection owner to a handle
    void assign_owner(handle_t handle, uint64_t owner_id);
    
    // Free all handles associated with a specific owner (e.g., on disconnect)
    void free_all_for_owner(uint64_t owner_id);

private:
    struct Allocation {
        size_t total_size;
        std::vector<size_t> pages;
        uint64_t owner_id = 0;
    };
    
    SlabAllocator* allocator_;
    
    mutable std::mutex mutex_;
    std::unordered_map<handle_t, Allocation> allocations_;
    handle_t next_handle_ = 1;
    
    void check_ownership(handle_t handle, uint64_t owner_id) const;
};

} // namespace memory
} // namespace meminfo
