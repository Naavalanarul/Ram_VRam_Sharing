#pragma once
#include <cstddef>
#include <functional>
#include <memory>

namespace meminfo {
namespace platform {

struct MemoryStats {
    size_t total_bytes;
    size_t free_bytes;
    size_t available_bytes;
};

class IMemoryMonitor {
public:
    virtual MemoryStats get_stats() = 0;
    
    // Fires when free RAM crosses a critical threshold
    using PressureCallback = std::function<void()>;
    virtual void set_pressure_callback(PressureCallback cb) = 0;
    
    virtual ~IMemoryMonitor() = default;
};

// Factory
std::unique_ptr<IMemoryMonitor> create_memory_monitor();

} // namespace platform
} // namespace meminfo
