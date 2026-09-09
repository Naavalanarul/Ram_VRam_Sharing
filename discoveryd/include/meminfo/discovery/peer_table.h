#pragma once
#include <meminfo/common/types.h>
#include <unordered_map>
#include <vector>
#include <string>
#include <cstdint>
#include <chrono>
#include <mutex>

namespace meminfo {
namespace discovery {

struct PeerInfo {
    node_id_t id{};
    std::string hostname;
    std::string address;
    uint64_t free_ram_bytes = 0;
    uint64_t free_vram_bytes = 0;
    uint16_t memory_port = 0;
    uint16_t gpu_port = 0;
    std::chrono::steady_clock::time_point last_seen;
    enum class State { ACTIVE, STALE, EVICTED } state = State::ACTIVE;
};

class PeerTable {
public:
    PeerTable() = default;
    ~PeerTable() = default;

    void update(const PeerInfo& info);
    void tick();
    std::vector<PeerInfo> get_peers() const;
    PeerInfo get_peer(const node_id_t& id) const;
    void remove_peer(const node_id_t& id);
    size_t active_count() const;

    void set_ttl_seconds(int ttl) { ttl_seconds_ = ttl; }

private:
    mutable std::mutex mutex_;
    std::unordered_map<node_id_t, PeerInfo> peers_;
    int ttl_seconds_ = 10;
};

} // namespace discovery
} // namespace meminfo
