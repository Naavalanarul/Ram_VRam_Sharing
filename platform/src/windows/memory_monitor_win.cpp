#include <meminfo/platform/IMemoryMonitor.h>
#include <windows.h>
#include <thread>
#include <atomic>

namespace meminfo {
namespace platform {

class MemoryMonitorWin : public IMemoryMonitor {
public:
    MemoryMonitorWin() {
        running_ = true;
        monitor_thread_ = std::thread([this]() {
            while (running_) {
                Sleep(2000);
                if (callback_) {
                    auto stats = get_stats();
                    if (stats.available_bytes < 500 * 1024 * 1024) {
                        callback_();
                    }
                }
            }
        });
    }

    ~MemoryMonitorWin() override {
        running_ = false;
        if (monitor_thread_.joinable()) {
            monitor_thread_.join();
        }
    }

    MemoryStats get_stats() override {
        MemoryStats stats = {0, 0, 0};
        MEMORYSTATUSEX memInfo;
        memInfo.dwLength = sizeof(MEMORYSTATUSEX);
        if (GlobalMemoryStatusEx(&memInfo)) {
            stats.total_bytes = memInfo.ullTotalPhys;
            stats.free_bytes = memInfo.ullAvailPhys;
            stats.available_bytes = memInfo.ullAvailPhys;
        }
        return stats;
    }

    void set_pressure_callback(PressureCallback cb) override {
        callback_ = std::move(cb);
    }

private:
    PressureCallback callback_;
    std::thread monitor_thread_;
    std::atomic<bool> running_{false};
};

std::unique_ptr<IMemoryMonitor> create_memory_monitor() {
    return std::make_unique<MemoryMonitorWin>();
}

} // namespace platform
} // namespace meminfo
