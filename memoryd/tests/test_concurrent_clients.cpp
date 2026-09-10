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

TEST(ConcurrentClientsTest, MultipleClientsAllocWriteRead) {
    auto write_config = [](const std::string& path, int port) {
        std::ofstream out(path);
        out << "[memory]\n";
        out << "listen_address = \"127.0.0.1\"\n";
        out << "port = " << port << "\n";
        out << "total_reserved_bytes = 1048576\n"; // 1MB
        out << "page_size_bytes = 4096\n";
    };
    
    std::string cfg_path = "/tmp/meminfo_test_memoryd.toml";
    int test_port = 0; // Let OS assign
    write_config(cfg_path, test_port);
    Config config(cfg_path);
    
    std::unique_ptr<MemoryDaemon> daemon = std::make_unique<MemoryDaemon>(config);
    std::thread daemon_thread([&]() { daemon->run(); });
    
    // Give it a moment to bind
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // Get the actual port the daemon bound to
    int actual_port = daemon->get_listen_port();
    
    auto run_client = [&](int client_id) {
        uv_loop_t loop;
        uv_loop_init(&loop);
        
        uv_tcp_t client;
        uv_tcp_init(&loop, &client);
        
        struct sockaddr_in dest;
        uv_ip4_addr("127.0.0.1", actual_port, &dest);
        
        uv_connect_t connect_req;
        bool connected = false;
        uv_tcp_connect(&connect_req, &client, reinterpret_cast<const struct sockaddr*>(&dest), [](uv_connect_t* req, int status) {
            if (status == 0) *static_cast<bool*>(req->data) = true;
        });
        connect_req.data = &connected;
        
        while (!connected) uv_run(&loop, UV_RUN_ONCE);
        
        // --- 1. ALLOC ---
        flatbuffers::FlatBufferBuilder builder;
        meminfo::memory::MemoryRequestBuilder mrb(builder);
        mrb.add_protocol_version(meminfo::MEMINFO_PROTOCOL_VERSION);
        mrb.add_request_id(client_id * 100 + 1);
        mrb.add_op(meminfo::memory::OpCode_ALLOC);
        mrb.add_size(100);
        builder.FinishSizePrefixed(mrb.Finish());
        
        uv_write_t write_req;
        uv_buf_t buf = uv_buf_init(reinterpret_cast<char*>(builder.GetBufferPointer()), builder.GetSize());
        bool written = false;
        write_req.data = &written;
        uv_write(&write_req, reinterpret_cast<uv_stream_t*>(&client), &buf, 1, [](uv_write_t* req, int) {
            *static_cast<bool*>(req->data) = true;
        });
        
        while (!written) uv_run(&loop, UV_RUN_ONCE);
        
        // Read response
        std::vector<uint8_t> resp_data;
        client.data = &resp_data;
        uv_read_start(reinterpret_cast<uv_stream_t*>(&client),
            [](uv_handle_t*, size_t suggested, uv_buf_t* b) {
                b->base = new char[suggested];
                // uv_buf_t::len is size_t on Unix but a 32-bit ULONG on Windows, so this
                // assignment narrows there; make the conversion explicit.
                b->len = static_cast<decltype(b->len)>(suggested);
            },
            [](uv_stream_t* stream, ssize_t nread, const uv_buf_t* b) {
                auto* out = static_cast<std::vector<uint8_t>*>(stream->data);
                if (nread > 0) out->insert(out->end(), b->base, b->base + nread);
                else if (nread < 0) uv_close(reinterpret_cast<uv_handle_t*>(stream), nullptr);
                delete[] b->base;
            });
            
        while (resp_data.size() < 4) uv_run(&loop, UV_RUN_ONCE);
        
        // Keep running until we have the full size-prefixed message
        while (true) {
            uint32_t msg_size = flatbuffers::GetPrefixedSize(resp_data.data());
            if (resp_data.size() >= msg_size + 4) break;
            uv_run(&loop, UV_RUN_ONCE);
        }
        
        const auto* resp = flatbuffers::GetSizePrefixedRoot<meminfo::memory::MemoryResponse>(resp_data.data());
        EXPECT_EQ(resp->request_id(), client_id * 100 + 1);
        EXPECT_EQ(resp->status(), meminfo::memory::StatusCode_OK);
        EXPECT_GT(resp->handle(), 0);
        
        uv_read_stop(reinterpret_cast<uv_stream_t*>(&client));
        
        if (!uv_is_closing(reinterpret_cast<uv_handle_t*>(&client))) {
            uv_close(reinterpret_cast<uv_handle_t*>(&client), nullptr);
        }
        uv_run(&loop, UV_RUN_DEFAULT);
        uv_loop_close(&loop);
    };
    
    std::thread c1([&]() { run_client(1); });
    std::thread c2([&]() { run_client(2); });
    std::thread c3([&]() { run_client(3); });
    
    c1.join();
    c2.join();
    c3.join();
    
    daemon->stop();
    daemon_thread.join();
}
