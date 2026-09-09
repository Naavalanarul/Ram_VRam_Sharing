#include <gtest/gtest.h>
#include <meminfo/discovery/control_socket.h>
#include <meminfo/discovery/peer_table.h>
#include <meminfo/platform/ILocalIpc.h>
#include <control_generated.h>
#include <thread>
#include <chrono>

using namespace meminfo;
using namespace meminfo::discovery;

TEST(ControlSocketTest, ListPeers) {
    std::string sock_path = "test_discovery_control.sock";
    
    PeerTable table;
    PeerInfo p1;
    p1.id[0] = 0xAA;
    p1.hostname = "peer1";
    p1.free_ram_bytes = 1000;
    table.update(p1);
    
    ControlSocket control(&table, sock_path);
    control.start();
    
    // Give it a moment to bind
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Connect client using ILocalIpc
    auto ipc_client = platform::create_local_ipc();
    
    flatbuffers::FlatBufferBuilder builder;
    meminfo::control::ControlRequestBuilder crb(builder);
    crb.add_command(meminfo::control::ControlCommand_LIST_PEERS);
    crb.add_request_id(1234);
    builder.FinishSizePrefixed(crb.Finish());
    
    std::vector<uint8_t> req_data(builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize());
    
    auto response_data = ipc_client->send_request(sock_path, req_data);
    
    ASSERT_GT(response_data.size(), 4);
    
    flatbuffers::Verifier v(response_data.data(), response_data.size());
    ASSERT_TRUE(v.VerifySizePrefixedBuffer<meminfo::control::ControlResponse>(nullptr));
    
    const auto* resp = flatbuffers::GetSizePrefixedRoot<meminfo::control::ControlResponse>(response_data.data());
    EXPECT_EQ(resp->request_id(), 1234);
    EXPECT_TRUE(resp->success());
    ASSERT_NE(resp->peers(), nullptr);
    EXPECT_EQ(resp->peers()->size(), 1);
    EXPECT_EQ(resp->peers()->Get(0)->hostname()->str(), "peer1");
    EXPECT_EQ(resp->peers()->Get(0)->free_ram_bytes(), 1000);
    
    control.stop();
}
