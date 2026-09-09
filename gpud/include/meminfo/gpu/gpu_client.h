#pragma once
#include <meminfo/common/types.h>
#include <vector>
#include <string>
#include <memory>
#include <future>
#include <mutex>
#include <thread>
#include <uv.h>
#include <unordered_map>
#include <queue>

namespace meminfo {
namespace gpu {

class GpuClient {
public:
    // Initializes the client and connects to a specific gpud
    GpuClient(const std::string& ip, int port);
    ~GpuClient();

    // CUDA-like API
    int cudaMalloc(uint64_t* devPtr, size_t size);
    int cudaFree(uint64_t devPtr);
    int cudaMemcpyHtoD(uint64_t dst, const void* src, size_t size);
    int cudaMemcpyDtoH(void* dst, uint64_t src, size_t size);
    int cudaGetDeviceProperties(std::string& name, size_t& totalGlobalMem);

private:
    struct RemotePeer {
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

    struct OutboundMessage {
        std::vector<uint8_t> payload;
    };

    std::vector<uint8_t> sync_remote_call(const uint8_t* payload, size_t size, uint64_t request_id);
    
    // --- libuv thread methods ---
    void network_thread_main();
    static void on_peer_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);

    std::unique_ptr<RemotePeer> peer_;
    
    std::mutex requests_mutex_;
    std::unordered_map<uint64_t, std::shared_ptr<RequestContext>> pending_requests_;
    uint64_t next_request_id_ = 1;
    
    std::mutex queue_mutex_;
    std::queue<OutboundMessage> outbound_queue_;
    
    std::thread network_thread_;
    uv_loop_t loop_;
    uv_async_t stop_async_;
    uv_async_t wakeup_async_;
    bool running_ = true;
};

} // namespace gpu
} // namespace meminfo
