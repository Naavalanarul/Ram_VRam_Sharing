#include <meminfo/discovery/pool_probe.h>

#include <meminfo/platform/ILocalIpc.h>
#include <control_generated.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <utility>
#include <vector>

namespace meminfo {
namespace discovery {

PoolProbe::PoolProbe(std::string socket_name, uint32_t poll_interval_ms)
    : socket_name_(std::move(socket_name)),
      poll_interval_ms_(poll_interval_ms == 0 ? 1000u : poll_interval_ms) {}

PoolProbe::~PoolProbe() {
    stop();
}

void PoolProbe::start() {
    if (socket_name_.empty() || running_.exchange(true)) return;
    thread_ = std::thread(&PoolProbe::run, this);
}

void PoolProbe::stop() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool PoolProbe::poll_once() {
    if (socket_name_.empty()) return false;

    flatbuffers::FlatBufferBuilder builder;
    meminfo::control::ControlRequestBuilder crb(builder);
    crb.add_command(meminfo::control::ControlCommand_GET_MEMORY_STATS);
    builder.FinishSizePrefixed(crb.Finish());

    const std::vector<uint8_t> req(builder.GetBufferPointer(),
                                   builder.GetBufferPointer() + builder.GetSize());

    std::vector<uint8_t> resp_buf;
    try {
        // A fresh client per query: ILocalIpc holds no connection between
        // requests, and a memoryd restart must not leave a dead handle behind.
        auto ipc = platform::create_local_ipc();
        resp_buf = ipc->send_request(socket_name_, req);
    } catch (const std::exception& e) {
        spdlog::debug("Pool probe request failed: {}", e.what());
        available_.store(false, std::memory_order_release);
        return false;
    }

    if (resp_buf.size() < 4) {
        // memoryd is not running, or its control socket is disabled.
        available_.store(false, std::memory_order_release);
        return false;
    }

    // ControlResponse is not the schema's root_type, so flatbuffers generates
    // no VerifySizePrefixed<...>Buffer helper for it; verify it explicitly.
    flatbuffers::Verifier verifier(resp_buf.data(), resp_buf.size());
    if (!verifier.VerifySizePrefixedBuffer<meminfo::control::ControlResponse>(nullptr)) {
        spdlog::warn("Pool probe received a malformed response from {}", socket_name_);
        available_.store(false, std::memory_order_release);
        return false;
    }

    const auto* resp = flatbuffers::GetSizePrefixedRoot<meminfo::control::ControlResponse>(resp_buf.data());
    if (!resp->success()) {
        available_.store(false, std::memory_order_release);
        return false;
    }

    free_bytes_.store(resp->pool_free_bytes(), std::memory_order_release);
    total_bytes_.store(resp->pool_total_bytes(), std::memory_order_release);
    available_.store(true, std::memory_order_release);
    return true;
}

void PoolProbe::run() {
    bool was_available = false;

    while (running_.load(std::memory_order_acquire)) {
        const bool ok = poll_once();
        if (ok != was_available) {
            if (ok) {
                spdlog::info("Announcing memoryd pool capacity from {} ({} bytes reserved)",
                             socket_name_, total_bytes());
            } else {
                spdlog::warn("memoryd pool stats unavailable on {}; falling back to OS free RAM",
                             socket_name_);
            }
            was_available = ok;
        }

        // Sleep in slices so stop() is not held up for a whole interval.
        for (uint32_t waited = 0;
             waited < poll_interval_ms_ && running_.load(std::memory_order_acquire);
             waited += 50) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

} // namespace discovery
} // namespace meminfo
