#include <meminfo/gpu/gpu_daemon.h>
#include <meminfo/gpu/gpu_session.h>
#include <spdlog/spdlog.h>

namespace meminfo {
namespace gpu {

GpuDaemon::GpuDaemon(const Config& config) 
    : config_(config) {
    
    // Attempt to load real CUDA, fallback to stub
    executor_ = create_real_cuda_executor();
    if (!executor_) {
        spdlog::warn("Real CUDA runtime not available or not compiled. Falling back to Stub executor.");
        executor_ = create_stub_cuda_executor();
    }
    
    uv_loop_init(&loop_);
    
    uv_async_init(&loop_, &stop_async_, [](uv_async_t* handle) {
        auto* self = static_cast<GpuDaemon*>(handle->data);
        self->close_sockets();
        uv_stop(&self->loop_);
        self->is_running_ = false;
    });
    stop_async_.data = this;
    
    std::string ip = config_.get_string("gpu", "listen_address", "0.0.0.0");
    int port = config_.get<int>("gpu", "port", 9300);
    
    uv_tcp_init(&loop_, &server_socket_);
    server_socket_.data = this;
    
    struct sockaddr_in addr;
    uv_ip4_addr(ip.c_str(), port, &addr);
    
    uv_tcp_bind(&server_socket_, reinterpret_cast<const struct sockaddr*>(&addr), 0);
    int r = uv_listen(reinterpret_cast<uv_stream_t*>(&server_socket_), 128, on_connection);
    
    if (r == 0) {
        spdlog::info("gpud listening on {}:{}", ip, port);
    } else {
        spdlog::error("gpud failed to listen: {}", uv_strerror(r));
    }
}

// Closes the listening socket and every accepted session. Loop thread only.
//
// Sessions own themselves and are freed by their own close callback; the
// listening socket's data points at this daemon, so it must not take that path.
void GpuDaemon::close_sockets() {
    uv_walk(&loop_, [](uv_handle_t* handle, void* arg) {
        auto* self = static_cast<GpuDaemon*>(arg);
        if (uv_is_closing(handle) || handle->type != UV_TCP) return;

        if (handle == reinterpret_cast<uv_handle_t*>(&self->server_socket_)) {
            uv_close(handle, nullptr);
        } else {
            uv_close(handle, GpuSession::on_close_handle);
        }
    }, this);
}

GpuDaemon::~GpuDaemon() {
    // Close directly rather than signalling stop_async_: by this point the loop
    // is not running, so the async callback would never fire and the still-open
    // listening socket would block the drain below forever.
    close_sockets();

    if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&stop_async_))) {
        uv_close(reinterpret_cast<uv_handle_t*>(&stop_async_), nullptr);
    }

    is_running_ = false;

    uv_run(&loop_, UV_RUN_DEFAULT); // Drain remaining closing handles
    uv_loop_close(&loop_);
}

void GpuDaemon::run() {
    uv_run(&loop_, UV_RUN_DEFAULT);
}

void GpuDaemon::stop() {
    if (!is_running_) return;
    spdlog::info("Stopping gpud...");
    uv_async_send(&stop_async_);
}

void GpuDaemon::on_connection(uv_stream_t* server, int status) {
    if (status < 0) {
        spdlog::error("GpuDaemon connection error: {}", uv_strerror(status));
        return;
    }
    
    auto* self = static_cast<GpuDaemon*>(server->data);
    new GpuSession(&self->loop_, server, self->executor_.get(), self->next_session_id_++);
}

} // namespace gpu
} // namespace meminfo
