#include <meminfo/memory/page_tracker.h>
#include <algorithm>
#include <limits>

namespace meminfo {
namespace memory {

PageTracker::PageTracker(SlabAllocator* allocator)
    : allocator_(allocator) {
    if (!allocator) {
        throw std::invalid_argument("Allocator cannot be null");
    }
}

void PageTracker::check_ownership(handle_t handle, uint64_t owner_id) const {
    auto it = allocations_.find(handle);
    if (it == allocations_.end()) {
        throw std::invalid_argument("Invalid handle");
    }
    if (it->second.owner_id != 0 && it->second.owner_id != owner_id) {
        throw PermissionDeniedError();
    }
}

handle_t PageTracker::allocate(size_t size) {
    if (size == 0) return 0;
    
    size_t page_size = allocator_->page_size();
    if (size > std::numeric_limits<size_t>::max() - (page_size - 1)) {
        throw std::bad_alloc();
    }
    size_t pages_needed = (size + page_size - 1) / page_size;
    
    std::vector<size_t> allocated_pages = allocator_->allocate_pages(pages_needed);
    
    std::lock_guard<std::mutex> lock(mutex_);
    handle_t h = next_handle_++;
    allocations_[h] = Allocation{size, std::move(allocated_pages), 0};
    return h;
}

void PageTracker::free(handle_t handle, uint64_t owner_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    check_ownership(handle, owner_id);
    auto it = allocations_.find(handle);
    if (it != allocations_.end()) {
        allocator_->free_pages(it->second.pages);
        allocations_.erase(it);
    }
}

void PageTracker::write(handle_t handle, size_t offset, const uint8_t* data, size_t size, uint64_t owner_id) {
    if (size == 0) return;
    
    std::vector<size_t> pages;
    size_t total_size = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        check_ownership(handle, owner_id);
        auto it = allocations_.find(handle);
        pages = it->second.pages;
        total_size = it->second.total_size;
    }
    
    // Subtraction form: offset + size can wrap for a hostile offset, which
    // would pass the bound and then index pages[] out of range below.
    if (offset > total_size || size > total_size - offset) {
        throw std::out_of_range("Write exceeds allocation size");
    }
    
    size_t page_size = allocator_->page_size();
    size_t data_idx = 0;
    
    while (data_idx < size) {
        size_t logical_pos = offset + data_idx;
        size_t page_idx = logical_pos / page_size;
        size_t offset_in_page = logical_pos % page_size;
        size_t bytes_to_write = std::min(size - data_idx, page_size - offset_in_page);
        
        allocator_->write_page(pages[page_idx], offset_in_page, data + data_idx, bytes_to_write);
        data_idx += bytes_to_write;
    }
}

void PageTracker::read(handle_t handle, size_t offset, uint8_t* out_data, size_t size, uint64_t owner_id) const {
    if (size == 0) return;
    
    std::vector<size_t> pages;
    size_t total_size = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        check_ownership(handle, owner_id);
        auto it = allocations_.find(handle);
        pages = it->second.pages;
        total_size = it->second.total_size;
    }
    
    // Subtraction form: offset + size can wrap for a hostile offset, which
    // would pass the bound and then index pages[] out of range below.
    if (offset > total_size || size > total_size - offset) {
        throw std::out_of_range("Read exceeds allocation size");
    }
    
    size_t page_size = allocator_->page_size();
    size_t data_idx = 0;
    
    while (data_idx < size) {
        size_t logical_pos = offset + data_idx;
        size_t page_idx = logical_pos / page_size;
        size_t offset_in_page = logical_pos % page_size;
        size_t bytes_to_read = std::min(size - data_idx, page_size - offset_in_page);
        
        allocator_->read_page(pages[page_idx], offset_in_page, out_data + data_idx, bytes_to_read);
        data_idx += bytes_to_read;
    }
}

void PageTracker::assign_owner(handle_t handle, uint64_t owner_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = allocations_.find(handle);
    if (it != allocations_.end()) {
        it->second.owner_id = owner_id;
    }
}

void PageTracker::free_all_for_owner(uint64_t owner_id) {
    // owner_id 0 is the "unowned" sentinel used by check_ownership; sweeping it
    // would free every allocation that has not been claimed yet.
    if (owner_id == 0) return;

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = allocations_.begin();
    while (it != allocations_.end()) {
        if (it->second.owner_id == owner_id) {
            allocator_->free_pages(it->second.pages);
            it = allocations_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace memory
} // namespace meminfo
