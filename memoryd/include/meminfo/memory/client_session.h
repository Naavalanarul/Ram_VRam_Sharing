#pragma once
#include <uv.h>
#include <meminfo/memory/page_tracker.h>
#include <vector>

namespace meminfo {
namespace memory {

class ClientSession {
public:
    ClientSession(uv_loop_t* loop, uv_stream_t* server, PageTracker* page_tracker, uint64_t session_id);
    ~ClientSession();

    // uv_close callback that destroys the session owning the handle. Public so
    // the daemon can tear down accepted connections during shutdown.
    static void on_close_handle(uv_handle_t* handle);

private:
    static void on_alloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf);
    static void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);
    static void on_write_done(uv_write_t* req, int status);

    void process_buffer();
    void handle_request(const uint8_t* data, size_t size);
    void send_response(const uint8_t* data, size_t size);

    uv_tcp_t socket_;
    PageTracker* page_tracker_;
    uint64_t session_id_;
    std::vector<uint8_t> read_buffer_;
};

} // namespace memory
} // namespace meminfo
