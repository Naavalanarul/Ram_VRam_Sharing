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

class IPageFaultBackend {
public:
    // False when the OS cannot provide fault handling in this process --
    // notably Linux userfaultfd, which needs CAP_SYS_PTRACE unless
    // vm.unprivileged_userfaultfd is enabled. Callers should treat the backend
    // as unavailable rather than assume reserve_region() will succeed.
    virtual bool is_supported() const = 0;

    virtual PageRegion reserve_region(size_t bytes) = 0;
    
    // Called when a reserved page is touched. 
    // The implementation should block the faulting thread and invoke this callback.
    using FaultHandler = std::function<void(void* fault_addr)>;
    virtual void on_fault(FaultHandler cb) = 0;
    
    // Resolves the fault by mapping 'data' into 'addr'.
    virtual void resolve_fault(void* addr, const void* data, size_t len) = 0;
    
    virtual ~IPageFaultBackend() = default;
};

// Factory
std::unique_ptr<IPageFaultBackend> create_page_fault_backend();

} // namespace platform
} // namespace meminfo
