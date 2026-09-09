#include <gtest/gtest.h>
#include <meminfo/gpu/gpu_daemon.h>
#include <meminfo/gpu/gpu_client.h>
#include <meminfo/common/config.h>
#include <thread>
#include <chrono>
#include <fstream>
#include <vector>

using namespace meminfo;
using namespace meminfo::gpu;

TEST(GpuProtocolTest, EndToEndFlow) {
    // 1. Create a GpuDaemon on a random port
    std::string config_path = "/tmp/meminfo_test_gpud.toml";
    std::ofstream out(config_path);
    out << "[gpu]\n"
        << "listen_address = \"127.0.0.1\"\n"
        << "port = 9355\n";
    out.close();
    
    Config config(config_path);
    auto daemon = std::make_unique<GpuDaemon>(config);
    std::thread daemon_thread([&]() { daemon->run(); });
    
    // Give it a moment to start
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // 2. Connect client
    GpuClient client("127.0.0.1", 9355);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // 3. Get properties
    std::string name;
    size_t mem = 0;
    int err = client.cudaGetDeviceProperties(name, mem);
    EXPECT_EQ(err, 0);
    EXPECT_FALSE(name.empty());
    EXPECT_GT(mem, 0);
    
    // 4. Malloc
    uint64_t devPtr = 0;
    err = client.cudaMalloc(&devPtr, 1024);
    EXPECT_EQ(err, 0);
    EXPECT_NE(devPtr, 0);
    
    // 5. Memcpy Host to Device
    std::vector<uint8_t> h_data(1024, 0xAB);
    err = client.cudaMemcpyHtoD(devPtr, h_data.data(), h_data.size());
    EXPECT_EQ(err, 0);
    
    // 6. Memcpy Device to Host
    std::vector<uint8_t> h_out(1024, 0);
    err = client.cudaMemcpyDtoH(h_out.data(), devPtr, h_out.size());
    EXPECT_EQ(err, 0);
    EXPECT_EQ(h_out, h_data); // should match!
    
    // 7. Free
    err = client.cudaFree(devPtr);
    EXPECT_EQ(err, 0);
    
    // 8. Cleanup
    daemon->stop();
    daemon_thread.join();
}
