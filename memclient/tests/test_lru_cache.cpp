#include <gtest/gtest.h>
#include <meminfo/client/lru_cache.h>

using namespace meminfo::client;

TEST(LRUCacheTest, BasicPutAndGet) {
    LRUCache cache(1024);
    
    std::vector<uint8_t> data1 = {1, 2, 3};
    EXPECT_TRUE(cache.put(1, data1));
    
    auto retrieved = cache.get(1);
    ASSERT_TRUE(retrieved.has_value());
    EXPECT_EQ(retrieved.value(), data1);
    
    EXPECT_EQ(cache.current_size(), 3);
    EXPECT_EQ(cache.count(), 1);
}

TEST(LRUCacheTest, EvictionLogic) {
    LRUCache cache(10); // 10 bytes max
    
    EXPECT_TRUE(cache.put(1, std::vector<uint8_t>(4, 'a'))); // 4 bytes
    EXPECT_TRUE(cache.put(2, std::vector<uint8_t>(4, 'b'))); // 8 bytes
    
    EXPECT_FALSE(cache.needs_eviction(2)); // exactly 10
    EXPECT_TRUE(cache.needs_eviction(3));  // 11 > 10
    
    // Evict one should drop handle 1 (least recently used)
    auto evicted = cache.evict_one();
    ASSERT_TRUE(evicted.has_value());
    EXPECT_EQ(evicted->handle, 1);
    
    EXPECT_EQ(cache.current_size(), 4);
    EXPECT_EQ(cache.count(), 1);
    
    // get(2) moves it to MRU (though it's the only item anyway)
    cache.get(2);
}

TEST(LRUCacheTest, UpdateExisting) {
    LRUCache cache(1024);
    
    EXPECT_TRUE(cache.put(1, std::vector<uint8_t>(5, 'a')));
    EXPECT_EQ(cache.current_size(), 5);
    
    // Update with larger payload
    EXPECT_TRUE(cache.put(1, std::vector<uint8_t>(10, 'b')));
    EXPECT_EQ(cache.current_size(), 10);
    EXPECT_EQ(cache.count(), 1);
    
    auto retrieved = cache.get(1);
    EXPECT_EQ(retrieved.value().size(), 10);
}

TEST(LRUCacheTest, PutTooLarge) {
    LRUCache cache(10);
    EXPECT_FALSE(cache.put(1, std::vector<uint8_t>(11, 'a')));
    EXPECT_EQ(cache.count(), 0);
}
