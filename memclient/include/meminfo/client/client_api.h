#pragma once
#include <meminfo/client/lru_cache.h>
#include <meminfo/common/types.h>
#include <meminfo/platform/IMemoryMonitor.h>
#include <memory_generated.h>
#include <control_generated.h>
#include <string>
#include <vector>
#include <memory>
#include <future>
#include <mutex>
#include <thread>
#include <uv.h>
#include <unordered_map>
#include <queue>

namespace meminfo {
namespace client {

class MemoryClient {
public:
    // If threshold_bytes is 0, it relies purely on OS memory pressure callbacks.
    // If max_local_bytes is 0, it relies on system total RAM info.
    MemoryClient(size_t max_local_bytes, const std::string& discovery_socket = "/var/run/meminfo_discovery.sock", const std::string& test_ip = "", int test_port = 0);
    ~MemoryClient();

    // Allocate memory logically. Returns a handle.
    handle_t allocate(size_t size);

    // Free a handle completely (local and remote)
    void free(handle_t handle);

    // Read data from a handle. Will pull from remote if evicted.
    std::vector<uint8_t> read(handle_t handle, size_t offset, size_t size);

    // Write data to a handle. Will push to remote if evicted or load into local cache.
    void write(handle_t handle, size_t offset, const std::vector<uint8_t>& data);

private:
    struct RemotePeer {
        MemoryClient* client = nullptr;
        std::string ip;
        int port;
        uv_tcp_t* socket = nullptr;
        bool connected = false;
        std::vector<uint8_t> read_buffer;
    };

    struct RequestContext {
        std::promise<std::vector<uint8_t>> promise;
        uint64_t request_id;
    };

    // --- Thread-safe internal methods ---
    void connect_to_peers();
    void add_peer_and_connect(const std::string& ip, int port);
    std::vector<uint8_t> sync_remote_call(RemotePeer* peer, const uint8_t* payload, size_t size, uint64_t request_id);
    void evict_if_needed(size_t size_needed);
    void load_to_local(handle_t handle);
    
    // --- libuv thread methods ---
    void network_thread_main();
    static void on_peer_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);

    struct OutboundMessage {
        RemotePeer* peer;
        std::vector<uint8_t> payload;
    };
    
    std::mutex queue_mutex_;
    std::queue<OutboundMessage> outbound_queue_;
    
    std::string discovery_socket_;
    
    LRUCache cache_;
    std::unique_ptr<platform::IMemoryMonitor> memory_monitor_;
    
    // Remote tracking
    std::mutex remote_mutex_;
    struct RemoteAllocation {
        uint64_t remote_handle;
        size_t size;
    };
    std::unordered_map<handle_t, RemoteAllocation> remote_handles_; // local handle -> remote info
    std::unordered_map<handle_t, RemotePeer*> handle_to_peer_;
    
    std::vector<std::unique_ptr<RemotePeer>> peers_;
    RemotePeer* current_peer_ = nullptr;
    
    // Networking
    std::thread network_thread_;
    uv_loop_t loop_;
    uv_async_t stop_async_;
    uv_async_t wakeup_async_;
    bool running_ = true;
    
    std::mutex requests_mutex_;
    std::unordered_map<uint64_t, std::shared_ptr<RequestContext>> pending_requests_;
    uint64_t next_request_id_ = 1;
    handle_t next_local_handle_ = 1;
};

} // namespace client
} // namespace meminfo
