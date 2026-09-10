#include <meminfo/discovery/announcer.h>
#include <meminfo/common/protocol_version.h>
#include <discovery_generated.h>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <spdlog/spdlog.h>

namespace meminfo {
namespace discovery {

Announcer::Announcer(uv_loop_t* loop, 
                     const node_id_t& local_id,
                     const std::string& local_hostname,
                     const std::string& listen_address,
                     uint16_t memory_port,
                     uint16_t gpu_port,
                     const std::string& mcast_ip, 
                     int mcast_port, 
                     int interval_ms)
    : loop_(loop),
      local_id_(local_id),
      local_hostname_(local_hostname),
      listen_address_(listen_address),
      memory_port_(memory_port),
      gpu_port_(gpu_port),
      mcast_ip_(mcast_ip),
      mcast_port_(mcast_port),
      interval_ms_(interval_ms) {
    
    uv_udp_init(loop_, &udp_handle_);
    uv_timer_init(loop_, &timer_handle_);
    
    udp_handle_.data = this;
    timer_handle_.data = this;
    
    uv_ip4_addr(mcast_ip_.c_str(), mcast_port_, &dest_addr_);
}

Announcer::~Announcer() {
    stop();
}

void Announcer::start() {
    if (is_running_) return;
    
    // We bind to 0.0.0.0:0 to send from any available port.
    // Multicast membership for SENDING is handled by the OS routing,
    // though sometimes it's necessary to set the multicast interface.
    // For simplicity on LAN, we just send to the multicast dest.
    struct sockaddr_in any_addr;
    uv_ip4_addr("0.0.0.0", 0, &any_addr);

    int r = uv_udp_bind(&udp_handle_, reinterpret_cast<const struct sockaddr*>(&any_addr), 0);
    if (r < 0) {
        spdlog::error("Announcer UDP bind failed: {}", uv_strerror(r));
        return;
    }

    uv_udp_set_multicast_loop(&udp_handle_, 1);

    // Send announcements out of the configured interface. Pinning this to
    // loopback would confine discovery to the local machine, which defeats LAN
    // discovery entirely; "0.0.0.0" means "let the routing table decide", which
    // matches how Listener interprets the same setting.
    if (!listen_address_.empty() && listen_address_ != "0.0.0.0") {
        r = uv_udp_set_multicast_interface(&udp_handle_, listen_address_.c_str());
        if (r < 0) {
            spdlog::warn("Could not set multicast interface to {}: {}",
                         listen_address_, uv_strerror(r));
        }
    }

    
    uv_timer_start(&timer_handle_, on_timer, 0, interval_ms_);
    is_running_ = true;
}

void Announcer::stop() {
    if (!is_running_) return;
    
    uv_timer_stop(&timer_handle_);
    
    if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&udp_handle_))) {
        uv_close(reinterpret_cast<uv_handle_t*>(&udp_handle_), nullptr);
    }
    if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&timer_handle_))) {
        uv_close(reinterpret_cast<uv_handle_t*>(&timer_handle_), nullptr);
    }
    
    is_running_ = false;
}

void Announcer::update_metrics(uint64_t free_ram, uint64_t free_vram) {
    free_ram_ = free_ram;
    free_vram_ = free_vram;
}

struct SendReqCtx {
    uv_udp_send_t req;
    char* buf_base;
};

void Announcer::on_timer(uv_timer_t* handle) {
    auto* self = static_cast<Announcer*>(handle->data);
    
    // If not joined to pool, announce zero capacity so peers don't route allocations here
    uint64_t announce_ram = self->pool_joined_ ? self->free_ram_ : 0;
    uint64_t announce_vram = self->pool_joined_ ? self->free_vram_ : 0;
    
    flatbuffers::FlatBufferBuilder builder;
    auto fb_node_id = builder.CreateVector(self->local_id_.data(), self->local_id_.size());
    auto fb_hostname = builder.CreateString(self->local_hostname_);
    auto fb_address = builder.CreateString(self->listen_address_);
    
    uint64_t heartbeat_ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
        
    meminfo::discovery::AnnouncementBuilder ab(builder);
    ab.add_protocol_version(meminfo::MEMINFO_PROTOCOL_VERSION);
    ab.add_node_id(fb_node_id);
    ab.add_hostname(fb_hostname);
    ab.add_free_ram_bytes(announce_ram);
    ab.add_free_vram_bytes(announce_vram);
    ab.add_heartbeat_ts(heartbeat_ts);
    ab.add_memory_port(self->memory_port_);
    ab.add_gpu_port(self->gpu_port_);
    ab.add_listen_address(fb_address);
    builder.Finish(ab.Finish());
    
    size_t size = builder.GetSize();
    char* data = new char[size];
    std::memcpy(data, builder.GetBufferPointer(), size);
    
    SendReqCtx* ctx = new SendReqCtx;
    ctx->buf_base = data;
    
    uv_buf_t buf = uv_buf_init(ctx->buf_base, static_cast<unsigned int>(size));
    ctx->req.data = ctx;
    
    uv_udp_send(&ctx->req, &self->udp_handle_, &buf, 1, 
                reinterpret_cast<const struct sockaddr*>(&self->dest_addr_), on_send);
}

void Announcer::on_send(uv_udp_send_t* req, int /*status*/) {
    SendReqCtx* ctx = static_cast<SendReqCtx*>(req->data);
    delete[] ctx->buf_base;
    delete ctx;
}

} // namespace discovery
} // namespace meminfo
