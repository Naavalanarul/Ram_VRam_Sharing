#include <gtest/gtest.h>
#include <meminfo/memory/slab_allocator.h>
#include <limits>

using namespace meminfo::memory;

TEST(SlabAllocatorTest, InitializeAndAllocate) {
    // 10 pages of 4KB each = 40KB
    SlabAllocator alloc(4096 * 10, 4096);
    
    EXPECT_EQ(alloc.total_pages(), 10);
    EXPECT_EQ(alloc.free_pages_count(), 10);
    
    auto pages = alloc.allocate_pages(3);
    EXPECT_EQ(pages.size(), 3);
    EXPECT_EQ(alloc.free_pages_count(), 7);
    
    alloc.free_pages(pages);
    EXPECT_EQ(alloc.free_pages_count(), 10);
}

TEST(SlabAllocatorTest, WriteAndRead) {
    SlabAllocator alloc(4096 * 2, 4096); // 2 pages
    auto pages = alloc.allocate_pages(1);
    
    uint8_t write_data[4] = {0xAA, 0xBB, 0xCC, 0xDD};
    alloc.write_page(pages[0], 10, write_data, 4);
    
    uint8_t read_data[4] = {0};
    alloc.read_page(pages[0], 10, read_data, 4);
    
    EXPECT_EQ(read_data[0], 0xAA);
    EXPECT_EQ(read_data[1], 0xBB);
    EXPECT_EQ(read_data[2], 0xCC);
    EXPECT_EQ(read_data[3], 0xDD);
}

TEST(SlabAllocatorTest, OutOfMemoryThrows) {
    SlabAllocator alloc(4096 * 2, 4096); // 2 pages
    EXPECT_NO_THROW(alloc.allocate_pages(2));
    EXPECT_THROW(alloc.allocate_pages(1), std::bad_alloc);
}

// A page_offset large enough to wrap page_offset + size used to slip past the
// bounds check and memcpy outside the slab.
TEST(SlabAllocatorTest, OffsetOverflowIsRejected) {
    SlabAllocator alloc(4096 * 2, 4096);
    auto pages = alloc.allocate_pages(1);

    constexpr size_t kMax = std::numeric_limits<size_t>::max();
    uint8_t buf[8] = {0};

    // kMax - 3 + 8 wraps to 4, which is <= page_size.
    EXPECT_THROW(alloc.write_page(pages[0], kMax - 3, buf, 8), std::out_of_range);
    EXPECT_THROW(alloc.read_page(pages[0], kMax - 3, buf, 8), std::out_of_range);

    // Ordinary out-of-range offsets must still be rejected.
    EXPECT_THROW(alloc.write_page(pages[0], 4090, buf, 8), std::out_of_range);
    EXPECT_THROW(alloc.read_page(pages[0], 4096, buf, 1), std::out_of_range);

    // ...and a legitimate write at the very end of the page must still work.
    EXPECT_NO_THROW(alloc.write_page(pages[0], 4088, buf, 8));
}
