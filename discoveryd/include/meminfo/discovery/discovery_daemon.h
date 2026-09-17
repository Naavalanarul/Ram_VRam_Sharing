#pragma once
#include <uv.h>
#include <string>
#include <memory>
#include <meminfo/common/config.h>
#include <meminfo/discovery/peer_table.h>
#include <meminfo/discovery/announcer.h>
#include <meminfo/discovery/listener.h>
#include <meminfo/discovery/control_socket.h>
#include <meminfo/discovery/pool_probe.h>
#include <meminfo/common/signal_handler.h>
#include <meminfo/platform/IMemoryMonitor.h>

namespace meminfo {
namespace discovery {

class DiscoveryDaemon {
public:
    DiscoveryDaemon(const Config& config);
    ~DiscoveryDaemon();
    
    void run();
    void stop();
    
    // Pool membership control
    void join_pool();
    void leave_pool();
    bool is_pool_joined() const { return pool_joined_; }
    
    // Get local status for control socket
    const node_id_t& get_local_id() const { return local_id_; }
    const std::string& get_local_hostname() const { return local_hostname_; }
    uint64_t get_free_ram() const { return free_ram_; }
    uint64_t get_free_vram() const { return free_vram_; }
    size_t get_peer_count() const { return peer_table_.active_count(); }
    const std::string& get_listen_address() const { return listen_address_; }
    uint16_t get_memory_port() const { return memory_port_; }
    uint16_t get_gpu_port() const { return gpu_port_; }

private:
    Config config_;
    uv_loop_t loop_;
    uv_async_t stop_async_;
    
    PeerTable peer_table_;
    std::unique_ptr<Announcer> announcer_;
    std::unique_ptr<Listener> listener_;
    std::unique_ptr<ControlSocket> control_socket_;
    std::unique_ptr<SignalHandler> sig_handler_;
    std::unique_ptr<platform::IMemoryMonitor> memory_monitor_;
    std::unique_ptr<PoolProbe> pool_probe_;
    
    bool is_running_ = false;
    bool pool_joined_ = true;  // Default to joined for backward compatibility
    
    // Local node info
    node_id_t local_id_;
    std::string local_hostname_;
    std::string listen_address_;
    uint16_t memory_port_ = 0;
    uint16_t gpu_port_ = 0;
    
    // Current metrics
    uint64_t free_ram_ = 0;
    uint64_t free_vram_ = 0;
    
    void update_announcer_state();

    // Capacity to announce: memoryd's remaining pool when its control socket
    // answers, otherwise the OS free-RAM figure.
    uint64_t current_free_ram() const;
};

} // namespace discovery
} // namespace meminfo
