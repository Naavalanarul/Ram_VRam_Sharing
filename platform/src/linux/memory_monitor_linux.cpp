#include <meminfo/platform/IMemoryMonitor.h>
#include <fstream>
#include <string>
#include <sstream>
#include <thread>
#include <atomic>
#include <chrono>

namespace meminfo {
namespace platform {

class MemoryMonitorLinux : public IMemoryMonitor {
public:
    MemoryMonitorLinux() {
        running_ = true;
        monitor_thread_ = std::thread([this]() {
            while (running_) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                if (callback_) {
                    auto stats = get_stats();
                    // Arbitrary threshold for warning: < 500MB available
                    if (stats.available_bytes < 500 * 1024 * 1024) {
                        callback_();
                    }
                }
            }
        });
    }

    ~MemoryMonitorLinux() override {
        running_ = false;
        if (monitor_thread_.joinable()) {
            monitor_thread_.join();
        }
    }

    MemoryStats get_stats() override {
        MemoryStats stats = {0, 0, 0};
        std::ifstream file("/proc/meminfo");
        std::string line;
        while (std::getline(file, line)) {
            std::istringstream iss(line);
            std::string key;
            size_t value;
            std::string unit;
            if (iss >> key >> value >> unit) {
                if (key == "MemTotal:") {
                    stats.total_bytes = value * 1024;
                } else if (key == "MemFree:") {
                    stats.free_bytes = value * 1024;
                } else if (key == "MemAvailable:") {
                    stats.available_bytes = value * 1024;
                }
            }
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
    return std::make_unique<MemoryMonitorLinux>();
}

} // namespace platform
} // namespace meminfo
