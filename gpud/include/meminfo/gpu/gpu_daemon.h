#pragma once
#include <meminfo/common/config.h>
#include <meminfo/gpu/cuda_executor.h>
#include <uv.h>
#include <memory>
#include <string>
#include <atomic>

namespace meminfo {
namespace gpu {

class GpuDaemon {
public:
    explicit GpuDaemon(const Config& config);
    ~GpuDaemon();

    void run();
    void stop();

private:
    static void on_connection(uv_stream_t* server, int status);
    // Closes the listening socket and all accepted sessions. Loop thread only.
    void close_sockets();

    Config config_;
    uv_loop_t loop_;
    uv_tcp_t server_socket_;
    uv_async_t stop_async_;
    
    std::unique_ptr<ICudaExecutor> executor_;
    
    std::atomic<bool> is_running_{true};
    uint64_t next_session_id_ = 1;
};

} // namespace gpu
} // namespace meminfo
