#pragma once
#include <cstddef>
#include <functional>
#include <memory>

namespace meminfo {
namespace platform {

struct PageRegion {
    void* addr;
    size_t size;
};

// What the backend knows about a fault when it hands it to the user callback.
struct FaultInfo {
    // The faulting address exactly as the OS reported it. Not page-aligned;
    // round it down with page_size() to find the page to fill.
    void* addr;
    // True when the access was a store. The backend uses this to decide whether
    // the resolved page may stay read-only (so a later store is observable) or
    // must be writable immediately.
    bool is_write;
};

class IPageFaultBackend {
public:
    // False when the OS cannot provide fault handling in this process --
    // notably Linux userfaultfd, which needs CAP_SYS_PTRACE unless
    // vm.unprivileged_userfaultfd is enabled. Callers should treat the backend
    // as unavailable rather than assume reserve_region() will succeed.
    virtual bool is_supported() const = 0;

    // OS page size. Fault addresses and eviction ranges are handled at this
    // granularity.
    virtual size_t page_size() const = 0;

    // Reserves address space without backing it. No physical memory is
    // consumed until a fault is resolved, which is the point: the region may be
    // far larger than the local RAM budget.
    virtual PageRegion reserve_region(size_t bytes) = 0;

    // Unmaps a region previously returned by reserve_region().
    virtual void release_region(void* addr) = 0;

    // Called when a page with no contents is touched. The implementation blocks
    // the faulting thread and invokes this callback, which is expected to call
    // resolve_fault() before returning.
    //
    // A store to a page that is already resident but read-only does NOT reach
    // this callback: the backend promotes it to writable and records it dirty
    // on its own. So a call here always means "this page's contents are not
    // present -- fetch them".
    using FaultHandler = std::function<void(const FaultInfo&)>;
    virtual void on_fault(FaultHandler cb) = 0;

    // Resolves the fault by mapping 'data' into 'addr'. Passing null data fills
    // the page with zeroes. The page is left writable and marked dirty when the
    // fault was a store, and read-only and clean when it was a load.
    virtual void resolve_fault(void* addr, const void* data, size_t len) = 0;

    // True when any page in [addr, addr+len) has been written since it was last
    // made resident or cleared.
    //
    // Backends that cannot distinguish a load from a store over-report rather
    // than under-report: a clean page wrongly called dirty costs a redundant
    // flush, while a dirty page wrongly called clean loses the write. See the
    // per-backend notes for which is which.
    virtual bool is_dirty(void* addr, size_t len) const = 0;

    // Forgets the dirty state of [addr, addr+len). Call after flushing.
    virtual void clear_dirty(void* addr, size_t len) = 0;

    // Returns the physical pages backing [addr, addr+len) to the operating
    // system and re-arms faulting on them, so the next access calls the fault
    // handler again. This is the step that actually moves the process's memory
    // figure; simply forgetting a page in userspace does not.
    //
    // Any dirty contents must be flushed first -- this discards them.
    virtual bool evict_pages(void* addr, size_t len) = 0;

    // Pages currently backed by physical memory across all regions. Useful for
    // asserting that eviction really happened.
    virtual size_t resident_pages() const = 0;

    virtual ~IPageFaultBackend() = default;
};

// Factory
std::unique_ptr<IPageFaultBackend> create_page_fault_backend();

} // namespace platform
} // namespace meminfo
