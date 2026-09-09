#include <meminfo/platform/IPageFaultBackend.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <thread>
#include <atomic>
#include <cstring>
#include <spdlog/spdlog.h>

namespace meminfo {
namespace platform {

class PageFaultBackendLinux : public IPageFaultBackend {
public:
    PageFaultBackendLinux() {
        uffd_ = syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK);
        if (uffd_ == -1) {
            spdlog::error("userfaultfd syscall failed");
            return;
        }

        struct uffdio_api api = { .api = UFFD_API, .features = 0 };
        if (ioctl(uffd_, UFFDIO_API, &api) == -1) {
            spdlog::error("ioctl UFFDIO_API failed");
            return;
        }
        
        running_ = true;
        fault_thread_ = std::thread(&PageFaultBackendLinux::fault_handler_thread, this);
    }

    ~PageFaultBackendLinux() override {
        running_ = false;
        if (fault_thread_.joinable()) {
            fault_thread_.join();
        }
        if (uffd_ != -1) {
            close(uffd_);
        }
    }

    PageRegion reserve_region(size_t bytes) override {
        void* addr = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (addr == MAP_FAILED) {
            return {nullptr, 0};
        }

        struct uffdio_register reg;
        reg.range.start = reinterpret_cast<uint64_t>(addr);
        reg.range.len = bytes;
        reg.mode = UFFDIO_REGISTER_MODE_MISSING;

        if (ioctl(uffd_, UFFDIO_REGISTER, &reg) == -1) {
            spdlog::error("ioctl UFFDIO_REGISTER failed");
            munmap(addr, bytes);
            return {nullptr, 0};
        }

        return {addr, bytes};
    }

    void on_fault(FaultHandler cb) override {
        user_cb_ = std::move(cb);
    }

    void resolve_fault(void* addr, const void* data, size_t len) override {
        struct uffdio_copy copy;
        copy.src = reinterpret_cast<uint64_t>(data);
        copy.dst = reinterpret_cast<uint64_t>(addr);
        copy.len = len;
        copy.mode = 0;
        copy.copy = 0;

        if (ioctl(uffd_, UFFDIO_COPY, &copy) == -1) {
            spdlog::error("ioctl UFFDIO_COPY failed");
        }
    }

private:
    void fault_handler_thread() {
        struct pollfd evt;
        evt.fd = uffd_;
        evt.events = POLLIN;

        while (running_) {
            int res = poll(&evt, 1, 100);
            if (res > 0 && (evt.revents & POLLIN)) {
                struct uffd_msg msg;
                if (read(uffd_, &msg, sizeof(msg)) == sizeof(msg)) {
                    if (msg.event == UFFD_EVENT_PAGEFAULT) {
                        void* fault_addr = reinterpret_cast<void*>(msg.arg.pagefault.address);
                        if (user_cb_) {
                            user_cb_(fault_addr);
                        }
                    }
                }
            }
        }
    }

    int uffd_ = -1;
    std::thread fault_thread_;
    std::atomic<bool> running_{false};
    FaultHandler user_cb_;
};

std::unique_ptr<IPageFaultBackend> create_page_fault_backend() {
    return std::make_unique<PageFaultBackendLinux>();
}

} // namespace platform
} // namespace meminfo
