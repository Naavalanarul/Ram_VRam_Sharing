#include <gtest/gtest.h>
#include <meminfo/discovery/discovery_daemon.h>
#include <meminfo/common/config.h>
#include <meminfo/platform/ILocalIpc.h>
#include <control_generated.h>
#include <thread>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace meminfo;
using namespace meminfo::discovery;

TEST(MultiNodeTest, ThreeNodesDiscoverEachOther) {
    auto write_config = [](const std::string& path, int memory_port, int gpu_port, const std::string& sock) {
        std::ofstream out(path);
        out << "[discovery]\n";
        out << "listen_address = \"127.0.0.1\"\n";
        out << "memory_port = " << memory_port << "\n";
        out << "gpu_port = " << gpu_port << "\n";
        out << "control_socket = \"" << sock << "\"\n";
        out << "multicast_group = \"239.255.73.77\"\n";
        out << "multicast_port = 9102\n"; // different from other tests
        out << "announce_interval_ms = 100\n";
    };
    
    std::string cfg1 = "/tmp/meminfo_test_d1.toml";
    std::string cfg2 = "/tmp/meminfo_test_d2.toml";
    std::string cfg3 = "/tmp/meminfo_test_d3.toml";
    
    write_config(cfg1, 9201, 9301, "/tmp/sock1");
    write_config(cfg2, 9202, 9302, "/tmp/sock2");
    write_config(cfg3, 9203, 9303, "/tmp/sock3");
    
    Config c1(cfg1), c2(cfg2), c3(cfg3);
    
    std::unique_ptr<DiscoveryDaemon> d1 = std::make_unique<DiscoveryDaemon>(c1);
    std::unique_ptr<DiscoveryDaemon> d2 = std::make_unique<DiscoveryDaemon>(c2);
    std::unique_ptr<DiscoveryDaemon> d3 = std::make_unique<DiscoveryDaemon>(c3);
    
    std::thread t1([&]() { d1->run(); });
    std::thread t2([&]() { d2->run(); });
    std::thread t3([&]() { d3->run(); });
    
    // Give them time to start up, announce, and discover each other
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // Helper to query control socket
    auto query_peers = [](const std::string& sock_path) -> size_t {
        auto ipc = platform::create_local_ipc();
        
        flatbuffers::FlatBufferBuilder builder;
        meminfo::control::ControlRequestBuilder crb(builder);
        crb.add_command(meminfo::control::ControlCommand_LIST_PEERS);
        builder.FinishSizePrefixed(crb.Finish());
        
        std::vector<uint8_t> req(builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize());
        
        size_t count = 0;
        for (int i = 0; i < 5; ++i) {
            auto resp_data = ipc->send_request(sock_path, req);
            if (!resp_data.empty()) {
                const auto* resp = flatbuffers::GetSizePrefixedRoot<meminfo::control::ControlResponse>(resp_data.data());
                if (resp && resp->peers()) {
                    count = resp->peers()->size();
                    if (count >= 2) break; // found peers (itself + others)
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        return count;
    };
    
    size_t peers1 = query_peers("/tmp/sock1");
    size_t peers2 = query_peers("/tmp/sock2");
    size_t peers3 = query_peers("/tmp/sock3");
    
    // Each should see the other 2 peers
    EXPECT_EQ(peers1, 3);
    EXPECT_EQ(peers2, 3);
    EXPECT_EQ(peers3, 3);
    
    // Stop them all
    d1->stop();
    d2->stop();
    d3->stop();
    
    t1.join();
    t2.join();
    t3.join();
    
    d1.reset();
    d2.reset();
    d3.reset();
}
