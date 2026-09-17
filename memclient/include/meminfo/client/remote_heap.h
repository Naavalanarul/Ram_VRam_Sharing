#pragma once
#include <meminfo/client/client_api.h>
#include <meminfo/common/types.h>
#include <meminfo/platform/IPageFaultBackend.h>

#include <cstddef>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace meminfo {
namespace client {

struct RemoteHeapConfig {
    // Size of the address range the heap hands out. May be far larger than the
    // local budget -- that is the point.
    size_t capacity_bytes = 0;

    // Resident bytes allowed before eviction starts. This is the figure the
    // process's memory usage plateaus at.
    size_t local_budget_bytes = 0;

    // Transfer granularity. Rounded up to a whole number of OS pages. Larger
    // pages mean fewer round-trips but more bytes moved per fault; on a LAN the
    // round-trip dominates, so the default is well above one OS page.
    size_t page_bytes = 64 * 1024;
};

// A region of ordinary memory whose contents live on a peer.
//
// Reads and writes are plain pointer accesses -- `p[i] = x` -- with no API to
// call. Touching a page that is not resident raises a fault, which is caught,
// answered by fetching that page from the peer, and resumed. When resident
// bytes exceed the local budget, the oldest pages are written back if dirty and
// then handed to the operating system, which is what makes the process's memory
// figure plateau instead of climbing.
//
// The backing store is an ordinary MemoryClient allocation, so the existing
// wire protocol, peer selection and striping all apply unchanged.
//
// Threading: several threads may touch the region at once. Faults are serviced
// under one lock, so concurrent faults queue rather than race.
//
// Reentrancy: the fault handler allocates (it buffers a page and talks to the
// peer). Anything that routes those allocations back into this heap -- the
// Detours malloc hook, for instance -- must break the cycle on its side with a
// per-thread guard, or the first fault recurses forever.
class RemoteHeap {
public:
    // Throws std::runtime_error when the platform has no usable page-fault
    // backend, when the address range cannot be reserved, or when the backing
    // allocation cannot be placed on a peer.
    RemoteHeap(MemoryClient& client, const RemoteHeapConfig& config);
    ~RemoteHeap();

    RemoteHeap(const RemoteHeap&) = delete;
    RemoteHeap& operator=(const RemoteHeap&) = delete;

    // Base of the region. Usable as an ordinary pointer.
    void* data() const { return base_; }
    size_t size() const { return capacity_; }
    size_t page_bytes() const { return page_bytes_; }

    struct Stats {
        size_t resident_bytes = 0;
        size_t fetches = 0;    // pages pulled from the peer
        size_t flushes = 0;    // dirty pages pushed to the peer
        size_t evictions = 0;  // pages handed back to the OS
    };
    Stats stats() const;

    // Pushes every dirty page to the peer, leaving the region resident.
    void flush();

    // Pushes every dirty page and then evicts everything, so the region's whole
    // contents are on the peer and nothing is held locally.
    void flush_and_evict_all();

    // True while the calling thread is inside a fault handler.
    //
    // Servicing a fault means buffering a page and talking to the peer, which
    // allocates. A malloc interposer that routes large allocations into a
    // RemoteHeap must send those allocations to the real allocator while this
    // is set: otherwise servicing a fault allocates out of the very heap it is
    // servicing, touches the new pages, faults again, and recurses until the
    // stack is gone.
    static bool servicing_fault();

private:
    void handle_fault(const platform::FaultInfo& info);
    // Evicts oldest-first until resident bytes fit the budget. heap_mutex_ held.
    void enforce_budget();
    // Writes one page back if dirty and forgets it. heap_mutex_ held.
    void evict_page(size_t page_index);
    void flush_page(size_t page_index);

    uint8_t* page_addr(size_t page_index) const { return base_ + page_index * page_bytes_; }
    size_t page_index_of(const void* addr) const;

    MemoryClient& client_;
    std::unique_ptr<platform::IPageFaultBackend> backend_;

    uint8_t* base_ = nullptr;
    size_t capacity_ = 0;
    size_t page_bytes_ = 0;
    size_t page_count_ = 0;
    size_t local_budget_bytes_ = 0;

    meminfo::handle_t handle_ = 0;

    mutable std::mutex heap_mutex_;
    // Resident pages, oldest first. Eviction takes from the front.
    std::list<size_t> resident_order_;
    std::unordered_map<size_t, std::list<size_t>::iterator> resident_;
    Stats stats_;
};

} // namespace client
} // namespace meminfo
