#pragma once
#include <uv.h>
#include <string>
#include <memory>
#include <meminfo/common/config.h>
#include <meminfo/discovery/peer_table.h>
#include <meminfo/discovery/announcer.h>
#include <meminfo/discovery/listener.h>
#include <meminfo/discovery/control_socket.h>
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
    
    bool is_running_ = false;
};

} // namespace discovery
} // namespace meminfo
