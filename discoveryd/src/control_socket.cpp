#include <meminfo/discovery/control_socket.h>
#include <meminfo/discovery/discovery_daemon.h>
#include <control_generated.h>
#include <stdexcept>
#include <iostream>
#include <spdlog/spdlog.h>
#include <chrono>

namespace meminfo {
namespace discovery {

ControlSocket::ControlSocket(PeerTable* peer_table, const std::string& socket_path, DiscoveryDaemon* daemon)
    : peer_table_(peer_table),
      daemon_(daemon),
      socket_path_(socket_path) {
    ipc_ = platform::create_local_ipc();
}

ControlSocket::~ControlSocket() {
    stop();
}

void ControlSocket::start() {
    ipc_->listen(socket_path_, [this](const std::vector<uint8_t>& req, std::vector<uint8_t>& resp) {
        handle_request(req, resp);
    });
}

void ControlSocket::stop() {
    // ILocalIpc handles cleanup in its destructor or internal running flag.
}

void ControlSocket::handle_request(const std::vector<uint8_t>& data, std::vector<uint8_t>& resp) {
    if (data.size() < 4) return;
    
    flatbuffers::Verifier verifier(data.data(), data.size());
    if (!meminfo::control::VerifySizePrefixedControlRequestBuffer(verifier)) {
        return; // Invalid format
    }
    
    const auto* req = meminfo::control::GetSizePrefixedControlRequest(data.data());
    
    flatbuffers::FlatBufferBuilder builder;
    
    bool success = false;
    std::string message = "Unknown command";
    std::vector<flatbuffers::Offset<meminfo::control::PeerInfo>> peers_vec;
    
    // Only create daemon-dependent fields if daemon is available
    flatbuffers::Offset<flatbuffers::Vector<uint8_t>> fb_local_id;
    flatbuffers::Offset<flatbuffers::String> fb_hostname;
    if (daemon_) {
        fb_local_id = builder.CreateVector(daemon_->get_local_id().data(), daemon_->get_local_id().size());
        fb_hostname = builder.CreateString(daemon_->get_local_hostname());
    }
    
    switch (req->command()) {
        case meminfo::control::ControlCommand_LIST_PEERS: {
            auto peers = peer_table_->get_peers();
            for (const auto& p : peers) {
                auto fb_id = builder.CreateVector(p.id.data(), p.id.size());
                // Distinct from the daemon-level fb_hostname above; shadowing it
                // is both confusing and an error under MSVC /W4 /WX (C4456).
                auto fb_peer_hostname = builder.CreateString(p.hostname);
                auto fb_address = builder.CreateString(p.address);

                meminfo::control::PeerInfoBuilder pib(builder);
                pib.add_node_id(fb_id);
                pib.add_hostname(fb_peer_hostname);
                pib.add_address(fb_address);
                pib.add_free_ram_bytes(p.free_ram_bytes);
                pib.add_free_vram_bytes(p.free_vram_bytes);
                pib.add_memory_port(p.memory_port);
                pib.add_gpu_port(p.gpu_port);
                pib.add_state(p.state == PeerInfo::State::ACTIVE ? meminfo::control::PeerState_ACTIVE : meminfo::control::PeerState_STALE);
                
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - p.last_seen).count();
                pib.add_last_seen_ms(elapsed);
                
                peers_vec.push_back(pib.Finish());
            }
            success = true;
            message = "OK";
            break;
        }
        case meminfo::control::ControlCommand_JOIN_POOL: {
            if (daemon_) {
                daemon_->join_pool();
                success = true;
                message = "Joined pool";
            } else {
                success = false;
                message = "Daemon not available";
            }
            break;
        }
        case meminfo::control::ControlCommand_LEAVE_POOL: {
            if (daemon_) {
                daemon_->leave_pool();
                success = true;
                message = "Left pool";
            } else {
                success = false;
                message = "Daemon not available";
            }
            break;
        }
        case meminfo::control::ControlCommand_GET_LOCAL_STATUS: {
            if (daemon_) {
                success = true;
                message = "OK";
            } else {
                success = false;
                message = "Daemon not available";
            }
            break;
        }
        default:
            success = false;
            message = "Unsupported command";
            break;
    }
    
    auto fb_peers = builder.CreateVector(peers_vec);
    auto fb_msg = builder.CreateString(message);
    
    meminfo::control::ControlResponseBuilder crb(builder);
    crb.add_request_id(req->request_id());
    crb.add_success(success);
    crb.add_message(fb_msg);
    
    if (req->command() == meminfo::control::ControlCommand_LIST_PEERS) {
        crb.add_peers(fb_peers);
    }
    
    if (req->command() == meminfo::control::ControlCommand_GET_LOCAL_STATUS && daemon_) {
        crb.add_local_node_id(fb_local_id);
        crb.add_local_hostname(fb_hostname);
        crb.add_local_free_ram(daemon_->get_free_ram());
        crb.add_local_free_vram(daemon_->get_free_vram());
        crb.add_pool_joined(daemon_->is_pool_joined());
        crb.add_peer_count(static_cast<uint32_t>(daemon_->get_peer_count()));
    }
    
    builder.FinishSizePrefixed(crb.Finish());
    
    size_t out_size = builder.GetSize();
    resp.assign(builder.GetBufferPointer(), builder.GetBufferPointer() + out_size);
}

} // namespace discovery
} // namespace meminfo
