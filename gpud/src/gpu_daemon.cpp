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
        if (self->is_running_) {
            uv_walk(&self->loop_, [](uv_handle_t* walked, void* /*arg*/) {
                if (!uv_is_closing(walked) && walked->type == UV_TCP) {
                    uv_close(walked, [](uv_handle_t* /*h*/) { });
                }
            }, nullptr);
            uv_stop(&self->loop_);
            self->is_running_ = false;
        }
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

GpuDaemon::~GpuDaemon() {
    stop();
    uv_close(reinterpret_cast<uv_handle_t*>(&stop_async_), nullptr);
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
