#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>

namespace meminfo {
namespace client {

// A first-fit allocator carving up a byte range somebody else owns.
//
// It exists to turn a RemoteHeap region -- one flat span of peer-backed
// addresses -- into something malloc() can be redirected into. It never reads
// or writes the range it manages: all bookkeeping lives in ordinary local
// memory. That is deliberate. Inline block headers would mean every malloc and
// every free touched a peer-backed page purely for accounting, so a program
// that allocated and freed without using the memory would still drag pages
// across the network.
//
// Thread-safe: the malloc it stands behind is called from every thread.
class HeapArena {
public:
    // Manages [base, base + size). Does not own or free it.
    HeapArena(void* base, size_t size);

    HeapArena(const HeapArena&) = delete;
    HeapArena& operator=(const HeapArena&) = delete;

    // Returns null when no free block is large enough. Callers are expected to
    // fall back to the real allocator rather than treat this as fatal.
    // alignment must be a power of two.
    void* allocate(size_t bytes, size_t alignment = 16);

    // Ignores null and addresses this arena did not hand out, so a hooked
    // free() can pass everything through one check.
    void deallocate(void* ptr);

    // Usable bytes of a live allocation, or 0 if this is not one of ours.
    size_t block_size(const void* ptr) const;

    bool owns(const void* ptr) const {
        const auto p = reinterpret_cast<uintptr_t>(ptr);
        return p >= base_ && p < base_ + size_;
    }

    struct Stats {
        size_t in_use_bytes = 0;
        size_t free_bytes = 0;
        size_t live_blocks = 0;
        size_t largest_free_block = 0;
    };
    Stats stats() const;

private:
    // Merges a newly freed span with any free neighbour. mutex_ held.
    void insert_free(uintptr_t offset, size_t length);

    uintptr_t base_ = 0;
    size_t size_ = 0;

    mutable std::mutex mutex_;
    // Both keyed by offset from base_ and kept ordered, which is what makes
    // coalescing a look at the two adjacent entries rather than a scan.
    std::map<uintptr_t, size_t> free_blocks_;
    std::map<uintptr_t, size_t> live_blocks_;
    size_t in_use_bytes_ = 0;
};

} // namespace client
} // namespace meminfo
