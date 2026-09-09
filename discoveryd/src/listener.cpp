#include <meminfo/discovery/listener.h>
#include <meminfo/common/protocol_version.h>
#include <discovery_generated.h>
#include <stdexcept>
#include <iostream>

namespace meminfo {
namespace discovery {

Listener::Listener(uv_loop_t* loop, 
                   PeerTable* peer_table,
                   const std::string& mcast_ip, 
                   int mcast_port,
                   const std::string& bind_iface)
    : loop_(loop),
      peer_table_(peer_table),
      mcast_ip_(mcast_ip),
      mcast_port_(mcast_port),
      bind_iface_(bind_iface) {
      
    uv_udp_init(loop_, &udp_handle_);
    udp_handle_.data = this;
}

Listener::~Listener() {
    stop();
}

void Listener::start() {
    if (is_running_) return;

    struct sockaddr_in bind_addr;
    // Bind to 0.0.0.0 on the multicast port
    uv_ip4_addr("0.0.0.0", mcast_port_, &bind_addr);
    
    int r = uv_udp_bind(&udp_handle_, reinterpret_cast<const struct sockaddr*>(&bind_addr), UV_UDP_REUSEADDR);
    if (r < 0) {
        throw std::runtime_error(std::string("UDP bind error: ") + uv_strerror(r));
    }
    
    // Join the multicast group
    const char* iface = bind_iface_ == "0.0.0.0" ? nullptr : bind_iface_.c_str();
    r = uv_udp_set_membership(&udp_handle_, mcast_ip_.c_str(), iface, UV_JOIN_GROUP);
    if (r < 0) {
        throw std::runtime_error(std::string("Multicast join error: ") + uv_strerror(r));
    }
    
    r = uv_udp_recv_start(&udp_handle_, on_alloc, on_recv);
    if (r < 0) {
        throw std::runtime_error(std::string("UDP recv start error: ") + uv_strerror(r));
    }
    
    is_running_ = true;
}

void Listener::stop() {
    if (!is_running_) return;
    
    if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&udp_handle_))) {
        // Optional: Leave multicast group
        const char* iface = bind_iface_ == "0.0.0.0" ? nullptr : bind_iface_.c_str();
        uv_udp_set_membership(&udp_handle_, mcast_ip_.c_str(), iface, UV_LEAVE_GROUP);
        uv_close(reinterpret_cast<uv_handle_t*>(&udp_handle_), nullptr);
    }
    
    is_running_ = false;
}

void Listener::on_alloc(uv_handle_t* /*handle*/, size_t suggested_size, uv_buf_t* buf) {
    buf->base = new char[suggested_size];
    buf->len = suggested_size;
}

void Listener::on_recv(uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf, 
                       const struct sockaddr* /*addr*/, unsigned /*flags*/) {
    if (nread > 0) {
        auto* self = static_cast<Listener*>(handle->data);
        
        flatbuffers::Verifier verifier(reinterpret_cast<const uint8_t*>(buf->base), nread);
        if (meminfo::discovery::VerifyAnnouncementBuffer(verifier)) {
            const auto* ann = meminfo::discovery::GetAnnouncement(buf->base);
            
            if (check_protocol_version(ann->protocol_version())) {
                PeerInfo info;
                
                auto fb_node_id = ann->node_id();
                if (fb_node_id && fb_node_id->size() == info.id.size()) {
                    std::copy(fb_node_id->begin(), fb_node_id->end(), info.id.begin());
                }
                
                if (ann->hostname()) info.hostname = ann->hostname()->str();
                if (ann->listen_address()) info.address = ann->listen_address()->str();
                
                info.free_ram_bytes = ann->free_ram_bytes();
                info.free_vram_bytes = ann->free_vram_bytes();
                info.memory_port = ann->memory_port();
                info.gpu_port = ann->gpu_port();
                
                // We use our local clock for last_seen, not the heartbeat_ts from remote,
                // because clocks might not be perfectly synchronized.
                self->peer_table_->update(info);
            }
        }
    }
    
    if (buf->base) {
        delete[] buf->base;
    }
}

} // namespace discovery
} // namespace meminfo
