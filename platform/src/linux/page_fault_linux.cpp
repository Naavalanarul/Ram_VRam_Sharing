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
#include <cerrno>
#include <vector>
#include <spdlog/spdlog.h>

namespace meminfo {
namespace platform {

class PageFaultBackendLinux : public IPageFaultBackend {
public:
    PageFaultBackendLinux() {
        long ps = sysconf(_SC_PAGESIZE);
        page_size_ = (ps > 0) ? static_cast<size_t>(ps) : 4096u;

        uffd_ = static_cast<int>(syscall(__NR_userfaultfd, O_CLOEXEC | O_NONBLOCK));
        if (uffd_ == -1) {
            // Commonly EPERM: unprivileged userfaultfd is disabled by default
            // on many kernels. Not fatal -- callers check is_supported().
            spdlog::warn("userfaultfd unavailable: {}", std::strerror(errno));
            return;
        }

        struct uffdio_api api;
        std::memset(&api, 0, sizeof(api));
        api.api = UFFD_API;
        api.features = 0;
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

    bool is_supported() const override { return uffd_ != -1; }

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
        if (uffd_ == -1 || addr == nullptr) {
            return;
        }

        // UFFDIO_COPY requires a page-aligned destination and a length that is
        // a whole number of pages. Callers hand us arbitrary addresses and
        // lengths, so stage the payload in a page-sized bounce buffer: align
        // the destination down, place the data at its offset within the page,
        // and zero-fill the remainder. Passing an unaligned dst or a partial
        // length makes the ioctl fail with EINVAL, which leaves the faulting
        // thread blocked forever.
        const size_t page_size = static_cast<size_t>(page_size_);
        const uint64_t raw_dst = reinterpret_cast<uint64_t>(addr);
        const uint64_t aligned_dst = raw_dst & ~static_cast<uint64_t>(page_size - 1);
        const size_t offset_in_page = static_cast<size_t>(raw_dst - aligned_dst);

        // Round up to cover every page the payload touches.
        const size_t span = offset_in_page + len;
        const size_t total = ((span + page_size - 1) / page_size) * page_size;

        std::vector<uint8_t> staged(total, 0);
        if (len > 0 && data != nullptr) {
            std::memcpy(staged.data() + offset_in_page, data, len);
        }

        struct uffdio_copy copy;
        std::memset(&copy, 0, sizeof(copy));
        copy.src = reinterpret_cast<uint64_t>(staged.data());
        copy.dst = aligned_dst;
        copy.len = total;
        copy.mode = 0;
        copy.copy = 0;

        if (ioctl(uffd_, UFFDIO_COPY, &copy) == -1) {
            spdlog::error("ioctl UFFDIO_COPY failed: {}", std::strerror(errno));
            return;
        }
        resolved_ = true;
    }

private:
    // Last-resort resolution so a faulting thread is never left blocked.
    void zero_page(void* addr) {
        const uint64_t aligned = reinterpret_cast<uint64_t>(addr) &
                                 ~static_cast<uint64_t>(page_size_ - 1);

        struct uffdio_zeropage zp;
        std::memset(&zp, 0, sizeof(zp));
        zp.range.start = aligned;
        zp.range.len = page_size_;
        zp.mode = 0;

        if (ioctl(uffd_, UFFDIO_ZEROPAGE, &zp) == -1) {
            spdlog::error("ioctl UFFDIO_ZEROPAGE failed: {}", std::strerror(errno));
        }
    }

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

                        // A fault that is never answered leaves the faulting
                        // thread blocked forever, so track whether the handler
                        // actually resolved it and zero-fill the page if not.
                        resolved_ = false;
                        if (user_cb_) {
                            user_cb_(fault_addr);
                        }
                        if (!resolved_) {
                            zero_page(fault_addr);
                        }
                    }
                }
            }
        }
    }

    int uffd_ = -1;
    size_t page_size_ = 4096;
    std::thread fault_thread_;
    std::atomic<bool> running_{false};
    bool resolved_ = false; // only touched on the fault-handler thread
    FaultHandler user_cb_;
};

std::unique_ptr<IPageFaultBackend> create_page_fault_backend() {
    return std::make_unique<PageFaultBackendLinux>();
}

} // namespace platform
} // namespace meminfo
