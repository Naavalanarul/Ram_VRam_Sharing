#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include "PagingWireProtocol.hpp"

#pragma comment(lib, "ws2_32.lib")

static constexpr size_t POOL_SIZE = 8ull * 1024 * 1024 * 1024; // 8 GiB
static constexpr uint16_t PORT = 9999;

struct ClientContext {
    SOCKET sock = INVALID_SOCKET;
    std::thread thread;
    bool active = false;
};

std::vector<ClientContext> g_clients;
std::mutex g_clients_mutex;
std::atomic<bool> g_running{true};
void* g_pool = nullptr;

void handle_client(ClientContext& ctx) {
    char buffer[CLUSTER_SIZE];
    PagingHeader hdr;

    while (g_running.load() && ctx.active) {
        if (!recv_exact(ctx.sock, &hdr, sizeof(hdr))) {
            break;
        }
        if (hdr.magic != PAGING_MAGIC) {
            break;
        }

        uint64_t cluster_addr = hdr.address;
        uint32_t page_count = hdr.page_count;
        uint32_t payload_len = hdr.payload_len;

        if (page_count == 0 || page_count > PAGES_PER_CLUSTER) {
            break;
        }
        if (payload_len != page_count * PAGE_SIZE) {
            break;
        }

        uint8_t* pool_base = static_cast<uint8_t*>(g_pool);
        uint8_t* cluster_ptr = pool_base + (cluster_addr % POOL_SIZE);

        if (hdr.opcode == PAGING_OP_READ_CLUSTER) {
            if (!send_exact(ctx.sock, cluster_ptr, payload_len)) {
                break;
            }
        } else if (hdr.opcode == PAGING_OP_WRITE_DIFF) {
            if (!recv_exact(ctx.sock, buffer, payload_len)) {
                break;
            }
            std::memcpy(cluster_ptr, buffer, payload_len);
        } else {
            break;
        }
    }

    closesocket(ctx.sock);
    ctx.sock = INVALID_SOCKET;
    ctx.active = false;
}

int main() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }

    g_pool = VirtualAlloc(nullptr, POOL_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_pool) {
        std::cerr << "Failed to allocate " << POOL_SIZE << " bytes\n";
        WSACleanup();
        return 1;
    }
    std::cout << "Allocated " << POOL_SIZE / (1024*1024*1024) << " GiB RAM pool at " << g_pool << "\n";

    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) {
        std::cerr << "socket() failed\n";
        VirtualFree(g_pool, 0, MEM_RELEASE);
        WSACleanup();
        return 1;
    }

    int optval = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&optval), sizeof(optval));
    setsockopt(listen_sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&optval), sizeof(optval));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(listen_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "bind() failed\n";
        closesocket(listen_sock);
        VirtualFree(g_pool, 0, MEM_RELEASE);
        WSACleanup();
        return 1;
    }

    if (listen(listen_sock, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "listen() failed\n";
        closesocket(listen_sock);
        VirtualFree(g_pool, 0, MEM_RELEASE);
        WSACleanup();
        return 1;
    }

    std::cout << "Paging server listening on port " << PORT << "\n";

    while (g_running.load()) {
        sockaddr_in client_addr;
        int client_len = sizeof(client_addr);
        SOCKET client_sock = accept(listen_sock, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_sock == INVALID_SOCKET) {
            if (g_running.load()) {
                std::cerr << "accept() failed\n";
            }
            continue;
        }

        int opt = 1;
        setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&opt), sizeof(opt));

        {
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            g_clients.emplace_back();
            ClientContext& ctx = g_clients.back();
            ctx.sock = client_sock;
            ctx.active = true;
            ctx.thread = std::thread(handle_client, std::ref(ctx));
        }

        std::cout << "Client connected\n";
    }

    {
        std::lock_guard<std::mutex> lock(g_clients_mutex);
        for (auto& ctx : g_clients) {
            if (ctx.active) {
                ctx.active = false;
                if (ctx.sock != INVALID_SOCKET) {
                    shutdown(ctx.sock, SD_BOTH);
                }
            }
        }
        for (auto& ctx : g_clients) {
            if (ctx.thread.joinable()) {
                ctx.thread.join();
            }
        }
    }

    closesocket(listen_sock);
    VirtualFree(g_pool, 0, MEM_RELEASE);
    WSACleanup();
    return 0;
}