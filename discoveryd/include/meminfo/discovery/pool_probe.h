#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace meminfo {
namespace discovery {

// Polls memoryd's local control socket for the real occupancy of its slab pool.
//
// discoveryd used to announce platform::IMemoryMonitor's free_bytes -- how much
// RAM the operating system had spare. That is not the number a client needs.
// What a client can actually place on this node is memoryd's
// total_reserved_bytes minus what is already handed out, and the two figures
// diverge completely once the pool fills: a node with a full 256MB pool on a
// 32GB machine kept advertising ~20GB of capacity, so clients picked it and
// were answered OUT_OF_MEMORY.
//
// The query runs on its own thread rather than in the daemon's libuv tick.
// ILocalIpc::send_request() is blocking and has no timeout, so a memoryd that
// accepted the connection and then wedged would otherwise stall discoveryd's
// event loop -- announcements, peer expiry and the control socket with it.
class PoolProbe {
public:
    // An empty socket_name disables the probe; available() then stays false and
    // the caller keeps using its previous source of capacity.
    PoolProbe(std::string socket_name, uint32_t poll_interval_ms);
    ~PoolProbe();

    PoolProbe(const PoolProbe&) = delete;
    PoolProbe& operator=(const PoolProbe&) = delete;

    void start();
    void stop();

    // True once memoryd has answered at least one query and has not since
    // stopped answering.
    bool available() const { return available_.load(std::memory_order_acquire); }

    // Bytes still allocatable from memoryd's pool. Meaningless unless
    // available() is true.
    uint64_t free_bytes() const { return free_bytes_.load(std::memory_order_acquire); }
    uint64_t total_bytes() const { return total_bytes_.load(std::memory_order_acquire); }

    // Performs one query on the calling thread. Exposed for tests and for the
    // initial sample taken before the poll thread starts.
    bool poll_once();

private:
    void run();

    std::string socket_name_;
    uint32_t poll_interval_ms_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> available_{false};
    std::atomic<uint64_t> free_bytes_{0};
    std::atomic<uint64_t> total_bytes_{0};
};

} // namespace discovery
} // namespace meminfo
