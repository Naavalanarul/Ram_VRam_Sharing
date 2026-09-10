#include <meminfo/platform/IMemoryMonitor.h>
#include <fstream>
#include <string>
#include <sstream>
#include <thread>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace meminfo {
namespace platform {

class MemoryMonitorLinux : public IMemoryMonitor {
public:
    MemoryMonitorLinux() {
        running_ = true;
        monitor_thread_ = std::thread([this]() { poll_loop(); });
    }

    ~MemoryMonitorLinux() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
        }
        cv_.notify_all();
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
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = std::move(cb);
    }

private:
    // Waits on a condition variable rather than sleeping, so the destructor
    // does not have to block for a full poll interval before joining.
    void poll_loop() {
        constexpr auto kInterval = std::chrono::seconds(2);
        // Arbitrary threshold for warning: < 500MB available
        constexpr size_t kPressureThreshold = 500u * 1024u * 1024u;

        std::unique_lock<std::mutex> lock(mutex_);
        while (running_) {
            if (cv_.wait_for(lock, kInterval, [this] { return !running_; })) {
                break; // shutting down
            }

            // Copy the callback under the lock, then invoke it unlocked so a
            // handler that calls back into this object cannot deadlock.
            PressureCallback cb = callback_;
            lock.unlock();

            bool fire = false;
            if (cb) {
                fire = get_stats().available_bytes < kPressureThreshold;
            }
            if (fire) {
                cb();
            }

            lock.lock();
        }
    }

    PressureCallback callback_;
    std::thread monitor_thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool running_ = false;
};

std::unique_ptr<IMemoryMonitor> create_memory_monitor() {
    return std::make_unique<MemoryMonitorLinux>();
}

} // namespace platform
} // namespace meminfo
