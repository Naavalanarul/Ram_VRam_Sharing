#include <meminfo/platform/IPageFaultBackend.h>
#include <windows.h>
#include <unordered_map>
#include <mutex>
#include <cstring>
#include <spdlog/spdlog.h>

namespace meminfo {
namespace platform {

class PageFaultBackendWin : public IPageFaultBackend {
public:
    PageFaultBackendWin() {
        veh_handle_ = AddVectoredExceptionHandler(1, &PageFaultBackendWin::VectoredHandler);
    }

    ~PageFaultBackendWin() override {
        if (veh_handle_) {
            RemoveVectoredExceptionHandler(veh_handle_);
        }
    }

    PageRegion reserve_region(size_t bytes) override {
        void* addr = VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
        if (!addr) {
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
        DWORD old_protect;
        VirtualProtect(addr, len, PAGE_READWRITE, &old_protect);
        if (data) {
            std::memcpy(addr, data, len);
        }
    }

private:
    static LONG WINAPI VectoredHandler(struct _EXCEPTION_POINTERS *ExceptionInfo) {
        if (ExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
            void* fault_addr = (void*)ExceptionInfo->ExceptionRecord->ExceptionInformation[1];
            
            if (instance_ && instance_->fault_handler_) {
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
                    instance_->fault_handler_(fault_addr);
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    PVOID veh_handle_ = nullptr;
    std::mutex mutex_;
    std::unordered_map<void*, size_t> regions_;
    FaultHandler fault_handler_;
    
    static PageFaultBackendWin* instance_;
    friend std::unique_ptr<IPageFaultBackend> create_page_fault_backend();
};

PageFaultBackendWin* PageFaultBackendWin::instance_ = nullptr;

std::unique_ptr<IPageFaultBackend> create_page_fault_backend() {
    auto ptr = std::make_unique<PageFaultBackendWin>();
    PageFaultBackendWin::instance_ = ptr.get();
    return ptr;
}

} // namespace platform
} // namespace meminfo
