#pragma once
#include <uv.h>
#include <vector>
#include <memory>
#include <cstdint>

namespace meminfo {
namespace gpu {

class ICudaExecutor;

// Handles a single client connection to gpud
class GpuSession {
public:
    GpuSession(uv_loop_t* loop, uv_stream_t* server, ICudaExecutor* executor, uint64_t session_id);
    ~GpuSession();

private:
    static void on_alloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf);
    static void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);
    static void on_close(uv_handle_t* handle);
    static void on_write_done(uv_write_t* req, int status);

    void process_buffer();
    void handle_request(const uint8_t* data, size_t size);
    void send_response(uint64_t request_id, int error_code, uint64_t device_ptr = 0, const uint8_t* data = nullptr, size_t data_size = 0, const std::string& message = "");

    uv_tcp_t socket_;
    ICudaExecutor* executor_;
    uint64_t session_id_;
    std::vector<uint8_t> read_buffer_;

    struct WriteReqCtx {
        uv_write_t req;
        char* buf_base;
    };
};

} // namespace gpu
} // namespace meminfo
