#pragma once
#include <meminfo/client/lru_cache.h>
#include <meminfo/common/types.h>
#include <meminfo/platform/IMemoryMonitor.h>
#include <memory_generated.h>
#include <control_generated.h>
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <future>
#include <mutex>
#include <thread>
#include <uv.h>
#include <unordered_map>
#include <queue>
#include <chrono>

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
        
        // Peer capacity info (cached from discovery). capacity_known stays
        // false until discovery actually reports figures for this peer; a peer
        // reached directly (or before the first successful refresh) must not be
        // mistaken for one that reported zero free RAM.
        uint64_t free_ram_bytes = 0;
        uint64_t free_vram_bytes = 0;
        bool capacity_known = false;
        std::chrono::steady_clock::time_point last_capacity_update;

        // True when this peer could hold an allocation of size bytes, treating
        // unknown capacity as "worth trying" — the daemon answers OUT_OF_MEMORY
        // if it cannot actually take it.
        bool may_fit(size_t size) const {
            return !capacity_known || free_ram_bytes >= size;
        }
    };

    struct RemoteAllocation {
        // For single-peer allocations
        uint64_t remote_handle = 0;
        size_t size = 0;
        RemotePeer* peer = nullptr;
        
        // For striped allocations (multiple peers)
        struct Stripe {
            uint64_t remote_handle;
            size_t offset;
            size_t length;
            RemotePeer* peer;
        };
        std::vector<Stripe> stripes;
        bool is_striped = false;
    };

    struct RequestContext {
        std::promise<std::vector<uint8_t>> promise;
        uint64_t request_id;
    };

    // --- Thread-safe internal methods ---
    void connect_to_peers();
    void add_peer_and_connect(const std::string& ip, int port);
    void refresh_peer_capacity();  // Query discovery for updated peer capacities
    RemotePeer* select_best_peer(size_t size_needed);  // Pick peer with most free RAM that fits
    std::vector<uint8_t> sync_remote_call(RemotePeer* peer, const uint8_t* payload, size_t size, uint64_t request_id);
    void evict_if_needed(size_t size_needed);
    void load_to_local(handle_t handle);
    void allocate_remote(handle_t handle, size_t size);  // Allocate on best peer(s) with fallback
    void free_remote(const RemoteAllocation& alloc);   // Free remote allocation (single or striped)
    void write_remote(const RemoteAllocation& alloc, size_t offset, const uint8_t* data, size_t size);
    // release_after_read frees the remote allocation once it has been read,
    // which is what load_to_local() wants (it is moving the block back).
    // Direct range reads of a block that stays remote must pass false.
    std::vector<uint8_t> read_remote(const RemoteAllocation& alloc, size_t offset, size_t size,
                                     bool release_after_read = true);

    // Fills out with the remote allocation for handle when that block is too
    // large ever to sit in the local cache, so callers operate on it in place.
    bool oversized_remote_alloc(handle_t handle, RemoteAllocation& out);
    
    // --- libuv thread methods ---
    void network_thread_main();
    static void on_peer_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);
    // Resolves every in-flight request with an empty buffer after a peer drops.
    static void fail_pending_requests(MemoryClient* client);

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
    std::unordered_map<handle_t, RemoteAllocation> remote_handles_; // local handle -> remote info
    
    std::vector<std::unique_ptr<RemotePeer>> peers_;
    
    // Networking
    std::thread network_thread_;
    uv_loop_t loop_;
    uv_async_t stop_async_;
    uv_async_t wakeup_async_;
    bool running_ = true;
    
    std::mutex requests_mutex_;
    std::unordered_map<uint64_t, std::shared_ptr<RequestContext>> pending_requests_;
    // Handed out from application threads; a plain ++ would let two callers
    // share a request id and receive each other's responses.
    std::atomic<uint64_t> next_request_id_{1};
    std::atomic<handle_t> next_local_handle_{1};
    
    // Peer capacity cache TTL
    static constexpr auto PEER_CAPACITY_TTL = std::chrono::seconds(5);
};

} // namespace client
} // namespace meminfo
