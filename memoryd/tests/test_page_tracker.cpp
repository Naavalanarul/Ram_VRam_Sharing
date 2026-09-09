#include <thread>

#include <gtest/gtest.h>
#include <meminfo/memory/slab_allocator.h>
#include <meminfo/memory/page_tracker.h>

using namespace meminfo::memory;

TEST(PageTrackerTest, AllocateAndFree) {
    SlabAllocator alloc(4096 * 10, 4096);
    PageTracker tracker(&alloc);
    
    EXPECT_EQ(alloc.free_pages_count(), 10);
    
    // Allocate 8000 bytes (requires 2 pages)
    meminfo::handle_t h1 = tracker.allocate(8000);
    EXPECT_GT(h1, 0);
    EXPECT_EQ(alloc.free_pages_count(), 8);
    
    tracker.free(h1);
    EXPECT_EQ(alloc.free_pages_count(), 10);
}

TEST(PageTrackerTest, WriteAndReadAcrossPages) {
    SlabAllocator alloc(4096 * 10, 4096);
    PageTracker tracker(&alloc);
    
    meminfo::handle_t h1 = tracker.allocate(8000); // 2 pages
    
    std::vector<uint8_t> write_data(5000, 0xAA);
    // Write 5000 bytes starting at offset 100. This will span across 2 pages
    tracker.write(h1, 100, write_data.data(), write_data.size());
    
    std::vector<uint8_t> read_data(5000, 0);
    tracker.read(h1, 100, read_data.data(), read_data.size());
    
    EXPECT_EQ(read_data, write_data);
    
    // Test boundaries
    EXPECT_THROW(tracker.write(h1, 8000, write_data.data(), 1), std::out_of_range);
}

TEST(PageTrackerTest, OwnerTracking) {
    SlabAllocator alloc(4096 * 10, 4096);
    PageTracker tracker(&alloc);
    
    meminfo::handle_t h1 = tracker.allocate(100);
    meminfo::handle_t h2 = tracker.allocate(100);
    
    tracker.assign_owner(h1, 42);
    tracker.assign_owner(h2, 42);
    
    EXPECT_EQ(alloc.free_pages_count(), 8);
    
    tracker.free_all_for_owner(42);
    
    EXPECT_EQ(alloc.free_pages_count(), 10);
    uint8_t dummy = 0;
    EXPECT_THROW(tracker.read(h1, 0, &dummy, 1), std::invalid_argument);
}

TEST(PageTrackerTest, ConcurrentFreeAndWrite) {
    SlabAllocator alloc(4096 * 10, 4096);
    PageTracker tracker(&alloc);
    
    meminfo::handle_t h1 = tracker.allocate(8000);
    std::vector<uint8_t> write_data(5000, 0xBB);
    
    // Thread 1: writes to the handle repeatedly
    std::thread t1([&]() {
        for (int i=0; i<100; ++i) {
            try {
                tracker.write(h1, 0, write_data.data(), write_data.size());
            } catch (const std::exception&) {
                // It is expected to throw after free
            }
        }
    });
    
    // Thread 2: frees the handle
    std::thread t2([&]() {
        tracker.free(h1);
    });
    
    t1.join();
    t2.join();
    
    // Should not crash and should leave everything clean
    EXPECT_EQ(alloc.free_pages_count(), 10);
}
