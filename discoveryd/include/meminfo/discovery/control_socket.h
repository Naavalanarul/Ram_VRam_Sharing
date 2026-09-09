#pragma once
#include <string>
#include <memory>
#include <meminfo/discovery/peer_table.h>
#include <meminfo/platform/ILocalIpc.h>

namespace meminfo {
namespace discovery {

class ControlSocket {
public:
    ControlSocket(PeerTable* peer_table, const std::string& socket_path);
    ~ControlSocket();
    
    void start();
    void stop();

private:
    void handle_request(const std::vector<uint8_t>& req, std::vector<uint8_t>& resp);

    PeerTable* peer_table_;
    std::string socket_path_;
    std::unique_ptr<platform::ILocalIpc> ipc_;
};

} // namespace discovery
} // namespace meminfo
