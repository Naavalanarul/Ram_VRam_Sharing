#pragma once
#include <uv.h>
#include <string>
#include <functional>

namespace meminfo {

// Minimal HTTP server that responds to GET /status with JSON.
// The status_provider callback returns the JSON body string.
class HttpStatusServer {
public:
    using StatusProvider = std::function<std::string()>;
    
    HttpStatusServer(uv_loop_t* loop, const std::string& bind_addr, int port,
                     StatusProvider provider);
    ~HttpStatusServer();
    
    HttpStatusServer(const HttpStatusServer&) = delete;
    HttpStatusServer& operator=(const HttpStatusServer&) = delete;
    
    void stop();
    
private:
    static void on_connection(uv_stream_t* server, int status);
    static void on_alloc(uv_handle_t* handle, size_t suggested, uv_buf_t* buf);
    static void on_read(uv_stream_t* client, ssize_t nread, const uv_buf_t* buf);
    static void on_write_done(uv_write_t* req, int status);
    
    uv_tcp_t server_;
    StatusProvider provider_;
    bool stopped_ = false;
};

} // namespace meminfo
