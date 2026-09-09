#include <meminfo/discovery/peer_table.h>
#include <stdexcept>

namespace meminfo {
namespace discovery {

void PeerTable::update(const PeerInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& peer = peers_[info.id];
    // Preserve existing fields if this is an update, then overwrite with new info
    peer = info;
    peer.last_seen = std::chrono::steady_clock::now();
    peer.state = PeerInfo::State::ACTIVE;
}

void PeerTable::tick() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    auto it = peers_.begin();
    while (it != peers_.end()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.last_seen).count();
        if (elapsed > ttl_seconds_ * 3) {
            // Evict completely
            it = peers_.erase(it);
        } else if (elapsed > ttl_seconds_) {
            it->second.state = PeerInfo::State::STALE;
            ++it;
        } else {
            it->second.state = PeerInfo::State::ACTIVE;
            ++it;
        }
    }
}

std::vector<PeerInfo> PeerTable::get_peers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PeerInfo> result;
    result.reserve(peers_.size());
    for (const auto& [id, peer] : peers_) {
        result.push_back(peer);
    }
    return result;
}

PeerInfo PeerTable::get_peer(const node_id_t& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = peers_.find(id);
    if (it == peers_.end()) {
        throw std::out_of_range("Peer not found");
    }
    return it->second;
}

void PeerTable::remove_peer(const node_id_t& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    peers_.erase(id);
}

size_t PeerTable::active_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (const auto& [id, peer] : peers_) {
        if (peer.state == PeerInfo::State::ACTIVE) {
            count++;
        }
    }
    return count;
}

} // namespace discovery
} // namespace meminfo
