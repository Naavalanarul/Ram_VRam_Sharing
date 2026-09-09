#include <gtest/gtest.h>
#include <meminfo/memory/memory_daemon.h>
#include <meminfo/common/config.h>
#include <meminfo/common/protocol_version.h>
#include <memory_generated.h>
#include <thread>
#include <fstream>
#include <uv.h>
#include <vector>

using namespace meminfo;
using namespace meminfo::memory;

TEST(PeerDisconnectTest, SessionCleanupOnDisconnect) {
    auto write_config = [](const std::string& path) {
        std::ofstream out(path);
        out << "[memory]\n";
        out << "listen_address = \"127.0.0.1\"\n";
        out << "port = 9251\n";
        out << "total_reserved_bytes = 40960\n"; // 10 pages
        out << "page_size_bytes = 4096\n";
    };
    
    std::string cfg_path = "/tmp/meminfo_test_memoryd2.toml";
    write_config(cfg_path);
    Config config(cfg_path);
    
    std::unique_ptr<MemoryDaemon> daemon = std::make_unique<MemoryDaemon>(config);
    std::thread daemon_thread([&]() { daemon->run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // Connect, allocate 5 pages (20480 bytes), then abruptly disconnect
    {
        uv_loop_t loop;
        uv_loop_init(&loop);
        
        uv_tcp_t client;
        uv_tcp_init(&loop, &client);
        
        struct sockaddr_in dest;
        uv_ip4_addr("127.0.0.1", 9251, &dest);
        
        uv_connect_t connect_req;
        bool connected = false;
        uv_tcp_connect(&connect_req, &client, reinterpret_cast<const struct sockaddr*>(&dest), [](uv_connect_t* req, int status) {
            if (status == 0) *static_cast<bool*>(req->data) = true;
        });
        connect_req.data = &connected;
        
        while (!connected) uv_run(&loop, UV_RUN_ONCE);
        
        flatbuffers::FlatBufferBuilder builder;
        meminfo::memory::MemoryRequestBuilder mrb(builder);
        mrb.add_protocol_version(meminfo::MEMINFO_PROTOCOL_VERSION);
        mrb.add_request_id(1);
        mrb.add_op(meminfo::memory::OpCode_ALLOC);
        mrb.add_size(20480);
        builder.FinishSizePrefixed(mrb.Finish());
        
        uv_write_t write_req;
        uv_buf_t buf = uv_buf_init(reinterpret_cast<char*>(builder.GetBufferPointer()), builder.GetSize());
        bool written = false;
        write_req.data = &written;
        uv_write(&write_req, reinterpret_cast<uv_stream_t*>(&client), &buf, 1, [](uv_write_t* req, int) {
            *static_cast<bool*>(req->data) = true;
        });
        
        while (!written) uv_run(&loop, UV_RUN_ONCE);
        
        // Wait briefly for server to process it
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Disconnect immediately without freeing
        uv_close(reinterpret_cast<uv_handle_t*>(&client), nullptr);
        uv_run(&loop, UV_RUN_DEFAULT);
        uv_loop_close(&loop);
    }
    
    // Give daemon time to process the disconnect and free memory
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // Now verify we can still allocate the full 10 pages in a new connection
    {
        uv_loop_t loop;
        uv_loop_init(&loop);
        
        uv_tcp_t client;
        uv_tcp_init(&loop, &client);
        
        struct sockaddr_in dest;
        uv_ip4_addr("127.0.0.1", 9251, &dest);
        
        uv_connect_t connect_req;
        bool connected = false;
        uv_tcp_connect(&connect_req, &client, reinterpret_cast<const struct sockaddr*>(&dest), [](uv_connect_t* req, int status) {
            if (status == 0) *static_cast<bool*>(req->data) = true;
        });
        connect_req.data = &connected;
        
        while (!connected) uv_run(&loop, UV_RUN_ONCE);
        
        flatbuffers::FlatBufferBuilder builder;
        meminfo::memory::MemoryRequestBuilder mrb(builder);
        mrb.add_protocol_version(meminfo::MEMINFO_PROTOCOL_VERSION);
        mrb.add_request_id(2);
        mrb.add_op(meminfo::memory::OpCode_ALLOC);
        mrb.add_size(40960); // request full 10 pages
        builder.FinishSizePrefixed(mrb.Finish());
        
        uv_write_t write_req;
        uv_buf_t buf = uv_buf_init(reinterpret_cast<char*>(builder.GetBufferPointer()), builder.GetSize());
        bool written = false;
        write_req.data = &written;
        uv_write(&write_req, reinterpret_cast<uv_stream_t*>(&client), &buf, 1, [](uv_write_t* req, int) {
            *static_cast<bool*>(req->data) = true;
        });
        
        while (!written) uv_run(&loop, UV_RUN_ONCE);
        
        std::vector<uint8_t> resp_data;
        client.data = &resp_data;
        uv_read_start(reinterpret_cast<uv_stream_t*>(&client),
            [](uv_handle_t*, size_t suggested, uv_buf_t* b) {
                b->base = new char[suggested];
                b->len = suggested;
            },
            [](uv_stream_t* stream, ssize_t nread, const uv_buf_t* b) {
                auto* out = static_cast<std::vector<uint8_t>*>(stream->data);
                if (nread > 0) out->insert(out->end(), b->base, b->base + nread);
                else if (nread < 0) uv_close(reinterpret_cast<uv_handle_t*>(stream), nullptr);
                delete[] b->base;
            });
            
        while (resp_data.size() < 4) uv_run(&loop, UV_RUN_ONCE);
        
        while (true) {
            uint32_t msg_size = flatbuffers::GetPrefixedSize(resp_data.data());
            if (resp_data.size() >= msg_size + 4) break;
            uv_run(&loop, UV_RUN_ONCE);
        }
        
        const auto* resp = flatbuffers::GetSizePrefixedRoot<meminfo::memory::MemoryResponse>(resp_data.data());
        EXPECT_EQ(resp->request_id(), 2);
        EXPECT_EQ(resp->status(), meminfo::memory::StatusCode_OK);
        
        uv_read_stop(reinterpret_cast<uv_stream_t*>(&client));
        
        if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&client))) {
            uv_close(reinterpret_cast<uv_handle_t*>(&client), nullptr);
        }
        uv_run(&loop, UV_RUN_DEFAULT);
        uv_loop_close(&loop);
    }
    
    daemon->stop();
    daemon_thread.join();
}
