#include <meminfo/platform/IPageFaultBackend.h>
#include <sys/mman.h>
#include <unistd.h>
#include <signal.h>
#include <spdlog/spdlog.h>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace meminfo {
namespace platform {

namespace {
enum class PageState : uint8_t {
    ABSENT = 0,   // PROT_NONE; next access calls the fault handler
    RESIDENT,     // PROT_READ, unmodified since it arrived
    DIRTY,        // PROT_READ|PROT_WRITE, modified
};
} // namespace

// macOS page-fault backend built on a SIGSEGV/SIGBUS handler and mprotect.
//
// Write detection works the same way as on Windows: a page filled by a load
// fault is left PROT_READ, so the next store traps and promotes it. What the
// signal handler cannot do portably is tell a load trap from a store trap --
// that lives in the machine-specific ucontext -- so a trap on a page that is
// already resident is taken to be a store. The only way to trap a resident
// read-only page is to write to it, so in practice that inference holds; where
// it does not, the cost is a page called dirty that was not, which means one
// redundant flush rather than a lost write.
//
// This is the least-exercised of the three backends: the target platform for
// this work is Windows, and nothing in the test suite runs on macOS.
class PageFaultBackendMac : public IPageFaultBackend {
public:
    PageFaultBackendMac() {
        long ps = sysconf(_SC_PAGESIZE);
        page_size_ = (ps > 0) ? static_cast<size_t>(ps) : 4096u;

        // Publish before installing the handler: a trap arriving in between
        // would otherwise find a null instance.
        instance_ = this;

        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = &PageFaultBackendMac::segv_handler;
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, &prev_segv_);
        sigaction(SIGBUS, &sa, &prev_bus_);
        installed_ = true;
    }

    ~PageFaultBackendMac() override {
        std::vector<void*> bases;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& kv : regions_) bases.push_back(kv.first);
        }
        for (void* base : bases) release_region(base);

        if (installed_) {
            sigaction(SIGSEGV, &prev_segv_, nullptr);
            sigaction(SIGBUS, &prev_bus_, nullptr);
            installed_ = false;
        }
        instance_ = nullptr;
    }

    bool is_supported() const override { return installed_; }

    size_t page_size() const override { return page_size_; }

    PageRegion reserve_region(size_t bytes) override {
        if (bytes == 0) return {nullptr, 0};
        const size_t rounded = round_up(bytes, page_size_);

        void* addr = mmap(nullptr, rounded, PROT_NONE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (addr == MAP_FAILED) {
            return {nullptr, 0};
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            Region region;
            region.size = rounded;
            region.states.assign(rounded / page_size_, PageState::ABSENT);
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
            for (PageState s : it->second.states) {
                if (s != PageState::ABSENT) --resident_pages_;
            }
            regions_.erase(it);
        }
        munmap(addr, size);
    }

    void on_fault(FaultHandler cb) override {
        std::lock_guard<std::mutex> lock(mutex_);
        fault_handler_ = std::move(cb);
    }

    void resolve_fault(void* addr, const void* data, size_t len) override {
        if (!addr) return;

        uint8_t* page = page_base(addr);
        const size_t span = offset_in_page(addr) + len;
        const size_t total = round_up(span == 0 ? page_size_ : span, page_size_);

        if (mprotect(page, total, PROT_READ | PROT_WRITE) != 0) {
            spdlog::error("mprotect(RW) at {} failed: {}", static_cast<void*>(page),
                          std::strerror(errno));
            return;
        }

        if (data) {
            std::memcpy(static_cast<uint8_t*>(addr), data, len);
        } else {
            std::memset(page, 0, total);
        }

        const bool write_fault = in_write_fault_;
        if (!write_fault && mprotect(page, total, PROT_READ) != 0) {
            // Losing write detection would lose writes; call it dirty instead.
            set_states(page, total, PageState::DIRTY);
            resolved_ = true;
            return;
        }

        set_states(page, total, write_fault ? PageState::DIRTY : PageState::RESIDENT);
        resolved_ = true;
    }

    bool is_dirty(void* addr, size_t len) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        uint8_t* page = page_base(addr);
        const size_t total = round_up(offset_in_page(addr) + len, page_size_);
        for (size_t off = 0; off < total; off += page_size_) {
            size_t index = 0;
            const Region* region = region_for(page + off, index);
            if (region && region->states[index] == PageState::DIRTY) return true;
        }
        return false;
    }

    void clear_dirty(void* addr, size_t len) override {
        uint8_t* page = page_base(addr);
        const size_t total = round_up(offset_in_page(addr) + len, page_size_);
        if (mprotect(page, total, PROT_READ) != 0) {
            return; // stay dirty: a redundant flush beats a lost write
        }

        std::lock_guard<std::mutex> lock(mutex_);
        for_each_page(page, total, [](PageState& s) {
            if (s == PageState::DIRTY) s = PageState::RESIDENT;
        });
    }

    bool evict_pages(void* addr, size_t len) override {
        uint8_t* page = page_base(addr);
        const size_t total = round_up(offset_in_page(addr) + len, page_size_);
        if (total == 0) return true;

        // MADV_FREE alone leaves the pages mapped until the kernel wants them
        // back, so the process's footprint would not move. Dropping them and
        // taking the mapping to PROT_NONE both frees them and re-arms the trap.
        madvise(page, total, MADV_FREE);
        if (mprotect(page, total, PROT_NONE) != 0) {
            spdlog::error("mprotect(PROT_NONE) at {} failed: {}", static_cast<void*>(page),
                          std::strerror(errno));
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        for_each_page(page, total, [this](PageState& s) {
            if (s != PageState::ABSENT) --resident_pages_;
            s = PageState::ABSENT;
        });
        return true;
    }

    size_t resident_pages() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return resident_pages_;
    }

private:
    struct Region {
        size_t size = 0;
        std::vector<PageState> states;
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
        return const_cast<PageFaultBackendMac*>(this)->region_for(addr, page_index);
    }

    template <typename Fn>
    void for_each_page(uint8_t* addr, size_t len, Fn fn) {
        for (size_t off = 0; off < len; off += page_size_) {
            size_t index = 0;
            Region* region = region_for(addr + off, index);
            if (region) fn(region->states[index]);
        }
    }

    void set_states(uint8_t* addr, size_t len, PageState state) {
        std::lock_guard<std::mutex> lock(mutex_);
        for_each_page(addr, len, [this, state](PageState& s) {
            if (s == PageState::ABSENT && state != PageState::ABSENT) ++resident_pages_;
            s = state;
        });
    }

    static void segv_handler(int sig, siginfo_t* si, void* /*ucontext*/) {
        PageFaultBackendMac* self = instance_;
        if (!self || !si) {
            reraise(sig);
            return;
        }

        void* fault_addr = si->si_addr;

        PageState state;
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            size_t index = 0;
            const Region* region = self->region_for(fault_addr, index);
            if (!region) {
                reraise(sig); // not ours: a genuine fault
                return;
            }
            state = region->states[index];
        }

        // Resident already: the page has contents and only its protection is in
        // the way, so this is the store half of the read/write split. Promote
        // it and return without involving the user callback.
        if (state != PageState::ABSENT) {
            uint8_t* page = self->page_base(fault_addr);
            if (mprotect(page, self->page_size_, PROT_READ | PROT_WRITE) != 0) {
                reraise(sig);
                return;
            }
            self->set_states(page, self->page_size_, PageState::DIRTY);
            return;
        }

        FaultHandler handler;
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            handler = self->fault_handler_;
        }
        if (!handler) {
            reraise(sig);
            return;
        }

        const bool saved_write = in_write_fault_;
        const bool saved_resolved = resolved_;
        // An absent page traps on a load as readily as on a store, and the
        // access type is not in siginfo_t, so assume a load: the page is left
        // read-only and a following store simply traps again and is recorded.
        in_write_fault_ = false;
        resolved_ = false;

        handler(FaultInfo{fault_addr, false});

        const bool resolved = resolved_;
        in_write_fault_ = saved_write;
        resolved_ = saved_resolved;

        if (!resolved) {
            reraise(sig); // resuming would loop on the same instruction forever
        }
    }

    // Restores the default disposition and re-raises, so an unrelated fault
    // still produces the crash it should instead of spinning in this handler.
    static void reraise(int sig) {
        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_DFL;
        sigemptyset(&sa.sa_mask);
        sigaction(sig, &sa, nullptr);
        raise(sig);
    }

    size_t page_size_ = 4096;
    bool installed_ = false;
    struct sigaction prev_segv_ {};
    struct sigaction prev_bus_ {};

    mutable std::mutex mutex_;
    std::map<void*, Region> regions_;
    size_t resident_pages_ = 0;
    FaultHandler fault_handler_;

    static PageFaultBackendMac* instance_;
    static thread_local bool in_write_fault_;
    static thread_local bool resolved_;
};

PageFaultBackendMac* PageFaultBackendMac::instance_ = nullptr;
thread_local bool PageFaultBackendMac::in_write_fault_ = false;
thread_local bool PageFaultBackendMac::resolved_ = false;

std::unique_ptr<IPageFaultBackend> create_page_fault_backend() {
    return std::make_unique<PageFaultBackendMac>();
}

} // namespace platform
} // namespace meminfo
