#include <gtest/gtest.h>
#include <meminfo/client/client_api.h>
#include <meminfo/memory/memory_daemon.h>
#include <meminfo/common/config.h>
#include <thread>
#include <fstream>
#include <chrono>

using namespace meminfo;
using namespace meminfo::client;
using namespace meminfo::memory;

TEST(MemoryClientTest, EvictionAndReadback) {
    // 1. Create a MemoryDaemon on a random port
    std::string config_path = "/tmp/meminfo_test_memoryd.toml";
    std::ofstream out(config_path);
    out << "[memory]\n"
        << "listen_address = \"127.0.0.1\"\n"
        << "port = 9255\n"
        << "total_reserved_bytes = 1048576\n"
        << "page_size_bytes = 4096\n";
    out.close();
    
    Config config(config_path);
    auto daemon = std::make_unique<MemoryDaemon>(config);
    std::thread daemon_thread([&]() { daemon->run(); });
    
    // Give it a moment to start
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // 2. Initialize the client with a tiny local cache (only 20 bytes)
    MemoryClient client(20, "", "127.0.0.1", 9255);
    
    // Give client a moment to connect
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // 3. Allocate handle 1 (10 bytes), fits in local
    handle_t h1 = client.allocate(10);
    std::vector<uint8_t> data1(10, 'A');
    client.write(h1, 0, data1);
    
    // 4. Allocate handle 2 (15 bytes), forces h1 to evict
    handle_t h2 = client.allocate(15);
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
