#include <meminfo/platform/ILocalIpc.h>
#include <windows.h>
#include <thread>
#include <atomic>
#include <spdlog/spdlog.h>
#include <vector>
#include <string>
#include <cstdint>

namespace meminfo {
namespace platform {

namespace {
// Mirrors the POSIX backend: bounds a hostile or corrupt length prefix so it
// cannot turn into a multi-gigabyte allocation, and keeps every length inside
// the 32-bit range the pipe API and the wire format both use.
constexpr uint32_t kMaxIpcMessageBytes = 64u * 1024u * 1024u;
} // namespace

class LocalIpcWin : public ILocalIpc {
public:
    ~LocalIpcWin() override {
        running_ = false;
        if (listen_thread_.joinable()) {
            // Need a dummy connection to break out of ConnectNamedPipe
            if (!pipe_name_.empty()) {
                HANDLE hPipe = CreateFileA(pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE,
                    0, NULL, OPEN_EXISTING, 0, NULL);
                if (hPipe != INVALID_HANDLE_VALUE) {
                    CloseHandle(hPipe);
                }
            }
            listen_thread_.join();
        }
        if (server_pipe_ != INVALID_HANDLE_VALUE) {
            CloseHandle(server_pipe_);
        }
    }

    void listen(const std::string& name, MessageHandler cb) override {
        pipe_name_ = "\\\\.\\pipe\\" + name;
        
        server_pipe_ = CreateNamedPipeA(
            pipe_name_.c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            8192, 8192, 0, NULL);
            
        if (server_pipe_ == INVALID_HANDLE_VALUE) {
            spdlog::error("Failed to create named pipe");
            return;
        }
        
        handler_ = std::move(cb);
        running_ = true;
        
        listen_thread_ = std::thread([this]() {
            while (running_) {
                BOOL connected = ConnectNamedPipe(server_pipe_, NULL) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
                if (!running_) break;

                if (connected) {
                    serve_client();
                    FlushFileBuffers(server_pipe_);
                    DisconnectNamedPipe(server_pipe_);
                }
            }
        });
    }

    std::vector<uint8_t> send_request(const std::string& name, const std::vector<uint8_t>& req) override {
        if (req.size() > kMaxIpcMessageBytes) {
            spdlog::error("IPC request too large: {} bytes", req.size());
            return {};
        }

        std::string pipe_name = "\\\\.\\pipe\\" + name;
        HANDLE hPipe = CreateFileA(
            pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE,
            0, NULL, OPEN_EXISTING, 0, NULL);
            
        if (hPipe == INVALID_HANDLE_VALUE) return {};
        
        DWORD mode = PIPE_READMODE_MESSAGE;
        SetNamedPipeHandleState(hPipe, &mode, NULL, NULL);
        
        std::vector<uint8_t> resp;
        const uint32_t len = static_cast<uint32_t>(req.size());
        DWORD written = 0;

        if (WriteFile(hPipe, &len, sizeof(len), &written, NULL) &&
            (len == 0 || WriteFile(hPipe, req.data(), static_cast<DWORD>(req.size()), &written, NULL))) {

            uint32_t resp_len = 0;
            DWORD read_bytes = 0;
            if (ReadFile(hPipe, &resp_len, sizeof(resp_len), &read_bytes, NULL) &&
                read_bytes == sizeof(resp_len)) {
                if (resp_len > kMaxIpcMessageBytes) {
                    spdlog::error("IPC response too large: {} bytes", resp_len);
                } else if (resp_len > 0) {
                    resp.resize(resp_len);
                    if (!ReadFile(hPipe, resp.data(), resp_len, &read_bytes, NULL) ||
                        read_bytes != resp_len) {
                        resp.clear();
                    }
                }
            }
        }

        CloseHandle(hPipe);
        return resp;
    }

private:
    // Reads one length-prefixed request, runs the handler, writes the reply.
    void serve_client() {
        DWORD read_bytes = 0;
        uint32_t len = 0;
        if (!ReadFile(server_pipe_, &len, sizeof(len), &read_bytes, NULL) ||
            read_bytes != sizeof(len)) {
            return;
        }
        if (len > kMaxIpcMessageBytes) {
            spdlog::error("IPC request too large: {} bytes", len);
            return;
        }

        std::vector<uint8_t> req(len);
        if (len > 0 &&
            (!ReadFile(server_pipe_, req.data(), len, &read_bytes, NULL) || read_bytes != len)) {
            return;
        }

        std::vector<uint8_t> resp;
        if (handler_) {
            handler_(req, resp);
        }
        if (resp.size() > kMaxIpcMessageBytes) {
            spdlog::error("IPC response too large: {} bytes", resp.size());
            return;
        }

        const uint32_t resp_len = static_cast<uint32_t>(resp.size());
        DWORD written = 0;
        if (!WriteFile(server_pipe_, &resp_len, sizeof(resp_len), &written, NULL)) {
            return;
        }
        if (resp_len > 0) {
            WriteFile(server_pipe_, resp.data(), static_cast<DWORD>(resp.size()), &written, NULL);
        }
    }

    HANDLE server_pipe_ = INVALID_HANDLE_VALUE;
    std::string pipe_name_;
    std::thread listen_thread_;
    std::atomic<bool> running_{false};
    MessageHandler handler_;
};

std::unique_ptr<ILocalIpc> create_local_ipc() {
    return std::make_unique<LocalIpcWin>();
}

} // namespace platform
} // namespace meminfo
