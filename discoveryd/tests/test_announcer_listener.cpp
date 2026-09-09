#include <gtest/gtest.h>
#include <meminfo/discovery/announcer.h>
#include <meminfo/discovery/listener.h>
#include <meminfo/discovery/peer_table.h>

using namespace meminfo;
using namespace meminfo::discovery;

TEST(AnnouncerListenerTest, RoundTrip) {
    uv_loop_t loop;
    uv_loop_init(&loop);
    
    node_id_t test_id{};
    test_id[0] = 0xAA;
    test_id[15] = 0xBB;
    
    PeerTable table;
    
    Listener listener(&loop, &table, "239.255.73.77", 9101, "127.0.0.1");
    listener.start();
    
    Announcer announcer(&loop, test_id, "test_host", "127.0.0.1", 
                        9200, 9300, "239.255.73.77", 9101, 100);
    announcer.update_metrics(1024, 2048);
    announcer.start();
    
    // Setup a timer to stop the loop after 250ms
    uv_timer_t stop_timer;
    uv_timer_init(&loop, &stop_timer);
    uv_timer_start(&stop_timer, [](uv_timer_t* handle) {
        uv_stop(handle->loop);
    }, 250, 0);
    
    uv_run(&loop, UV_RUN_DEFAULT);
    
    announcer.stop();
    listener.stop();
    uv_close(reinterpret_cast<uv_handle_t*>(&stop_timer), nullptr);
    
    // Drain closing handles
    uv_run(&loop, UV_RUN_DEFAULT);
    uv_loop_close(&loop);
    
    EXPECT_GE(table.active_count(), 1);
    auto peer = table.get_peer(test_id);
    EXPECT_EQ(peer.hostname, "test_host");
    EXPECT_EQ(peer.address, "127.0.0.1");
    EXPECT_EQ(peer.free_ram_bytes, 1024);
    EXPECT_EQ(peer.free_vram_bytes, 2048);
    EXPECT_EQ(peer.memory_port, 9200);
    EXPECT_EQ(peer.gpu_port, 9300);
}
