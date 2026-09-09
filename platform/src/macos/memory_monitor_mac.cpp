#include <meminfo/platform/IMemoryMonitor.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <dispatch/dispatch.h>
#include <sys/sysctl.h>
#include <spdlog/spdlog.h>

namespace meminfo {
namespace platform {

class MemoryMonitorMac : public IMemoryMonitor {
public:
    MemoryMonitorMac() {
        queue_ = dispatch_queue_create("com.meminfo.memorypressure", DISPATCH_QUEUE_SERIAL);
        source_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0, 
            DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL, queue_);
        
        dispatch_source_set_event_handler(source_, ^{
            dispatch_source_memorypressure_flags_t pressure = dispatch_source_get_data(source_);
            spdlog::warn("macOS memory pressure event: {}", pressure);
            if (callback_) {
                callback_();
            }
        });
        dispatch_resume(source_);
    }

    ~MemoryMonitorMac() override {
        if (source_) {
            dispatch_source_cancel(source_);
            dispatch_release(source_);
        }
        if (queue_) {
            dispatch_release(queue_);
        }
    }

    MemoryStats get_stats() override {
        MemoryStats stats = {0, 0, 0};
        
        mach_port_t host_port = mach_host_self();
        mach_msg_type_number_t host_size = sizeof(vm_statistics64_data_t) / sizeof(integer_t);
        vm_size_t pagesize;
        vm_statistics64_data_t vm_stat;
        
        host_page_size(host_port, &pagesize);
        
        if (host_statistics64(host_port, HOST_VM_INFO64, (host_info64_t)&vm_stat, &host_size) == KERN_SUCCESS) {
            stats.free_bytes = vm_stat.free_count * pagesize;
            stats.available_bytes = (vm_stat.free_count + vm_stat.inactive_count) * pagesize;
            
            // Getting total RAM using sysctl is better, but this is an approximation for now.
            // A more accurate way is sysctl("hw.memsize")
            int64_t physical_memory;
            size_t len = sizeof(physical_memory);
            sysctlbyname("hw.memsize", &physical_memory, &len, nullptr, 0);
            stats.total_bytes = physical_memory;
        }
        return stats;
    }

    void set_pressure_callback(PressureCallback cb) override {
        callback_ = std::move(cb);
    }

private:
    dispatch_queue_t queue_ = nullptr;
    dispatch_source_t source_ = nullptr;
    PressureCallback callback_;
};

std::unique_ptr<IMemoryMonitor> create_memory_monitor() {
    return std::make_unique<MemoryMonitorMac>();
}

} // namespace platform
} // namespace meminfo
