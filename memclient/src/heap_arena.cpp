#include <meminfo/client/heap_arena.h>

#include <algorithm>

namespace meminfo {
namespace client {

namespace {
uintptr_t align_up(uintptr_t value, size_t alignment) {
    const auto mask = static_cast<uintptr_t>(alignment - 1);
    return (value + mask) & ~mask;
}
} // namespace

HeapArena::HeapArena(void* base, size_t size)
    : base_(reinterpret_cast<uintptr_t>(base)), size_(size) {
    if (base_ != 0 && size_ != 0) {
        free_blocks_.emplace(0, size_);
    }
}

void* HeapArena::allocate(size_t bytes, size_t alignment) {
    if (bytes == 0 || base_ == 0) return nullptr;
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) return nullptr;

    std::lock_guard<std::mutex> lock(mutex_);

    for (auto it = free_blocks_.begin(); it != free_blocks_.end(); ++it) {
        const uintptr_t block_start = it->first;
        const size_t block_len = it->second;

        // Align within the block. The skipped bytes stay free, so a run of
        // over-aligned requests does not quietly leak the gaps.
        const uintptr_t aligned = align_up(base_ + block_start, alignment) - base_;
        const size_t lead = aligned - block_start;
        if (lead > block_len || block_len - lead < bytes) {
            continue;
        }

        const size_t trail = block_len - lead - bytes;
        free_blocks_.erase(it);
        if (lead > 0) free_blocks_.emplace(block_start, lead);
        if (trail > 0) free_blocks_.emplace(aligned + bytes, trail);

        live_blocks_.emplace(aligned, bytes);
        in_use_bytes_ += bytes;
        return reinterpret_cast<void*>(base_ + aligned);
    }

    return nullptr; // fragmented or full; the caller falls back to the real heap
}

void HeapArena::deallocate(void* ptr) {
    if (!ptr || !owns(ptr)) return;

    const uintptr_t offset = reinterpret_cast<uintptr_t>(ptr) - base_;

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = live_blocks_.find(offset);
    if (it == live_blocks_.end()) {
        return; // double free, or an interior pointer: neither is ours to act on
    }

    const size_t length = it->second;
    live_blocks_.erase(it);
    in_use_bytes_ -= length;
    insert_free(offset, length);
}

void HeapArena::insert_free(uintptr_t offset, size_t length) {
    // Merge forward with the block that starts where this one ends.
    auto next = free_blocks_.find(offset + length);
    if (next != free_blocks_.end()) {
        length += next->second;
        free_blocks_.erase(next);
    }

    // Merge backward with the block that ends where this one starts.
    auto after = free_blocks_.lower_bound(offset);
    if (after != free_blocks_.begin()) {
        auto prev = std::prev(after);
        if (prev->first + prev->second == offset) {
            offset = prev->first;
            length += prev->second;
            free_blocks_.erase(prev);
        }
    }

    free_blocks_.emplace(offset, length);
}

size_t HeapArena::block_size(const void* ptr) const {
    if (!ptr || !owns(ptr)) return 0;

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = live_blocks_.find(reinterpret_cast<uintptr_t>(ptr) - base_);
    return it == live_blocks_.end() ? 0 : it->second;
}

HeapArena::Stats HeapArena::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    Stats s;
    s.in_use_bytes = in_use_bytes_;
    s.live_blocks = live_blocks_.size();
    for (const auto& block : free_blocks_) {
        s.free_bytes += block.second;
        s.largest_free_block = std::max(s.largest_free_block, block.second);
    }
    return s;
}

} // namespace client
} // namespace meminfo
