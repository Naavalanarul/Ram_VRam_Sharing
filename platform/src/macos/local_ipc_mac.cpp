#include <meminfo/platform/ILocalIpc.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <spdlog/spdlog.h>
#include <cstring>
#include <vector>

namespace meminfo {
namespace platform {

class LocalIpcMac : public ILocalIpc {
public:
    ~LocalIpcMac() override {
        running_ = false;
        if (server_fd_ >= 0) {
            close(server_fd_);
        }
        if (listen_thread_.joinable()) {
            listen_thread_.join();
        }
        if (!socket_path_.empty()) {
            unlink(socket_path_.c_str());
        }
    }

    void listen(const std::string& name, MessageHandler cb) override {
        socket_path_ = name;
        unlink(name.c_str());
        
        server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (server_fd_ < 0) {
            spdlog::error("Failed to create UDS socket");
            return;
        }
        
        struct sockaddr_un addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, name.c_str(), sizeof(addr.sun_path) - 1);
        
        if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            spdlog::error("Failed to bind UDS socket to {}", name);
            close(server_fd_);
            return;
        }
        
        if (::listen(server_fd_, 5) < 0) {
            spdlog::error("Failed to listen on UDS socket");
            close(server_fd_);
            return;
        }
        
        handler_ = std::move(cb);
        running_ = true;
        
        listen_thread_ = std::thread([this]() {
            while (running_) {
                int client_fd = accept(server_fd_, nullptr, nullptr);
                if (client_fd < 0) {
                    if (running_) spdlog::error("accept failed");
                    continue;
                }
                
                // Read length prefix
                uint32_t len = 0;
                if (read(client_fd, &len, sizeof(len)) == sizeof(len)) {
                    std::vector<uint8_t> req(len);
                    size_t read_bytes = 0;
                    while (read_bytes < len) {
                        ssize_t n = read(client_fd, req.data() + read_bytes, len - read_bytes);
                        if (n <= 0) break;
                        read_bytes += n;
                    }
                    
                    if (read_bytes == len) {
                        std::vector<uint8_t> resp;
                        handler_(req, resp);
                        
                        uint32_t resp_len = resp.size();
                        write(client_fd, &resp_len, sizeof(resp_len));
                        write(client_fd, resp.data(), resp.size());
                    }
                }
                close(client_fd);
            }
        });
    }

    std::vector<uint8_t> send_request(const std::string& name, const std::vector<uint8_t>& req) override {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return {};
        
        struct sockaddr_un addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, name.c_str(), sizeof(addr.sun_path) - 1);
        
        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(fd);
            return {};
        }
        
        uint32_t len = req.size();
        write(fd, &len, sizeof(len));
        write(fd, req.data(), req.size());
        
        uint32_t resp_len = 0;
        std::vector<uint8_t> resp;
        if (read(fd, &resp_len, sizeof(resp_len)) == sizeof(resp_len)) {
            resp.resize(resp_len);
            size_t read_bytes = 0;
            while (read_bytes < resp_len) {
                ssize_t n = read(fd, resp.data() + read_bytes, resp_len - read_bytes);
                if (n <= 0) break;
                read_bytes += n;
            }
        }
        close(fd);
        return resp;
    }

private:
    int server_fd_ = -1;
    std::string socket_path_;
    std::thread listen_thread_;
    std::atomic<bool> running_{false};
    MessageHandler handler_;
};

std::unique_ptr<ILocalIpc> create_local_ipc() {
    return std::make_unique<LocalIpcMac>();
}

} // namespace platform
} // namespace meminfo
