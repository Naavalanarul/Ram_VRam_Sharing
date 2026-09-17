#include <gtest/gtest.h>
#include <meminfo/platform/IMemoryMonitor.h>
#include <meminfo/platform/IPageFaultBackend.h>
#include <meminfo/platform/ILocalIpc.h>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>
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

    // Linux userfaultfd needs CAP_SYS_PTRACE unless vm.unprivileged_userfaultfd
    // is set; CI runners generally allow neither. Skip rather than fail, so the
    // assertions below still run wherever the capability does exist.
    if (!fault->is_supported()) {
        GTEST_SKIP() << "Page-fault backend unavailable on this platform/permissions";
    }

    const size_t page = fault->page_size();
    auto region = fault->reserve_region(page);

    ASSERT_NE(region.addr, nullptr);
    ASSERT_GE(region.size, page);

    std::atomic<bool> faulted{false};
    fault->on_fault([&](const FaultInfo& info) {
        faulted = true;
        std::string msg = "Hello from fault";
        fault->resolve_fault(info.addr, msg.c_str(), msg.size() + 1);
    });

    // Trigger fault
    char* ptr = reinterpret_cast<char*>(region.addr);
    volatile char c = ptr[0]; // should trigger fault
    (void)c;

    EXPECT_TRUE(faulted.load());
    EXPECT_STREQ(ptr, "Hello from fault");

    fault->release_region(region.addr);
}

// Reserving does not consume physical memory, filling a page does, and evicting
// gives it back. The last step is the one that makes the local process's memory
// figure fall while the data lives on a peer.
TEST(PlatformTest, PageFaultEvictionReleasesResidency) {
    auto fault = create_page_fault_backend();
    if (!fault->is_supported()) {
        GTEST_SKIP() << "Page-fault backend unavailable on this platform/permissions";
    }

    const size_t page = fault->page_size();
    const size_t pages = 8;
    auto region = fault->reserve_region(page * pages);
    ASSERT_NE(region.addr, nullptr);

    // Reserved address space is not resident.
    EXPECT_EQ(fault->resident_pages(), 0u);

    std::atomic<int> fills{0};
    fault->on_fault([&](const FaultInfo& info) {
        ++fills;
        // Each page is filled with its own index, so a readback after eviction
        // can tell a genuine refill from a stale mapping.
        auto* base = static_cast<uint8_t*>(info.addr);
        const size_t offset = static_cast<size_t>(base - static_cast<uint8_t*>(region.addr));
        std::vector<uint8_t> contents(page, static_cast<uint8_t>(offset / page));
        fault->resolve_fault(info.addr, contents.data(), contents.size());
    });

    auto* bytes = static_cast<volatile uint8_t*>(region.addr);
    for (size_t i = 0; i < pages; ++i) {
        EXPECT_EQ(bytes[i * page], static_cast<uint8_t>(i)) << "page " << i << " read back wrong";
    }
    EXPECT_EQ(fills.load(), static_cast<int>(pages));
    EXPECT_EQ(fault->resident_pages(), pages);

    ASSERT_TRUE(fault->evict_pages(region.addr, page * pages));
    EXPECT_EQ(fault->resident_pages(), 0u);

    // Touching an evicted page faults again and refetches it.
    EXPECT_EQ(bytes[0], 0u);
    EXPECT_EQ(fills.load(), static_cast<int>(pages) + 1);
    EXPECT_EQ(fault->resident_pages(), 1u);

    fault->release_region(region.addr);
}

// A page brought in by a load starts clean. A store to it must be observable,
// so the eviction path knows to flush it rather than discard it.
TEST(PlatformTest, PageFaultTracksWrites) {
    auto fault = create_page_fault_backend();
    if (!fault->is_supported()) {
        GTEST_SKIP() << "Page-fault backend unavailable on this platform/permissions";
    }

    const size_t page = fault->page_size();
    auto region = fault->reserve_region(page);
    ASSERT_NE(region.addr, nullptr);

    fault->on_fault([&](const FaultInfo& info) {
        std::vector<uint8_t> zeros(page, 0);
        fault->resolve_fault(info.addr, zeros.data(), zeros.size());
    });

    auto* bytes = static_cast<uint8_t*>(region.addr);

    volatile uint8_t observed = bytes[0]; // load fault brings the page in
    (void)observed;

    bytes[0] = 0x5A; // must not be lost, and must leave the page dirty
    EXPECT_EQ(bytes[0], 0x5A);
    EXPECT_TRUE(fault->is_dirty(region.addr, page))
        << "a written page reported clean: eviction would discard the write";

    fault->clear_dirty(region.addr, page);
    bytes[1] = 0x77;
    EXPECT_EQ(bytes[1], 0x77);
    EXPECT_TRUE(fault->is_dirty(region.addr, page))
        << "a page written after clear_dirty reported clean";

    fault->release_region(region.addr);
}
