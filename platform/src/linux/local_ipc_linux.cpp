#include <meminfo/platform/ILocalIpc.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <spdlog/spdlog.h>
#include <cerrno>
#include <cstring>
#include <vector>

namespace meminfo {
namespace platform {

namespace {
// Upper bound on a single IPC message. Keeps a malformed or hostile length
// prefix from turning into a multi-gigabyte allocation.
constexpr uint32_t kMaxIpcMessageBytes = 64u * 1024u * 1024u;

// read()/write() that resume on EINTR and short transfers.
bool read_exact(int fd, void* dst, size_t len) {
    auto* p = static_cast<uint8_t*>(dst);
    size_t done = 0;
    while (done < len) {
        ssize_t n = ::read(fd, p + done, len - done);
        if (n > 0) {
            done += static_cast<size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

bool write_exact(int fd, const void* src, size_t len) {
    const auto* p = static_cast<const uint8_t*>(src);
    size_t done = 0;
    while (done < len) {
        ssize_t n = ::write(fd, p + done, len - done);
        if (n > 0) {
            done += static_cast<size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

// Copy a socket path into sun_path, rejecting anything that would be silently
// truncated (a truncated path binds/connects to the wrong socket).
bool set_sun_path(struct sockaddr_un& addr, const std::string& name) {
    if (name.empty() || name.size() >= sizeof(addr.sun_path)) {
        return false;
    }
    std::memcpy(addr.sun_path, name.c_str(), name.size() + 1);
    return true;
}
} // namespace

class LocalIpcPosix : public ILocalIpc {
public:
    ~LocalIpcPosix() override {
        stop();
    }

    void listen(const std::string& name, MessageHandler cb) override {
        stop();

        struct sockaddr_un addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        if (!set_sun_path(addr, name)) {
            spdlog::error("UDS path too long or empty: {}", name);
            return;
        }

        unlink(name.c_str());

        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            spdlog::error("Failed to create UDS socket: {}", std::strerror(errno));
            return;
        }

        if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            spdlog::error("Failed to bind UDS socket to {}: {}", name, std::strerror(errno));
            close(fd);
            return;
        }

        if (::listen(fd, 16) < 0) {
            spdlog::error("Failed to listen on UDS socket {}: {}", name, std::strerror(errno));
            close(fd);
            unlink(name.c_str());
            return;
        }

        server_fd_ = fd;
        socket_path_ = name;
        handler_ = std::move(cb);
        running_ = true;

        listen_thread_ = std::thread([this]() { accept_loop(); });
    }

    std::vector<uint8_t> send_request(const std::string& name, const std::vector<uint8_t>& req) override {
        if (req.size() > kMaxIpcMessageBytes) {
            spdlog::error("IPC request too large: {} bytes", req.size());
            return {};
        }

        struct sockaddr_un addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        if (!set_sun_path(addr, name)) {
            spdlog::error("UDS path too long or empty: {}", name);
            return {};
        }

        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return {};

        if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(fd);
            return {};
        }

        std::vector<uint8_t> resp;
        uint32_t len = static_cast<uint32_t>(req.size());
        if (write_exact(fd, &len, sizeof(len)) && write_exact(fd, req.data(), req.size())) {
            uint32_t resp_len = 0;
            if (read_exact(fd, &resp_len, sizeof(resp_len))) {
                if (resp_len > kMaxIpcMessageBytes) {
                    spdlog::error("IPC response too large: {} bytes", resp_len);
                } else if (resp_len > 0) {
                    resp.resize(resp_len);
                    if (!read_exact(fd, resp.data(), resp_len)) {
                        resp.clear();
                    }
                }
            }
        }

        close(fd);
        return resp;
    }

private:
    // Stops the accept loop and joins the thread. Safe to call more than once.
    //
    // The listening socket is *not* closed before the join: closing a
    // descriptor that another thread is blocked on in accept() does not
    // reliably wake that thread, which would deadlock the join. Instead the
    // loop polls with a timeout and observes running_.
    void stop() {
        running_ = false;
        if (listen_thread_.joinable()) {
            listen_thread_.join();
        }
        if (server_fd_ >= 0) {
            close(server_fd_);
            server_fd_ = -1;
        }
        if (!socket_path_.empty()) {
            unlink(socket_path_.c_str());
            socket_path_.clear();
        }
        handler_ = nullptr;
    }

    void accept_loop() {
        while (running_) {
            struct pollfd pfd;
            pfd.fd = server_fd_;
            pfd.events = POLLIN;
            pfd.revents = 0;

            int pr = poll(&pfd, 1, 100);
            if (pr < 0) {
                if (errno == EINTR) continue;
                spdlog::error("UDS poll failed: {}", std::strerror(errno));
                break;
            }
            if (pr == 0 || !(pfd.revents & POLLIN)) {
                continue; // timeout: re-check running_
            }

            int client_fd = accept(server_fd_, nullptr, nullptr);
            if (client_fd < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ||
                    errno == ECONNABORTED) {
                    continue;
                }
                if (running_) {
                    spdlog::error("UDS accept failed: {}", std::strerror(errno));
                }
                break; // unrecoverable; do not spin
            }

            serve_client(client_fd);
            close(client_fd);
        }
    }

    void serve_client(int client_fd) {
        uint32_t len = 0;
        if (!read_exact(client_fd, &len, sizeof(len))) return;
        if (len > kMaxIpcMessageBytes) {
            spdlog::error("IPC request too large: {} bytes", len);
            return;
        }

        std::vector<uint8_t> req(len);
        if (len > 0 && !read_exact(client_fd, req.data(), len)) return;

        std::vector<uint8_t> resp;
        if (handler_) {
            handler_(req, resp);
        }

        uint32_t resp_len = static_cast<uint32_t>(resp.size());
        if (!write_exact(client_fd, &resp_len, sizeof(resp_len))) return;
        if (resp_len > 0) {
            write_exact(client_fd, resp.data(), resp.size());
        }
    }

    int server_fd_ = -1;
    std::string socket_path_;
    std::thread listen_thread_;
    std::atomic<bool> running_{false};
    MessageHandler handler_;
};

std::unique_ptr<ILocalIpc> create_local_ipc() {
    return std::make_unique<LocalIpcPosix>();
}

} // namespace platform
} // namespace meminfo
