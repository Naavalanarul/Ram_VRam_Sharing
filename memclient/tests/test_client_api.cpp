#include <gtest/gtest.h>
#include <meminfo/client/client_api.h>
#include <meminfo/memory/memory_daemon.h>
#include <meminfo/common/config.h>
#include <thread>
#include <fstream>
#include <chrono>
#include <filesystem>

using namespace meminfo;
using namespace meminfo::client;
using namespace meminfo::memory;

TEST(MemoryClientTest, EvictionAndReadback) {
    // 1. Create a MemoryDaemon on an OS-assigned port. A fixed port collides
    //    with the cluster integration script (which also binds 9255) and with
    //    any daemon a previous test left behind.
    std::string config_path = (std::filesystem::temp_directory_path() / "meminfo_test_client_api.toml").string();
    std::ofstream out(config_path);
    out << "[memory]\n"
        << "listen_address = \"127.0.0.1\"\n"
        << "port = 0\n"
        << "total_reserved_bytes = 1048576\n"
        << "page_size_bytes = 4096\n";
    out.close();

    Config config(config_path);
    auto daemon = std::make_unique<MemoryDaemon>(config);
    std::thread daemon_thread([&]() { daemon->run(); });

    // Wait for the daemon to bind so get_listen_port() reports the real port.
    int port = 0;
    for (int i = 0; i < 200 && port == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        port = daemon->get_listen_port();
    }
    ASSERT_NE(port, 0) << "memoryd did not bind a port";

    // 2. Initialize the client with a tiny local cache (only 20 bytes)
    MemoryClient client(20, "", "127.0.0.1", port);
    
    // Give client a moment to connect
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // 3. Allocate handle 1 (10 bytes), fits in local
    meminfo::handle_t h1 = client.allocate(10);
    std::vector<uint8_t> data1(10, 'A');
    client.write(h1, 0, data1);
    
    // 4. Allocate handle 2 (15 bytes), forces h1 to evict
    meminfo::handle_t h2 = client.allocate(15);
    std::vector<uint8_t> data2(15, 'B');
    client.write(h2, 0, data2);
    
    // 5. Read back h1. This forces h2 to evict, and h1 to load from remote
    auto read1 = client.read(h1, 0, 10);
    EXPECT_EQ(read1, data1);
    
    // 6. Read back h2. This forces h1 to evict, and h2 to load from remote
    auto read2 = client.read(h2, 0, 15);
    EXPECT_EQ(read2, data2);
    
    // 7. Cleanup
    client.free(h1);
    client.free(h2);
    
    daemon->stop();
    daemon_thread.join();
}
