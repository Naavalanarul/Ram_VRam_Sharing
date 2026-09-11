#include <meminfo/memory/slab_allocator.h>
#include <cstring>
#include <limits>
#include <new>

namespace meminfo {
namespace memory {

SlabAllocator::SlabAllocator(size_t total_size, size_t page_size)
    : page_size_(page_size) {
    if (page_size == 0) {
        throw std::invalid_argument("Page size cannot be 0");
    }
    
    total_pages_ = total_size / page_size;
    if (total_pages_ == 0) {
        throw std::invalid_argument("Total size must be at least one page size");
    }
    if (total_pages_ > std::numeric_limits<size_t>::max() / page_size_) {
        throw std::invalid_argument("Total size overflows addressable range");
    }

    buffer_ = new uint8_t[total_pages_ * page_size_];
    page_states_ = std::make_unique<PageState[]>(total_pages_);
    page_locks_ = std::make_unique<std::shared_mutex[]>(total_pages_);
    
    free_list_.reserve(total_pages_);
    for (size_t i = total_pages_; i > 0; --i) {
        page_states_[i - 1] = PageState::FREE;
        free_list_.push_back(i - 1);
    }
}

SlabAllocator::~SlabAllocator() {
    delete[] buffer_;
}

std::vector<size_t> SlabAllocator::allocate_pages(size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (free_list_.size() < count) {
        throw std::bad_alloc();
    }
    
    std::vector<size_t> pages;
    pages.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        size_t p = free_list_.back();
        free_list_.pop_back();
        page_states_[p] = PageState::ALLOCATED;

        // Clear before handing the page out. Pages are recycled between
        // handles and between sessions, so without this a new allocation
        // reads whatever the previous owner left there.
        std::memset(buffer_ + (p * page_size_), 0, page_size_);

        pages.push_back(p);
    }
    return pages;
}

void SlabAllocator::free_pages(const std::vector<size_t>& pages) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t p : pages) {
        if (p < total_pages_) {
            std::unique_lock<std::shared_mutex> page_lock(page_locks_[p]);
            if (page_states_[p] == PageState::ALLOCATED) {
                page_states_[p] = PageState::FREE;
                free_list_.push_back(p);
            }
        }
    }
}

void SlabAllocator::write_page(size_t page_idx, size_t page_offset, const uint8_t* data, size_t size) {
    // Written as a subtraction so a large page_offset cannot wrap the sum and
    // slip past the bound.
    if (page_idx >= total_pages_ || page_offset > page_size_ || size > page_size_ - page_offset) {
        throw std::out_of_range("Write out of bounds");
    }
    // Exclusive: two writers touching the same page would otherwise race.
    std::unique_lock<std::shared_mutex> page_lock(page_locks_[page_idx]);
    if (page_states_[page_idx] != PageState::ALLOCATED) {
        throw std::runtime_error("Write to unallocated page");
    }
    std::memcpy(buffer_ + (page_idx * page_size_) + page_offset, data, size);
}

void SlabAllocator::read_page(size_t page_idx, size_t page_offset, uint8_t* out_data, size_t size) const {
    if (page_idx >= total_pages_ || page_offset > page_size_ || size > page_size_ - page_offset) {
        throw std::out_of_range("Read out of bounds");
    }
    std::shared_lock<std::shared_mutex> page_lock(page_locks_[page_idx]);
    if (page_states_[page_idx] != PageState::ALLOCATED) {
        throw std::runtime_error("Read from unallocated page");
    }
    std::memcpy(out_data, buffer_ + (page_idx * page_size_) + page_offset, size);
}

size_t SlabAllocator::free_pages_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return free_list_.size();
}

} // namespace memory
} // namespace meminfo
