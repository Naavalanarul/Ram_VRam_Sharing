#include <meminfo/platform/IPageFaultBackend.h>
#include <windows.h>
#include <map>
#include <mutex>
#include <vector>
#include <cstring>
#include <spdlog/spdlog.h>

namespace meminfo {
namespace platform {

namespace {

// Per-page residency and dirty state.
//
// A page starts ABSENT: address space is reserved but nothing is committed, so
// touching it raises an access violation. On a load fault it becomes RESIDENT
// and is mapped PAGE_READONLY, so the *next store* to it raises a second
// access violation -- which is exactly how the write is detected. That second
// fault promotes it to PAGE_READWRITE and to DIRTY. A page whose first fault
// was already a store goes straight to DIRTY.
enum class PageState : uint8_t {
    ABSENT = 0,   // no physical page; next access calls the fault handler
    RESIDENT,     // committed, read-only, unmodified since it arrived
    DIRTY,        // committed, writable, modified
};

} // namespace

// Windows page-fault backend built on a vectored exception handler.
//
// Faults are delivered on the faulting thread itself, before any SEH frame,
// which is what lets a plain `p[i] = x` against a reserved region be serviced
// transparently. The handler must therefore be reentrancy-safe with respect to
// the threads it interrupts; the only lock it takes is mutex_, and nothing
// holding mutex_ ever touches a tracked page.
class PageFaultBackendWin : public IPageFaultBackend {
public:
    PageFaultBackendWin() {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        page_size_ = si.dwPageSize ? static_cast<size_t>(si.dwPageSize) : 4096u;

        // instance_ must be published before the handler is registered. The
        // previous code assigned it in the factory, after the constructor had
        // already armed the VEH, leaving a window in which a fault would find a
        // null instance and be passed through as a crash.
        instance_ = this;

        // First in the chain: ours are ordinary access violations and another
        // handler installed by the host process (GIMP loads plenty of DLLs)
        // must not see them first.
        veh_handle_ = AddVectoredExceptionHandler(1, &PageFaultBackendWin::VectoredHandler);
        if (!veh_handle_) {
            instance_ = nullptr;
            spdlog::error("AddVectoredExceptionHandler failed: {}", GetLastError());
        }
    }

    ~PageFaultBackendWin() override {
        if (veh_handle_) {
            RemoveVectoredExceptionHandler(veh_handle_);
            veh_handle_ = nullptr;
        }
        instance_ = nullptr;

        std::vector<void*> bases;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& kv : regions_) bases.push_back(kv.first);
        }
        for (void* base : bases) release_region(base);
    }

    bool is_supported() const override { return veh_handle_ != nullptr; }

    size_t page_size() const override { return page_size_; }

    PageRegion reserve_region(size_t bytes) override {
        if (bytes == 0) return {nullptr, 0};

        const size_t rounded = round_up(bytes, page_size_);

        // MEM_RESERVE only. The original code passed MEM_COMMIT, which charges
        // the whole region against the commit limit up front and leaves nothing
        // for eviction to give back -- the opposite of the point. Pages are
        // committed one at a time as they are faulted in, and decommitted again
        // by evict_pages().
        void* addr = VirtualAlloc(nullptr, rounded, MEM_RESERVE, PAGE_NOACCESS);
        if (!addr) {
            spdlog::error("VirtualAlloc(MEM_RESERVE, {}) failed: {}", rounded, GetLastError());
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
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = regions_.find(addr);
            if (it == regions_.end()) return;
            resident_pages_ -= count_resident(it->second);
            regions_.erase(it);
        }
        VirtualFree(addr, 0, MEM_RELEASE);
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

        // Commit the pages. VirtualAlloc(MEM_COMMIT) on already-committed pages
        // succeeds and is the documented way to commit within a reservation.
        if (!VirtualAlloc(page, total, MEM_COMMIT, PAGE_READWRITE)) {
            spdlog::error("VirtualAlloc(MEM_COMMIT) at {} failed: {}",
                          static_cast<void*>(page), GetLastError());
            return;
        }

        if (data) {
            std::memcpy(static_cast<uint8_t*>(addr), data, len);
        } else {
            // Freshly committed pages are already zero; only a recommitted page
            // could carry stale bytes, and MEM_COMMIT zeroes those too.
            std::memset(page, 0, total);
        }

        // A load fault leaves the page read-only so that the next store faults
        // again and is recorded. A store fault has already happened, so the
        // page goes straight to writable and dirty -- re-protecting it would
        // only make the same instruction fault a second time.
        const bool write_fault = in_write_fault_;
        const PageState state = write_fault ? PageState::DIRTY : PageState::RESIDENT;
        if (!write_fault) {
            DWORD old_protect = 0;
            if (!VirtualProtect(page, total, PAGE_READONLY, &old_protect)) {
                // Losing write detection would mean losing writes, so treat the
                // page as dirty from here on rather than risk it.
                spdlog::warn("VirtualProtect(PAGE_READONLY) at {} failed: {}; "
                             "treating the page as dirty",
                             static_cast<void*>(page), GetLastError());
                set_states(page, total, PageState::DIRTY);
                resolved_ = true;
                return;
            }
        }

        set_states(page, total, state);
        resolved_ = true;
    }

    bool is_dirty(void* addr, size_t len) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return for_each_page_const(addr, len, [](PageState s) { return s == PageState::DIRTY; });
    }

    void clear_dirty(void* addr, size_t len) override {
        uint8_t* page = page_base(addr);
        const size_t total = round_up(offset_in_page(addr) + len, page_size_);

        // Back to read-only, so the next store faults and re-marks the page.
        // Without this a page flushed once would never be seen dirty again.
        DWORD old_protect = 0;
        if (!VirtualProtect(page, total, PAGE_READONLY, &old_protect)) {
            spdlog::warn("VirtualProtect(PAGE_READONLY) during clear_dirty at {} failed: {}",
                         static_cast<void*>(page), GetLastError());
            return; // leave it dirty: a redundant flush beats a lost write
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

        // MEM_DECOMMIT, not MEM_RELEASE: the reservation must survive so the
        // addresses stay ours and the next touch faults back through here
        // instead of landing on an unrelated allocation. This is the call that
        // hands the physical pages and the commit charge back to Windows, and
        // so the one that moves the number in Task Manager.
        if (!VirtualFree(page, total, MEM_DECOMMIT)) {
            spdlog::error("VirtualFree(MEM_DECOMMIT) at {} failed: {}",
                          static_cast<void*>(page), GetLastError());
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

    // Finds the region containing addr. mutex_ must be held.
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
        return const_cast<PageFaultBackendWin*>(this)->region_for(addr, page_index);
    }

    static size_t count_resident(const Region& region) {
        size_t n = 0;
        for (PageState s : region.states) {
            if (s != PageState::ABSENT) ++n;
        }
        return n;
    }

    // Applies fn to each tracked page's state in [addr, addr+len). mutex_ held.
    template <typename Fn>
    void for_each_page(uint8_t* addr, size_t len, Fn fn) {
        for (size_t off = 0; off < len; off += page_size_) {
            size_t index = 0;
            Region* region = region_for(addr + off, index);
            if (region) fn(region->states[index]);
        }
    }

    // True when fn holds for any tracked page in the range. mutex_ held.
    template <typename Fn>
    bool for_each_page_const(void* addr, size_t len, Fn fn) const {
        uint8_t* page = page_base(addr);
        const size_t total = round_up(offset_in_page(addr) + len, page_size_);
        for (size_t off = 0; off < total; off += page_size_) {
            size_t index = 0;
            const Region* region = region_for(page + off, index);
            if (region && fn(region->states[index])) return true;
        }
        return false;
    }

    void set_states(uint8_t* addr, size_t len, PageState state) {
        std::lock_guard<std::mutex> lock(mutex_);
        for_each_page(addr, len, [this, state](PageState& s) {
            if (s == PageState::ABSENT && state != PageState::ABSENT) ++resident_pages_;
            s = state;
        });
    }

    static LONG WINAPI VectoredHandler(struct _EXCEPTION_POINTERS* ExceptionInfo) {
        const auto* record = ExceptionInfo->ExceptionRecord;
        if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        PageFaultBackendWin* self = instance_;
        if (!self) return EXCEPTION_CONTINUE_SEARCH;

        // ExceptionInformation[0]: 0 read, 1 write, 8 DEP violation.
        // ExceptionInformation[1]: the inaccessible address.
        const bool is_write = record->ExceptionInformation[0] == 1;
        void* fault_addr = reinterpret_cast<void*>(record->ExceptionInformation[1]);

        PageState state;
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            size_t index = 0;
            const Region* region = self->region_for(fault_addr, index);
            if (!region) {
                return EXCEPTION_CONTINUE_SEARCH; // not one of ours: a real crash
            }
            state = region->states[index];
        }

        // A store to a page that is already resident is the write half of the
        // read/write split: the contents are present, only the protection needs
        // lifting. The user callback is not involved -- there is nothing to
        // fetch -- which is why a fault reaching it always means "absent".
        if (state != PageState::ABSENT) {
            if (!is_write) {
                // Resident and readable, yet it faulted on a load. Something
                // outside this backend changed the mapping; do not mask it.
                return EXCEPTION_CONTINUE_SEARCH;
            }
            uint8_t* page = self->page_base(fault_addr);
            DWORD old_protect = 0;
            if (!VirtualProtect(page, self->page_size_, PAGE_READWRITE, &old_protect)) {
                return EXCEPTION_CONTINUE_SEARCH;
            }
            self->set_states(page, self->page_size_, PageState::DIRTY);
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        FaultHandler handler;
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            handler = self->fault_handler_;
        }
        if (!handler) return EXCEPTION_CONTINUE_SEARCH;

        // in_write_fault_ tells resolve_fault() which protection to leave
        // behind. It is thread-local because the VEH runs on whichever thread
        // faulted, and several may be in here at once.
        const bool saved_write = in_write_fault_;
        const bool saved_resolved = resolved_;
        in_write_fault_ = is_write;
        resolved_ = false;

        handler(FaultInfo{fault_addr, is_write});

        const bool resolved = resolved_;
        in_write_fault_ = saved_write;
        resolved_ = saved_resolved;

        if (!resolved) {
            // The handler declined to fill the page. Resuming would re-execute
            // the same instruction and fault forever, so let the exception
            // through and fail loudly instead of hanging.
            return EXCEPTION_CONTINUE_SEARCH;
        }

        return EXCEPTION_CONTINUE_EXECUTION;
    }

    PVOID veh_handle_ = nullptr;
    size_t page_size_ = 4096;

    mutable std::mutex mutex_;
    // Ordered by base address so region_for() is a lookup rather than a scan of
    // every region on every fault.
    std::map<void*, Region> regions_;
    size_t resident_pages_ = 0;
    FaultHandler fault_handler_;

    static PageFaultBackendWin* instance_;
    static thread_local bool in_write_fault_;
    static thread_local bool resolved_;
};

PageFaultBackendWin* PageFaultBackendWin::instance_ = nullptr;
thread_local bool PageFaultBackendWin::in_write_fault_ = false;
thread_local bool PageFaultBackendWin::resolved_ = false;

std::unique_ptr<IPageFaultBackend> create_page_fault_backend() {
    return std::make_unique<PageFaultBackendWin>();
}

} // namespace platform
} // namespace meminfo
