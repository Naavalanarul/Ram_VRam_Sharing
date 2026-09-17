// Two-node placement test.
//
// Nothing else in the tree covers the path this exercises. test_cluster.cpp
// runs memclient_cli with --max-local larger than the 1KB it allocates, so the
// block never leaves the local cache and no peer is involved. The unit test
// MemoryClientTest.AllocationLargerThanLocalCacheLivesRemotely does place a
// block remotely, but it hands MemoryClient a hardcoded IP and port and so
// bypasses discoveryd entirely.
//
// Here two full discoveryd+memoryd pairs run on loopback and a real
// MemoryClient goes through discovery to reach them. It covers three things at
// once:
//
//   * Continuous discovery. The client is constructed while only node A is up.
//     Node B starts afterwards, and the allocation must succeed without
//     restarting the client -- connect_to_peers() used to run exactly once, in
//     the constructor, so this was impossible.
//
//   * Real capacity reporting. Node A's pool is deliberately far too small to
//     hold the allocation. The client may only choose B, which it can only know
//     from announced capacity that reflects memoryd's pool rather than the
//     machine's free RAM (both nodes are on the same machine, so the OS figure
//     is identical for the two of them and carries no signal at all).
//
//   * The data really lands on the peer. After the write, memoryd B's control
//     socket must report the pool occupied and memoryd A's must report it
//     empty -- and the readback must return the bytes that were written.

#include <gtest/gtest.h>
#include <uv.h>

#include <meminfo/client/client_api.h>
#include <meminfo/platform/ILocalIpc.h>
#include <control_generated.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if !defined(MEMINFO_DISCOVERYD_EXE) || !defined(MEMINFO_MEMORYD_EXE)
#error "Executable paths must be injected by the build"
#endif

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// Ports and multicast group distinct from every other test in the tree, so a
// parallel ctest run cannot cross-talk.
constexpr int kMemoryPortA = 9470;
constexpr int kMemoryPortB = 9471;
constexpr int kMulticastPort = 9475;
constexpr const char* kMulticastGroup = "239.255.0.12";

// Node A's pool is smaller than the allocation; node B's is comfortably larger.
constexpr size_t kPoolBytesA = 256u * 1024u;
constexpr size_t kPoolBytesB = 64u * 1024u * 1024u;
constexpr size_t kMaxLocalBytes = 1u * 1024u * 1024u;
constexpr size_t kAllocBytes = 4u * 1024u * 1024u;

struct Process {
    uv_process_t handle{};
    bool exited = false;
    int64_t exit_status = -1;
    bool spawned = false;
};

void on_process_exit(uv_process_t* proc, int64_t status, int /*term_signal*/) {
    auto* p = static_cast<Process*>(proc->data);
    p->exited = true;
    p->exit_status = status;
    uv_close(reinterpret_cast<uv_handle_t*>(proc), nullptr);
}

int spawn(uv_loop_t* loop, Process& p, const std::vector<std::string>& argv) {
    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const auto& a : argv) {
        args.push_back(const_cast<char*>(a.c_str()));
    }
    args.push_back(nullptr);

    uv_stdio_container_t stdio[3];
    stdio[0].flags = UV_IGNORE;
    stdio[1].flags = UV_IGNORE;
    stdio[2].flags = UV_IGNORE;

    uv_process_options_t opts{};
    opts.exit_cb = on_process_exit;
    opts.file = args[0];
    opts.args = args.data();
    opts.stdio_count = 3;
    opts.stdio = stdio;

    p.handle.data = &p;
    int rc = uv_spawn(loop, &p.handle, &opts);
    p.spawned = (rc == 0);
    return rc;
}

void write_file(const fs::path& path, const std::string& contents) {
    std::ofstream out(path);
    out << contents;
}

std::string discovery_config(int memory_port, const std::string& control_socket,
                             const std::string& memory_control_socket) {
    return "[discovery]\n"
           "listen_address = \"127.0.0.1\"\n"
           "control_socket = \"" + control_socket + "\"\n"
           "memory_control_socket = \"" + memory_control_socket + "\"\n"
           "multicast_group = \"" + std::string(kMulticastGroup) + "\"\n"
           "multicast_port = " + std::to_string(kMulticastPort) + "\n"
           "announce_interval_ms = 250\n"
           "peer_ttl_seconds = 30\n"
           "memory_port = " + std::to_string(memory_port) + "\n"
           "gpu_port = 0\n";
}

std::string memory_config(int port, size_t pool_bytes, const std::string& control_socket) {
    return "[memory]\n"
           "listen_address = \"127.0.0.1\"\n"
           "port = " + std::to_string(port) + "\n"
           "total_reserved_bytes = " + std::to_string(pool_bytes) + "\n"
           "page_size_bytes = 4096\n"
           "control_socket = \"" + control_socket + "\"\n";
}

struct PoolStats {
    bool ok = false;
    uint64_t total = 0;
    uint64_t used = 0;
    uint64_t free_bytes = 0;
};

// Asks a memoryd control socket how much of its pool is handed out.
PoolStats query_pool(const std::string& socket_name) {
    PoolStats stats;

    flatbuffers::FlatBufferBuilder builder;
    meminfo::control::ControlRequestBuilder crb(builder);
    crb.add_command(meminfo::control::ControlCommand_GET_MEMORY_STATS);
    builder.FinishSizePrefixed(crb.Finish());
    const std::vector<uint8_t> req(builder.GetBufferPointer(),
                                   builder.GetBufferPointer() + builder.GetSize());

    auto ipc = meminfo::platform::create_local_ipc();
    auto resp_buf = ipc->send_request(socket_name, req);
    if (resp_buf.size() < sizeof(flatbuffers::uoffset_t)) return stats;

    flatbuffers::Verifier verifier(resp_buf.data(), resp_buf.size());
    if (!verifier.VerifySizePrefixedBuffer<meminfo::control::ControlResponse>(nullptr)) return stats;

    const auto* resp = flatbuffers::GetSizePrefixedRoot<meminfo::control::ControlResponse>(resp_buf.data());
    if (!resp->success()) return stats;

    stats.ok = true;
    stats.total = resp->pool_total_bytes();
    stats.used = resp->pool_used_bytes();
    stats.free_bytes = resp->pool_free_bytes();
    return stats;
}

// Waits until pred(stats) holds for the given socket, or the deadline passes.
template <typename Pred>
bool wait_for_pool(const std::string& socket_name, Pred pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred(query_pool(socket_name))) return true;
        std::this_thread::sleep_for(100ms);
    }
    return pred(query_pool(socket_name));
}

} // namespace

TEST(TwoNodePlacementTest, OversizedBlockLandsOnLateJoiningPeer) {
    const fs::path dir = fs::temp_directory_path() / "meminfo_two_node_test";
    std::error_code ec;
    fs::create_directories(dir, ec);
    ASSERT_FALSE(ec) << "Could not create " << dir.string() << ": " << ec.message();

    // Bare names: a UDS path relative to the working directory on POSIX, and a
    // valid named-pipe name on Windows, which forbids backslashes.
    const std::string disc_sock_a = "meminfo_2n_disc_a";
    const std::string disc_sock_b = "meminfo_2n_disc_b";
    const std::string mem_sock_a = "meminfo_2n_mem_a";
    const std::string mem_sock_b = "meminfo_2n_mem_b";

    const fs::path disc_cfg_a = dir / "discoveryd_a.toml";
    const fs::path disc_cfg_b = dir / "discoveryd_b.toml";
    const fs::path mem_cfg_a = dir / "memoryd_a.toml";
    const fs::path mem_cfg_b = dir / "memoryd_b.toml";

    write_file(disc_cfg_a, discovery_config(kMemoryPortA, disc_sock_a, mem_sock_a));
    write_file(disc_cfg_b, discovery_config(kMemoryPortB, disc_sock_b, mem_sock_b));
    write_file(mem_cfg_a, memory_config(kMemoryPortA, kPoolBytesA, mem_sock_a));
    write_file(mem_cfg_b, memory_config(kMemoryPortB, kPoolBytesB, mem_sock_b));

    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);

    Process memoryd_a, discoveryd_a, memoryd_b, discoveryd_b;

    // Everything below runs inside this lambda so the teardown at the bottom
    // always executes, even when an assertion stops the body early.
    auto body = [&]() {
        int rc = spawn(&loop, memoryd_a, {MEMINFO_MEMORYD_EXE, "--config", mem_cfg_a.string()});
        ASSERT_EQ(rc, 0) << "spawn memoryd A: " << uv_strerror(rc);
        rc = spawn(&loop, discoveryd_a, {MEMINFO_DISCOVERYD_EXE, "--config", disc_cfg_a.string()});
        ASSERT_EQ(rc, 0) << "spawn discoveryd A: " << uv_strerror(rc);

        // Node A's control socket must answer before the client can use it.
        ASSERT_TRUE(wait_for_pool(mem_sock_a, [](const PoolStats& s) { return s.ok; }, 10000ms))
            << "memoryd A's control socket never answered";
        const PoolStats a_initial = query_pool(mem_sock_a);
        EXPECT_EQ(a_initial.total, kPoolBytesA);
        EXPECT_EQ(a_initial.used, 0u);

        // Client comes up while node A is the only peer in the pool. Its own
        // pool is far too small for the allocation below.
        meminfo::client::MemoryClient client(kMaxLocalBytes, disc_sock_a);
        std::this_thread::sleep_for(1000ms);

        // Node B joins late. Nothing about the client is restarted.
        rc = spawn(&loop, memoryd_b, {MEMINFO_MEMORYD_EXE, "--config", mem_cfg_b.string()});
        ASSERT_EQ(rc, 0) << "spawn memoryd B: " << uv_strerror(rc);
        rc = spawn(&loop, discoveryd_b, {MEMINFO_DISCOVERYD_EXE, "--config", disc_cfg_b.string()});
        ASSERT_EQ(rc, 0) << "spawn discoveryd B: " << uv_strerror(rc);

        ASSERT_TRUE(wait_for_pool(mem_sock_b, [](const PoolStats& s) { return s.ok; }, 10000ms))
            << "memoryd B's control socket never answered";

        // The allocation is larger than the local cache, so it can only be
        // satisfied remotely, and larger than node A's whole pool, so only node
        // B can take it. Retried because discovery is asynchronous: the peer
        // table, the announced capacity and the client's own refresh each take
        // a beat. Without continuous discovery this never succeeds at all.
        meminfo::handle_t handle = 0;
        std::string last_error;
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (std::chrono::steady_clock::now() < deadline) {
            try {
                handle = client.allocate(kAllocBytes);
                break;
            } catch (const std::exception& e) {
                last_error = e.what();
                std::this_thread::sleep_for(250ms);
            }
        }
        ASSERT_NE(handle, 0u) << "allocation never succeeded; last error: " << last_error;

        // A recognisable pattern, written and read back across the wire.
        std::vector<uint8_t> pattern(kAllocBytes);
        for (size_t i = 0; i < pattern.size(); ++i) {
            pattern[i] = static_cast<uint8_t>((i * 31u + 7u) & 0xFFu);
        }
        ASSERT_NO_THROW(client.write(handle, 0, pattern));

        // The bytes are on node B, not in this process: its pool is occupied
        // and node A's is untouched.
        const PoolStats b_after = query_pool(mem_sock_b);
        const PoolStats a_after = query_pool(mem_sock_a);
        ASSERT_TRUE(b_after.ok);
        ASSERT_TRUE(a_after.ok);
        EXPECT_GE(b_after.used, kAllocBytes) << "node B does not hold the allocation";
        EXPECT_EQ(a_after.used, 0u) << "node A took an allocation its pool cannot hold";

        // Readback, in pieces, to exercise offsets rather than one whole-block
        // fetch.
        const size_t chunk = 64u * 1024u;
        for (size_t offset = 0; offset + chunk <= kAllocBytes; offset += chunk * 7) {
            std::vector<uint8_t> got;
            ASSERT_NO_THROW(got = client.read(handle, offset, chunk));
            ASSERT_EQ(got.size(), chunk);
            EXPECT_EQ(std::vector<uint8_t>(pattern.begin() + static_cast<long>(offset),
                                           pattern.begin() + static_cast<long>(offset + chunk)),
                      got)
                << "readback mismatch at offset " << offset;
        }

        // Freeing gives the pages back to node B's pool.
        ASSERT_NO_THROW(client.free(handle));
        EXPECT_TRUE(wait_for_pool(mem_sock_b, [](const PoolStats& s) { return s.ok && s.used == 0; }, 5000ms))
            << "node B did not release the pages on free";
    };

    body();

    for (Process* p : {&discoveryd_a, &discoveryd_b, &memoryd_a, &memoryd_b}) {
        if (p->spawned && !p->exited) {
            uv_process_kill(&p->handle, SIGTERM);
        }
    }

    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        uv_run(&loop, UV_RUN_NOWAIT);
        const bool all_done =
            (!discoveryd_a.spawned || discoveryd_a.exited) &&
            (!discoveryd_b.spawned || discoveryd_b.exited) &&
            (!memoryd_a.spawned || memoryd_a.exited) &&
            (!memoryd_b.spawned || memoryd_b.exited);
        if (all_done) break;
        std::this_thread::sleep_for(20ms);
    }

    uv_run(&loop, UV_RUN_DEFAULT);
    EXPECT_EQ(uv_loop_close(&loop), 0);

    std::error_code cleanup_ec;
    fs::remove_all(dir, cleanup_ec);
}
