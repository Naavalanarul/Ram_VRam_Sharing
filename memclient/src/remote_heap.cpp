#include <meminfo/client/remote_heap.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace meminfo {
namespace client {

namespace {
size_t round_up(size_t value, size_t multiple) {
    return ((value + multiple - 1) / multiple) * multiple;
}

// Per-thread, because a fault is serviced on whichever thread took it: on
// Windows the vectored handler runs on the faulting thread itself.
thread_local int g_fault_depth = 0;

struct FaultScope {
    FaultScope() { ++g_fault_depth; }
    ~FaultScope() { --g_fault_depth; }
};
} // namespace

bool RemoteHeap::servicing_fault() { return g_fault_depth > 0; }

RemoteHeap::RemoteHeap(MemoryClient& client, const RemoteHeapConfig& config)
    : client_(client) {
    if (config.capacity_bytes == 0) {
        throw std::runtime_error("RemoteHeap: capacity_bytes must be non-zero");
    }

    backend_ = platform::create_page_fault_backend();
    if (!backend_ || !backend_->is_supported()) {
        // Linux userfaultfd needs a capability most machines withhold; Windows
        // needs the vectored handler to install. Either way there is no way to
        // service a fault, so the region would just crash on first touch.
        throw std::runtime_error("RemoteHeap: no usable page-fault backend on this system");
    }

    const size_t os_page = backend_->page_size();
    page_bytes_ = round_up(std::max<size_t>(config.page_bytes, os_page), os_page);
    capacity_ = round_up(config.capacity_bytes, page_bytes_);
    page_count_ = capacity_ / page_bytes_;

    // A budget below one page cannot hold the page being faulted in, so
    // eviction would fight the fault it is servicing.
    local_budget_bytes_ = std::max(round_up(config.local_budget_bytes, page_bytes_), page_bytes_);

    // The backing block must be too large for the client's local cache,
    // otherwise MemoryClient keeps it in process memory and nothing is saved --
    // the heap would work, and hold every byte locally twice over.
    if (capacity_ <= client_.max_local_bytes()) {
        throw std::runtime_error(
            "RemoteHeap: capacity (" + std::to_string(capacity_) +
            " bytes) does not exceed the client's local cache (" +
            std::to_string(client_.max_local_bytes()) +
            " bytes), so the backing block would stay in local memory");
    }

    auto region = backend_->reserve_region(capacity_);
    if (!region.addr) {
        throw std::runtime_error("RemoteHeap: could not reserve " +
                                 std::to_string(capacity_) + " bytes of address space");
    }
    base_ = static_cast<uint8_t*>(region.addr);
    capacity_ = region.size;
    page_count_ = capacity_ / page_bytes_;

    try {
        handle_ = client_.allocate(capacity_);
    } catch (...) {
        backend_->release_region(base_);
        base_ = nullptr;
        throw;
    }

    backend_->on_fault([this](const platform::FaultInfo& info) { handle_fault(info); });

    spdlog::info("RemoteHeap: {} bytes at {} backed by handle {}, {} byte pages, {} byte local budget",
                 capacity_, static_cast<void*>(base_), handle_, page_bytes_, local_budget_bytes_);
}

RemoteHeap::~RemoteHeap() {
    // Order matters: stop faulting before the backing store goes away, or a
    // fault arriving during teardown would read from a freed handle.
    if (backend_) {
        backend_->on_fault(nullptr);
        if (base_) backend_->release_region(base_);
    }
    base_ = nullptr;

    if (handle_ != 0) {
        try {
            client_.free(handle_);
        } catch (const std::exception& e) {
            spdlog::warn("RemoteHeap: failed to release backing handle {}: {}", handle_, e.what());
        }
        handle_ = 0;
    }
}

size_t RemoteHeap::page_index_of(const void* addr) const {
    const auto offset = static_cast<size_t>(static_cast<const uint8_t*>(addr) - base_);
    return offset / page_bytes_;
}

void RemoteHeap::handle_fault(const platform::FaultInfo& info) {
    const FaultScope scope;

    const auto* addr = static_cast<const uint8_t*>(info.addr);
    if (addr < base_ || addr >= base_ + capacity_) {
        return; // not ours; the backend passes the fault on when nothing resolves it
    }

    const size_t index = page_index_of(info.addr);
    const size_t offset = index * page_bytes_;
    const size_t length = std::min(page_bytes_, capacity_ - offset);

    std::vector<uint8_t> bytes;
    try {
        bytes = client_.read(handle_, offset, length);
    } catch (const std::exception& e) {
        // Leaving the fault unresolved is the honest outcome: the backend lets
        // the access violation through rather than resuming into a page whose
        // contents are wrong or absent.
        spdlog::error("RemoteHeap: fetching page {} (offset {}) failed: {}", index, offset, e.what());
        return;
    }

    if (bytes.size() != length) {
        spdlog::error("RemoteHeap: page {} came back as {} bytes, expected {}",
                      index, bytes.size(), length);
        return;
    }

    std::lock_guard<std::mutex> lock(heap_mutex_);

    backend_->resolve_fault(page_addr(index), bytes.data(), bytes.size());

    if (resident_.find(index) == resident_.end()) {
        resident_order_.push_back(index);
        resident_[index] = std::prev(resident_order_.end());
        stats_.resident_bytes += length;
    }
    ++stats_.fetches;

    enforce_budget();
}

void RemoteHeap::enforce_budget() {
    // Evict oldest-first until the resident set fits, but never evict the page
    // that was just faulted in -- it is the last entry, and the faulting
    // instruction is about to touch it.
    while (stats_.resident_bytes > local_budget_bytes_ && resident_order_.size() > 1) {
        evict_page(resident_order_.front());
    }
}

void RemoteHeap::flush_page(size_t page_index) {
    const size_t offset = page_index * page_bytes_;
    const size_t length = std::min(page_bytes_, capacity_ - offset);
    uint8_t* addr = page_addr(page_index);

    if (!backend_->is_dirty(addr, length)) {
        return;
    }

    const std::vector<uint8_t> bytes(addr, addr + length);
    try {
        client_.write(handle_, offset, bytes);
    } catch (const std::exception& e) {
        // Do not clear the dirty flag: the page still differs from the peer's
        // copy, and a later flush must try again.
        spdlog::error("RemoteHeap: flushing page {} (offset {}) failed: {}",
                      page_index, offset, e.what());
        throw;
    }

    backend_->clear_dirty(addr, length);
    ++stats_.flushes;
}

void RemoteHeap::evict_page(size_t page_index) {
    auto it = resident_.find(page_index);
    if (it == resident_.end()) return;

    const size_t offset = page_index * page_bytes_;
    const size_t length = std::min(page_bytes_, capacity_ - offset);

    try {
        flush_page(page_index);
    } catch (const std::exception&) {
        // Keeping an unflushed page resident wastes local memory; dropping it
        // would lose the write. Keep it, and stop trying to evict this round --
        // otherwise the caller spins on a page that cannot be written back.
        resident_order_.splice(resident_order_.end(), resident_order_, it->second);
        return;
    }

    if (!backend_->evict_pages(page_addr(page_index), length)) {
        spdlog::warn("RemoteHeap: could not release page {} to the OS", page_index);
        resident_order_.splice(resident_order_.end(), resident_order_, it->second);
        return;
    }

    resident_order_.erase(it->second);
    resident_.erase(it);
    stats_.resident_bytes -= length;
    ++stats_.evictions;
}

void RemoteHeap::flush() {
    std::lock_guard<std::mutex> lock(heap_mutex_);
    for (size_t index : resident_order_) {
        flush_page(index);
    }
}

void RemoteHeap::flush_and_evict_all() {
    std::lock_guard<std::mutex> lock(heap_mutex_);
    while (!resident_order_.empty()) {
        const size_t index = resident_order_.front();
        const size_t before = resident_order_.size();
        evict_page(index);
        if (resident_order_.size() == before) {
            // evict_page() moved it to the back instead of removing it, which
            // means it could not be written back. Stop rather than loop.
            spdlog::warn("RemoteHeap: giving up on evicting page {}", index);
            break;
        }
    }
}

RemoteHeap::Stats RemoteHeap::stats() const {
    std::lock_guard<std::mutex> lock(heap_mutex_);
    return stats_;
}

} // namespace client
} // namespace meminfo
