#pragma once
#include <uv.h>
#include <string>
#include <memory>
#include <meminfo/common/config.h>
#include <meminfo/memory/slab_allocator.h>
#include <meminfo/memory/page_tracker.h>
#include <meminfo/common/signal_handler.h>

namespace meminfo {
namespace memory {

class MemoryDaemon {
public:
    MemoryDaemon(const Config& config);
    ~MemoryDaemon();
    
    void run();
    void stop();
    int get_listen_port() const { return listen_port_; }

private:
    static void on_connection(uv_stream_t* server, int status);

    Config config_;
    uv_loop_t loop_;
    uv_async_t stop_async_;
    
    std::unique_ptr<SlabAllocator> allocator_;
    std::unique_ptr<PageTracker> page_tracker_;
    
    uv_tcp_t server_socket_;
    uint64_t next_session_id_ = 1;
    bool is_running_ = false;
    int listen_port_ = 0;
    
    std::unique_ptr<SignalHandler> sig_handler_;
};

} // namespace memory
} // namespace meminfo
