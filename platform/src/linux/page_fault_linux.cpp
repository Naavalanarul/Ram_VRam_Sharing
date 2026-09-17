#include <meminfo/platform/IPageFaultBackend.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstring>
#include <cerrno>
#include <vector>
#include <spdlog/spdlog.h>

namespace meminfo {
namespace platform {

// Linux page-fault backend built on userfaultfd in MISSING mode.
//
// Dirty tracking is conservative here: every resident page reports dirty.
// Distinguishing a load from a store after the page is present needs
// UFFD_FEATURE_PAGEFAULT_FLAG_WP, which is a newer-kernel feature and needs its
// own registration mode, and mixing MISSING with an mprotect/SIGSEGV scheme
// does not compose. Over-reporting costs a redundant flush on eviction;
// under-reporting would silently lose writes, so this errs the safe way.
//
// FaultInfo::is_write is still accurate for the fault that makes a page
// resident -- userfaultfd reports it -- it is only the second and later stores
// that cannot be observed.
class PageFaultBackendLinux : public IPageFaultBackend {
public:
    PageFaultBackendLinux() {
        long ps = sysconf(_SC_PAGESIZE);
        page_size_ = (ps > 0) ? static_cast<size_t>(ps) : 4096u;

        uffd_ = static_cast<int>(syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK));
        if (uffd_ == -1) {
            // Commonly EPERM: unprivileged userfaultfd is disabled by default
            // on many kernels. Not fatal -- callers check is_supported().
            spdlog::warn("userfaultfd unavailable: {}", std::strerror(errno));
            return;
        }

        struct uffdio_api api;
        std::memset(&api, 0, sizeof(api));
        api.api = UFFD_API;
        api.features = 0;
        if (ioctl(uffd_, UFFDIO_API, &api) == -1) {
            spdlog::error("ioctl UFFDIO_API failed");
            close(uffd_);
            uffd_ = -1;
            return;
        }
        
        running_ = true;
        fault_thread_ = std::thread(&PageFaultBackendLinux::fault_handler_thread, this);
    }

    ~PageFaultBackendLinux() override {
        running_ = false;
        if (fault_thread_.joinable()) {
            fault_thread_.join();
        }

        std::vector<void*> bases;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& kv : regions_) bases.push_back(kv.first);
        }
        for (void* base : bases) release_region(base);

        if (uffd_ != -1) {
            close(uffd_);
        }
    }

    bool is_supported() const override { return uffd_ != -1; }

    size_t page_size() const override { return page_size_; }

    PageRegion reserve_region(size_t bytes) override {
        if (uffd_ == -1 || bytes == 0) return {nullptr, 0};

        const size_t rounded = round_up(bytes, page_size_);

        void* addr = mmap(nullptr, rounded, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (addr == MAP_FAILED) {
            return {nullptr, 0};
        }

        struct uffdio_register reg;
        std::memset(&reg, 0, sizeof(reg));
        reg.range.start = reinterpret_cast<uint64_t>(addr);
        reg.range.len = rounded;
        reg.mode = UFFDIO_REGISTER_MODE_MISSING;

        if (ioctl(uffd_, UFFDIO_REGISTER, &reg) == -1) {
            spdlog::error("ioctl UFFDIO_REGISTER failed: {}", std::strerror(errno));
            munmap(addr, rounded);
            return {nullptr, 0};
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            Region region;
            region.size = rounded;
            region.resident.assign(rounded / page_size_, false);
            regions_.emplace(addr, std::move(region));
        }

        return {addr, rounded};
    }

    void release_region(void* addr) override {
        size_t size = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = regions_.find(addr);
            if (it == regions_.end()) return;
            size = it->second.size;
            for (bool r : it->second.resident) {
                if (r) --resident_pages_;
            }
            regions_.erase(it);
        }

        if (uffd_ != -1) {
            struct uffdio_range range;
            std::memset(&range, 0, sizeof(range));
            range.start = reinterpret_cast<uint64_t>(addr);
            range.len = size;
            ioctl(uffd_, UFFDIO_UNREGISTER, &range);
        }
        munmap(addr, size);
    }

    void on_fault(FaultHandler cb) override {
        user_cb_ = std::move(cb);
    }

    void resolve_fault(void* addr, const void* data, size_t len) override {
        if (uffd_ == -1 || addr == nullptr) {
            return;
        }

        // UFFDIO_COPY requires a page-aligned destination and a length that is
        // a whole number of pages. Callers hand us arbitrary addresses and
        // lengths, so stage the payload in a page-sized bounce buffer: align
        // the destination down, place the data at its offset within the page,
        // and zero-fill the remainder. Passing an unaligned dst or a partial
        // length makes the ioctl fail with EINVAL, which leaves the faulting
        // thread blocked forever.
        const uint64_t raw_dst = reinterpret_cast<uint64_t>(addr);
        const uint64_t aligned_dst = raw_dst & ~static_cast<uint64_t>(page_size_ - 1);
        const size_t offset_in_page = static_cast<size_t>(raw_dst - aligned_dst);

        // Round up to cover every page the payload touches.
        const size_t span = offset_in_page + len;
        const size_t total = round_up(span == 0 ? page_size_ : span, page_size_);

        std::vector<uint8_t> staged(total, 0);
        if (len > 0 && data != nullptr) {
            std::memcpy(staged.data() + offset_in_page, data, len);
        }

        struct uffdio_copy copy;
        std::memset(&copy, 0, sizeof(copy));
        copy.src = reinterpret_cast<uint64_t>(staged.data());
        copy.dst = aligned_dst;
        copy.len = total;
        copy.mode = 0;
        copy.copy = 0;

        // Record residency *before* the ioctl, not after. UFFDIO_COPY releases
        // the blocked faulting thread as part of the call, so a thread that
        // faulted can be running again -- and asking is_dirty() -- while this
        // one is still on its way to mark_resident(). Marking first closes that
        // window; the state is rolled back if the copy does not happen.
        void* const filled = reinterpret_cast<void*>(aligned_dst);
        mark_resident(filled, total, true);

        if (ioctl(uffd_, UFFDIO_COPY, &copy) == -1) {
            spdlog::error("ioctl UFFDIO_COPY failed: {}", std::strerror(errno));
            mark_resident(filled, total, false);
            return;
        }

        resolved_ = true;
    }

    // Conservative: resident means possibly written. See the class comment.
    bool is_dirty(void* addr, size_t len) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        uint8_t* page = page_base(addr);
        const size_t total = round_up(offset_in_page(addr) + len, page_size_);
        for (size_t off = 0; off < total; off += page_size_) {
            size_t index = 0;
            const Region* region = region_for(page + off, index);
            if (region && region->resident[index]) return true;
        }
        return false;
    }

    // No-op: without write tracking there is no clean state to return a page
    // to. evict_pages() drops residency, which is what ends the "dirty" answer.
    void clear_dirty(void* /*addr*/, size_t /*len*/) override {}

    bool evict_pages(void* addr, size_t len) override {
        uint8_t* page = page_base(addr);
        const size_t total = round_up(offset_in_page(addr) + len, page_size_);
        if (total == 0) return true;

        // MADV_DONTNEED on a MISSING-registered range frees the physical pages
        // and restores the hole, so the next touch raises a fresh userfaultfd
        // event. MADV_FREE would not: it leaves the page in place until the
        // kernel needs it, so the RSS figure would not move.
        if (madvise(page, total, MADV_DONTNEED) != 0) {
            spdlog::error("madvise(MADV_DONTNEED) at {} failed: {}",
                          static_cast<void*>(page), std::strerror(errno));
            return false;
        }

        mark_resident(page, total, false);
        return true;
    }

    size_t resident_pages() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return resident_pages_;
    }

private:
    struct Region {
        size_t size = 0;
        std::vector<bool> resident;
    };

    static size_t round_up(size_t value, size_t multiple) {
        return ((value + multiple - 1) / multiple) * multiple;
    }

    uint8_t* page_base(void* addr) const {
        auto raw = reinterpret_cast<uintptr_t>(addr);
        return reinterpret_cast<uint8_t*>(raw & ~static_cast<uintptr_t>(page_size_ - 1));
    }

    size_t offset_in_page(void* addr) const {
        return reinterpret_cast<uintptr_t>(addr) & (page_size_ - 1);
    }

    // mutex_ must be held.
    Region* region_for(void* addr, size_t& page_index) {
        auto it = regions_.upper_bound(addr);
        if (it == regions_.begin()) return nullptr;
        --it;

        auto base = reinterpret_cast<uintptr_t>(it->first);
        auto target = reinterpret_cast<uintptr_t>(addr);
        if (target < base || target >= base + it->second.size) return nullptr;

        page_index = (target - base) / page_size_;
        return &it->second;
    }

    const Region* region_for(void* addr, size_t& page_index) const {
        return const_cast<PageFaultBackendLinux*>(this)->region_for(addr, page_index);
    }

    void mark_resident(void* addr, size_t len, bool resident) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* page = static_cast<uint8_t*>(addr);
        for (size_t off = 0; off < len; off += page_size_) {
            size_t index = 0;
            Region* region = region_for(page + off, index);
            if (!region) continue;
            const bool was = region->resident[index];
            if (was == resident) continue;
            region->resident[index] = resident;
            resident_pages_ += resident ? 1 : static_cast<size_t>(-1);
        }
    }

    // Last-resort resolution so a faulting thread is never left blocked.
    void zero_page(void* addr) {
        const uint64_t aligned = reinterpret_cast<uint64_t>(addr) &
                                 ~static_cast<uint64_t>(page_size_ - 1);

        struct uffdio_zeropage zp;
        std::memset(&zp, 0, sizeof(zp));
        zp.range.start = aligned;
        zp.range.len = page_size_;
        zp.mode = 0;

        // Marked before the ioctl for the same reason as in resolve_fault():
        // the call is what unblocks the faulting thread.
        void* const filled = reinterpret_cast<void*>(aligned);
        mark_resident(filled, page_size_, true);

        if (ioctl(uffd_, UFFDIO_ZEROPAGE, &zp) == -1) {
            spdlog::error("ioctl UFFDIO_ZEROPAGE failed: {}", std::strerror(errno));
            mark_resident(filled, page_size_, false);
            return;
        }
    }

    void fault_handler_thread() {
        struct pollfd evt;
        evt.fd = uffd_;
        evt.events = POLLIN;

        while (running_) {
            int res = poll(&evt, 1, 100);
            if (res > 0 && (evt.revents & POLLIN)) {
                struct uffd_msg msg;
                if (read(uffd_, &msg, sizeof(msg)) == sizeof(msg)) {
                    if (msg.event == UFFD_EVENT_PAGEFAULT) {
                        void* fault_addr = reinterpret_cast<void*>(msg.arg.pagefault.address);
                        const bool is_write =
                            (msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WRITE) != 0;

                        // A fault that is never answered leaves the faulting
                        // thread blocked forever, so track whether the handler
                        // actually resolved it and zero-fill the page if not.
                        resolved_ = false;
                        if (user_cb_) {
                            user_cb_(FaultInfo{fault_addr, is_write});
                        }
                        if (!resolved_) {
                            zero_page(fault_addr);
                        }
                    }
                }
            }
        }
    }

    int uffd_ = -1;
    size_t page_size_ = 4096;
    std::thread fault_thread_;
    std::atomic<bool> running_{false};
    bool resolved_ = false; // only touched on the fault-handler thread

    mutable std::mutex mutex_;
    std::map<void*, Region> regions_;
    size_t resident_pages_ = 0;

    FaultHandler user_cb_;
};

std::unique_ptr<IPageFaultBackend> create_page_fault_backend() {
    return std::make_unique<PageFaultBackendLinux>();
}

} // namespace platform
} // namespace meminfo
