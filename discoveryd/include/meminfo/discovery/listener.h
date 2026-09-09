#pragma once
#include <uv.h>
#include <string>
#include <meminfo/discovery/peer_table.h>

namespace meminfo {
namespace discovery {

class Listener {
public:
    Listener(uv_loop_t* loop, 
             PeerTable* peer_table,
             const std::string& mcast_ip, 
             int mcast_port,
             const std::string& bind_iface = "0.0.0.0");
             
    ~Listener();
    
    void start();
    void stop();

private:
    static void on_alloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf);
    static void on_recv(uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf, 
                        const struct sockaddr* addr, unsigned flags);

    uv_loop_t* loop_;
    PeerTable* peer_table_;
    std::string mcast_ip_;
    int mcast_port_;
    std::string bind_iface_;
    
    uv_udp_t udp_handle_;
    bool is_running_ = false;
};

} // namespace discovery
} // namespace meminfo
