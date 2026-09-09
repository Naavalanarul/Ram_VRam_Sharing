#pragma once
#include <uv.h>
#include <string>
#include <meminfo/common/types.h>

namespace meminfo {
namespace discovery {

class Announcer {
public:
    // Requires a libuv loop, local node info to broadcast, and multicast config
    Announcer(uv_loop_t* loop, 
              const node_id_t& local_id,
              const std::string& local_hostname,
              const std::string& listen_address,
              uint16_t memory_port,
              uint16_t gpu_port,
              const std::string& mcast_ip, 
              int mcast_port, 
              int interval_ms);
              
    ~Announcer();
    
    void start();
    void stop();

    // Call this to update the dynamic metrics (RAM/VRAM) before the next broadcast
    void update_metrics(uint64_t free_ram, uint64_t free_vram);
    
    // Set pool membership state - when false, announce zero capacity
    void set_pool_joined(bool joined) { pool_joined_ = joined; }

private:
    static void on_timer(uv_timer_t* handle);
    static void on_send(uv_udp_send_t* req, int status);

    uv_loop_t* loop_;
    uv_udp_t udp_handle_;
    uv_timer_t timer_handle_;
    
    node_id_t local_id_;
    std::string local_hostname_;
    std::string listen_address_;
    uint16_t memory_port_;
    uint16_t gpu_port_;
    
    uint64_t free_ram_ = 0;
    uint64_t free_vram_ = 0;
    
    std::string mcast_ip_;
    int mcast_port_;
    int interval_ms_;
    
    bool is_running_ = false;
    bool pool_joined_ = true;  // Default to joined for backward compatibility
    struct sockaddr_in dest_addr_;
};

} // namespace discovery
} // namespace meminfo
