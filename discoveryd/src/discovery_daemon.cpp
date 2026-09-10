#include <meminfo/discovery/discovery_daemon.h>
#include <meminfo/common/uuid.h>
#include <spdlog/spdlog.h>
#include <fstream>
#include <sstream>
#include <cstdint>
#include <unistd.h>

#ifdef __linux__
#include <unistd.h>
#endif

namespace meminfo {
namespace discovery {

namespace {
// Ports arrive from the config as int but are announced as uint16_t. Narrow
// explicitly so the conversion is intentional, and fall back to 0 (disabled)
// for a value that could not round-trip, rather than truncating it silently.
uint16_t to_port(int value, const char* what) {
    if (value < 0 || value > 65535) {
        spdlog::warn("Ignoring out-of-range {} value {}; disabling", what, value);
        return 0;
    }
    return static_cast<uint16_t>(value);
}
} // namespace

DiscoveryDaemon::DiscoveryDaemon(const Config& config) 
    : config_(config) {
    
    uv_loop_init(&loop_);
    
    uv_async_init(&loop_, &stop_async_, [](uv_async_t* handle) {
        auto* self = static_cast<DiscoveryDaemon*>(handle->data);
        if (self->is_running_) {
            self->announcer_->stop();
            self->listener_->stop();
            self->control_socket_->stop();
            uv_stop(&self->loop_);
            self->is_running_ = false;
        }
    });
    stop_async_.data = this;
    
    // Generate UUID if not present (simple placeholder)
    local_id_ = generate_uuid();
    
    local_hostname_ = "localhost";
    char host_buf[256];
    if (gethostname(host_buf, sizeof(host_buf)) == 0) {
        local_hostname_ = host_buf;
    }
    
    listen_address_ = config_.get<std::string>("discovery", "listen_address", "0.0.0.0");
    // Ports are announced as uint16_t; narrow explicitly and reject values that
    // could not round-trip rather than silently truncating a bad config.
    memory_port_ = to_port(config_.get<int>("discovery", "memory_port", 9200), "memory_port");
    gpu_port_ = to_port(config_.get<int>("discovery", "gpu_port", 9300), "gpu_port");
    
    std::string mcast_ip = config_.get<std::string>("discovery", "multicast_group", "239.255.73.77");
    int mcast_port = config_.get<int>("discovery", "multicast_port", 9100);
    int interval_ms = config_.get<int>("discovery", "announce_interval_ms", 1000);
    
    peer_table_.set_ttl_seconds(config_.get<int>("discovery", "peer_ttl_seconds", 10));
    
    announcer_ = std::make_unique<Announcer>(&loop_, local_id_, local_hostname_, listen_address_, 
                                             memory_port_, gpu_port_, mcast_ip, mcast_port, interval_ms);
                                             
    listener_ = std::make_unique<Listener>(&loop_, &peer_table_, mcast_ip, mcast_port, listen_address_);
    
    std::string sock_path = config_.get<std::string>("discovery", "control_socket", "/var/run/meminfo_discovery.sock");
    control_socket_ = std::make_unique<ControlSocket>(&peer_table_, sock_path, this);
    
    memory_monitor_ = platform::create_memory_monitor();
    
    sig_handler_ = std::make_unique<SignalHandler>(&loop_, [this]() {
        spdlog::info("Received termination signal");
        this->stop();
    });
}

DiscoveryDaemon::~DiscoveryDaemon() {
    stop();
    if (control_socket_) control_socket_->stop();
    if (listener_) listener_->stop();
    if (announcer_) announcer_->stop();
    
    if (uv_is_active(reinterpret_cast<uv_handle_t*>(&stop_async_)) || !uv_is_closing(reinterpret_cast<uv_handle_t*>(&stop_async_))) {
        uv_close(reinterpret_cast<uv_handle_t*>(&stop_async_), nullptr);
    }
    
    // Drain all closing handles while memory is still valid
    for (int i = 0; i < 10; ++i) {
        uv_run(&loop_, UV_RUN_NOWAIT);
    }
    
    sig_handler_.reset();
    control_socket_.reset();
    listener_.reset();
    announcer_.reset();
    
    uv_loop_close(&loop_);
}

void DiscoveryDaemon::run() {
    if (is_running_) return;
    
    spdlog::info("Starting discoveryd...");
    
    announcer_->start();
    update_announcer_state();
    listener_->start();
    control_socket_->start();
    
    is_running_ = true;
    
    // Setup a periodic timer to clean up peer table and update metrics
    uv_timer_t* tick_timer = new uv_timer_t;
    uv_timer_init(&loop_, tick_timer);
    tick_timer->data = this;
    uv_timer_start(tick_timer, [](uv_timer_t* handle) {
        auto* self = static_cast<DiscoveryDaemon*>(handle->data);
        self->peer_table_.tick();
        
        uint64_t free_ram = 0;
        if (self->memory_monitor_) {
            free_ram = self->memory_monitor_->get_stats().free_bytes;
        }
        
        // Simple mock for VRAM
        uint64_t free_vram = 0; 
        
        self->free_ram_ = free_ram;
        self->free_vram_ = free_vram;
        self->announcer_->update_metrics(free_ram, free_vram);
    }, 1000, 1000);
    
    uv_run(&loop_, UV_RUN_DEFAULT);
    
    // Stop and close tick_timer before run returns
    uv_timer_stop(tick_timer);
    uv_close(reinterpret_cast<uv_handle_t*>(tick_timer), [](uv_handle_t* handle) {
        delete reinterpret_cast<uv_timer_t*>(handle);
    });
    
    // Also stop all other loop members here if they are running
    is_running_ = false;
    spdlog::info("Discoveryd loop exited.");
}

void DiscoveryDaemon::stop() {
    if (!is_running_) return;
    spdlog::info("Stopping discoveryd...");
    uv_async_send(&stop_async_);
}

void DiscoveryDaemon::join_pool() {
    if (!pool_joined_) {
        pool_joined_ = true;
        spdlog::info("Joined pool");
        update_announcer_state();
    }
}

void DiscoveryDaemon::leave_pool() {
    if (pool_joined_) {
        pool_joined_ = false;
        spdlog::info("Left pool");
        update_announcer_state();
    }
}

void DiscoveryDaemon::update_announcer_state() {
    if (announcer_) {
        announcer_->set_pool_joined(pool_joined_);
    }
}

} // namespace discovery
} // namespace meminfo
