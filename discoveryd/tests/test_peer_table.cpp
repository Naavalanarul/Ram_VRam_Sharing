#include <gtest/gtest.h>
#include <meminfo/discovery/peer_table.h>
#include <thread>

using namespace meminfo;
using namespace meminfo::discovery;

TEST(PeerTableTest, UpdateAndGet) {
    PeerTable table;
    PeerInfo info;
    info.id[0] = 1;
    info.hostname = "node1";
    info.free_ram_bytes = 1024;
    
    table.update(info);
    
    EXPECT_EQ(table.active_count(), 1);
    
    auto peer = table.get_peer(info.id);
    EXPECT_EQ(peer.hostname, "node1");
    EXPECT_EQ(peer.free_ram_bytes, 1024);
    EXPECT_EQ(peer.state, PeerInfo::State::ACTIVE);
}

TEST(PeerTableTest, UpdateExisting) {
    PeerTable table;
    PeerInfo info;
    info.id[0] = 1;
    info.hostname = "node1";
    info.free_ram_bytes = 1024;
    
    table.update(info);
    
    info.free_ram_bytes = 2048;
    table.update(info);
    
    EXPECT_EQ(table.active_count(), 1);
    auto peer = table.get_peer(info.id);
    EXPECT_EQ(peer.free_ram_bytes, 2048);
}

TEST(PeerTableTest, RemovePeer) {
    PeerTable table;
    PeerInfo info;
    info.id[0] = 1;
    table.update(info);
    
    EXPECT_EQ(table.active_count(), 1);
    table.remove_peer(info.id);
    EXPECT_EQ(table.active_count(), 0);
    EXPECT_THROW(table.get_peer(info.id), std::out_of_range);
}

TEST(PeerTableTest, TickStaleAndEvict) {
    PeerTable table;
    table.set_ttl_seconds(1); // 1 second TTL
    
    PeerInfo info;
    info.id[0] = 1;
    table.update(info);
    
    EXPECT_EQ(table.active_count(), 1);
    
    // Sleep for 2 seconds to make it stale
    std::this_thread::sleep_for(std::chrono::seconds(2));
    table.tick();
    
    EXPECT_EQ(table.active_count(), 0); // Not active anymore
    auto peers = table.get_peers();
    ASSERT_EQ(peers.size(), 1);
    EXPECT_EQ(peers[0].state, PeerInfo::State::STALE);
    
    // Sleep for 2 more seconds to evict (total > 3 seconds)
    std::this_thread::sleep_for(std::chrono::seconds(2));
    table.tick();
    
    EXPECT_EQ(table.get_peers().size(), 0);
}
