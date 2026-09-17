#pragma once
#include <uv.h>
#include <atomic>
#include <string>
#include <memory>
#include <meminfo/common/config.h>
#include <meminfo/memory/slab_allocator.h>
#include <meminfo/memory/page_tracker.h>
#include <meminfo/memory/memory_control.h>
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

    // Name of the local control socket, or empty when disabled.
    const std::string& get_control_socket_name() const { return control_socket_name_; }

private:
    static void on_connection(uv_stream_t* server, int status);
    // Closes the listening socket and all accepted sessions. Loop thread only.
    void close_sockets();

    Config config_;
    uv_loop_t loop_;
    uv_async_t stop_async_;
    
    std::unique_ptr<SlabAllocator> allocator_;
    std::unique_ptr<PageTracker> page_tracker_;
    std::unique_ptr<MemoryControlSocket> control_socket_;
    std::string control_socket_name_;
    
    uv_tcp_t server_socket_;
    uint64_t next_session_id_ = 1;
    std::atomic<bool> is_running_{false}; // read from the caller's thread, set on the loop thread
    int listen_port_ = 0;
    
    std::unique_ptr<SignalHandler> sig_handler_;
};

} // namespace memory
} // namespace meminfo
