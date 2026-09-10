// End-to-end cluster test.
//
// This used to shell out to scripts/run_cluster_test.sh, which confined it to
// POSIX: std::system() runs cmd.exe on Windows, and the script also addressed
// binaries as <build>/<target>/<name>, a layout that only holds for
// single-config generators. The harness is now written against libuv's process
// API, which is portable, and CMake injects each executable's real path through
// $<TARGET_FILE:...> so multi-config layouts and the .exe suffix are handled by
// the build system rather than guessed at.

#include <gtest/gtest.h>
#include <uv.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if !defined(MEMINFO_DISCOVERYD_EXE) || !defined(MEMINFO_MEMORYD_EXE) || \
    !defined(MEMINFO_GPUD_EXE) || !defined(MEMINFO_MEMCLIENT_CLI_EXE)
#error "Executable paths must be injected by the build"
#endif

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

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

// Spawns argv[0] with the remaining arguments. Daemons run with their output
// discarded; the client inherits stdout/stderr so its log reaches the CI job.
int spawn(uv_loop_t* loop, Process& p, const std::vector<std::string>& argv, bool inherit_stdio) {
    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const auto& a : argv) {
        args.push_back(const_cast<char*>(a.c_str()));
    }
    args.push_back(nullptr);

    uv_stdio_container_t stdio[3];
    stdio[0].flags = UV_IGNORE;
    stdio[1].flags = inherit_stdio ? UV_INHERIT_FD : UV_IGNORE;
    stdio[1].data.fd = 1;
    stdio[2].flags = inherit_stdio ? UV_INHERIT_FD : UV_IGNORE;
    stdio[2].data.fd = 2;

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

// Pumps the loop until pred() holds or the deadline passes.
template <typename Pred>
bool pump_until(uv_loop_t* loop, Pred pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        uv_run(loop, UV_RUN_NOWAIT);
        if (pred()) return true;
        std::this_thread::sleep_for(20ms);
    }
    uv_run(loop, UV_RUN_NOWAIT);
    return pred();
}

void write_file(const fs::path& path, const std::string& contents) {
    std::ofstream out(path);
    out << contents;
}

} // namespace

TEST(ClusterIntegrationTest, RunCluster) {
    const fs::path dir = fs::temp_directory_path() / "meminfo_cluster_test";
    std::error_code ec;
    fs::create_directories(dir, ec);
    ASSERT_FALSE(ec) << "Could not create " << dir.string() << ": " << ec.message();

    // A bare name: a UDS path relative to the working directory on POSIX, and a
    // valid named-pipe name on Windows, which forbids backslashes in the name.
    const std::string control_socket = "meminfo_cluster_ctl";

    // Ports distinct from the other tests so a parallel or leftover run cannot
    // collide with this one.
    const fs::path discovery_cfg = dir / "discoveryd.toml";
    const fs::path memory_cfg    = dir / "memoryd.toml";
    const fs::path gpu_cfg       = dir / "gpud.toml";

    write_file(discovery_cfg,
               "[discovery]\n"
               "listen_address = \"127.0.0.1\"\n"
               "control_socket = \"" + control_socket + "\"\n"
               "multicast_group = \"239.255.0.11\"\n"
               "multicast_port = 9457\n"
               "announce_interval_ms = 500\n"
               "memory_port = 9455\n"
               "gpu_port = 9456\n");

    write_file(memory_cfg,
               "[memory]\n"
               "listen_address = \"127.0.0.1\"\n"
               "port = 9455\n"
               "total_reserved_bytes = 10485760\n"
               "page_size_bytes = 4096\n");

    write_file(gpu_cfg,
               "[gpu]\n"
               "listen_address = \"127.0.0.1\"\n"
               "port = 9456\n");

    uv_loop_t loop;
    ASSERT_EQ(uv_loop_init(&loop), 0);

    Process discoveryd, memoryd, gpud, client;

    int rc = spawn(&loop, discoveryd, {MEMINFO_DISCOVERYD_EXE, "--config", discovery_cfg.string()}, false);
    ASSERT_EQ(rc, 0) << "spawn discoveryd: " << uv_strerror(rc);

    rc = spawn(&loop, memoryd, {MEMINFO_MEMORYD_EXE, "--config", memory_cfg.string()}, false);
    ASSERT_EQ(rc, 0) << "spawn memoryd: " << uv_strerror(rc);

    rc = spawn(&loop, gpud, {MEMINFO_GPUD_EXE, "--config", gpu_cfg.string()}, false);
    ASSERT_EQ(rc, 0) << "spawn gpud: " << uv_strerror(rc);

    // Give the daemons time to bind and publish the control socket. If one dies
    // immediately, stop waiting and report it rather than timing out later.
    pump_until(&loop,
               [&] { return discoveryd.exited || memoryd.exited || gpud.exited; },
               2000ms);

    EXPECT_FALSE(discoveryd.exited) << "discoveryd exited early, status " << discoveryd.exit_status;
    EXPECT_FALSE(memoryd.exited) << "memoryd exited early, status " << memoryd.exit_status;
    EXPECT_FALSE(gpud.exited) << "gpud exited early, status " << gpud.exit_status;

    if (!discoveryd.exited && !memoryd.exited && !gpud.exited) {
        rc = spawn(&loop, client,
                   {MEMINFO_MEMCLIENT_CLI_EXE, "--socket", control_socket, "--max-local", "2048"},
                   true);
        EXPECT_EQ(rc, 0) << "spawn memclient_cli: " << uv_strerror(rc);

        if (rc == 0) {
            EXPECT_TRUE(pump_until(&loop, [&] { return client.exited; }, 30000ms))
                << "memclient_cli did not exit within 30s";
            EXPECT_EQ(client.exit_status, 0) << "memclient_cli failed";
        }
    }

    // Tear the daemons down and drain the loop so every handle is closed before
    // uv_loop_close(). SIGTERM is what they install a handler for; libuv maps it
    // to TerminateProcess on Windows.
    for (Process* p : {&discoveryd, &memoryd, &gpud, &client}) {
        if (p->spawned && !p->exited) {
            uv_process_kill(&p->handle, SIGTERM);
        }
    }

    pump_until(&loop,
               [&] {
                   return (!discoveryd.spawned || discoveryd.exited) &&
                          (!memoryd.spawned || memoryd.exited) &&
                          (!gpud.spawned || gpud.exited) &&
                          (!client.spawned || client.exited);
               },
               10000ms);

    uv_run(&loop, UV_RUN_DEFAULT);
    EXPECT_EQ(uv_loop_close(&loop), 0);

    fs::remove_all(dir, ec);
}
