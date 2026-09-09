#include <gtest/gtest.h>
#include <meminfo/platform/IMemoryMonitor.h>
#include <meminfo/platform/IPageFaultBackend.h>
#include <meminfo/platform/ILocalIpc.h>
#include <thread>
#include <chrono>

using namespace meminfo::platform;

TEST(PlatformTest, MemoryMonitor) {
    auto monitor = create_memory_monitor();
    auto stats = monitor->get_stats();
    EXPECT_GT(stats.total_bytes, 0);
    EXPECT_GT(stats.free_bytes, 0);
    EXPECT_GT(stats.available_bytes, 0);
}

TEST(PlatformTest, LocalIpc) {
    auto ipc_server = create_local_ipc();
    auto ipc_client = create_local_ipc();
    
    std::string pipe_name = "test_pipe_meminfo";
    
    ipc_server->listen(pipe_name, [](const std::vector<uint8_t>& req, std::vector<uint8_t>& resp) {
        resp = req; // echo back
        resp.push_back(0xAA);
    });
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    std::vector<uint8_t> req = {0x01, 0x02, 0x03};
    auto resp = ipc_client->send_request(pipe_name, req);
    
    ASSERT_EQ(resp.size(), 4);
    EXPECT_EQ(resp[0], 0x01);
    EXPECT_EQ(resp[3], 0xAA);
}

TEST(PlatformTest, PageFault) {
    auto fault = create_page_fault_backend();
    auto region = fault->reserve_region(4096);
    
    ASSERT_NE(region.addr, nullptr);
    ASSERT_EQ(region.size, 4096);
    
    std::atomic<bool> faulted{false};
    fault->on_fault([&](void* addr) {
        faulted = true;
        std::string msg = "Hello from fault";
        fault->resolve_fault(addr, msg.c_str(), msg.size() + 1);
    });
    
    // Trigger fault
    char* ptr = reinterpret_cast<char*>(region.addr);
    volatile char c = ptr[0]; // should trigger fault
    (void)c;
    
    EXPECT_TRUE(faulted.load());
    EXPECT_STREQ(ptr, "Hello from fault");
}
