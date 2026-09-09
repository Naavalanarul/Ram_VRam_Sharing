#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include <stdexcept>
#include <mutex>
#include <shared_mutex>
#include <memory>

namespace meminfo {
namespace memory {

class SlabAllocator {
public:
    // Throws std::bad_alloc if memory cannot be reserved
    SlabAllocator(size_t total_size, size_t page_size = 4096);
    ~SlabAllocator();

    SlabAllocator(const SlabAllocator&) = delete;
    SlabAllocator& operator=(const SlabAllocator&) = delete;

    // Returns a list of page indices. Throws std::bad_alloc if not enough free pages.
    std::vector<size_t> allocate_pages(size_t count);
    
    // Frees a list of page indices back to the free list
    void free_pages(const std::vector<size_t>& pages);
    
    // Write data to a specific page
    void write_page(size_t page_idx, size_t page_offset, const uint8_t* data, size_t size);
    
    // Read data from a specific page
    void read_page(size_t page_idx, size_t page_offset, uint8_t* out_data, size_t size) const;
    
    size_t page_size() const { return page_size_; }
    size_t total_pages() const { return total_pages_; }
    size_t free_pages_count() const;

private:
    size_t page_size_;
    size_t total_pages_;
    uint8_t* buffer_;
    
    enum class PageState { FREE, ALLOCATED };
    std::unique_ptr<PageState[]> page_states_;
    mutable std::unique_ptr<std::shared_mutex[]> page_locks_;
    
    mutable std::mutex mutex_;
    std::vector<size_t> free_list_;
};

} // namespace memory
} // namespace meminfo
