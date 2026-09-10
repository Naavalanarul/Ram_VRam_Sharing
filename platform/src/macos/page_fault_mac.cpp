#include <meminfo/platform/IPageFaultBackend.h>
#include <sys/mman.h>
#include <signal.h>
#include <spdlog/spdlog.h>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace meminfo {
namespace platform {

class PageFaultBackendMac : public IPageFaultBackend {
public:
    PageFaultBackendMac() {
        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = &PageFaultBackendMac::segv_handler;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, nullptr);
        sigaction(SIGBUS, &sa, nullptr);
    }

    ~PageFaultBackendMac() override {
        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_DFL;
        sigaction(SIGSEGV, &sa, nullptr);
        sigaction(SIGBUS, &sa, nullptr);
    }

    bool is_supported() const override { return true; }

    PageRegion reserve_region(size_t bytes) override {
        void* addr = mmap(nullptr, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (addr == MAP_FAILED) {
            return {nullptr, 0};
        }
        
        {
            std::lock_guard<std::mutex> lock(mutex_);
            regions_[addr] = bytes;
        }
        
        return {addr, bytes};
    }

    void on_fault(FaultHandler cb) override {
        std::lock_guard<std::mutex> lock(mutex_);
        fault_handler_ = std::move(cb);
    }

    void resolve_fault(void* addr, const void* data, size_t len) override {
        // Change protection to READ|WRITE
        mprotect(addr, len, PROT_READ | PROT_WRITE);
        if (data) {
            std::memcpy(addr, data, len);
        }
    }

private:
    static void segv_handler(int sig, siginfo_t* si, void* /*unused*/) {
        if (sig == SIGSEGV || sig == SIGBUS) {
            void* fault_addr = si->si_addr;
            if (instance_ && instance_->fault_handler_) {
                // Determine if it's within our regions
                bool ours = false;
                {
                    std::lock_guard<std::mutex> lock(instance_->mutex_);
                    for (const auto& kv : instance_->regions_) {
                        uintptr_t base = reinterpret_cast<uintptr_t>(kv.first);
                        uintptr_t fault = reinterpret_cast<uintptr_t>(fault_addr);
                        if (fault >= base && fault < base + kv.second) {
                            ours = true;
                            break;
                        }
                    }
                }
                
                if (ours) {
                    // Synchronously handle the fault.
                    // This blocks the faulting thread, which is exactly what we want.
                    instance_->fault_handler_(fault_addr);
                    return;
                }
            }
        }
        
        // If not handled, restore default handler and reraise to crash normally
        struct sigaction sa;
        std::memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_DFL;
        sigaction(SIGSEGV, &sa, nullptr);
    }

    std::mutex mutex_;
    std::unordered_map<void*, size_t> regions_;
    FaultHandler fault_handler_;
    
    // Singleton pointer for signal handler
    static PageFaultBackendMac* instance_;
    friend std::unique_ptr<IPageFaultBackend> create_page_fault_backend();
};

PageFaultBackendMac* PageFaultBackendMac::instance_ = nullptr;

std::unique_ptr<IPageFaultBackend> create_page_fault_backend() {
    auto ptr = std::make_unique<PageFaultBackendMac>();
    PageFaultBackendMac::instance_ = ptr.get();
    return ptr;
}

} // namespace platform
} // namespace meminfo
