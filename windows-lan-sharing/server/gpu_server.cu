#include <winsock2.h>
#include <ws2tcpip.h>
#include <cuda_runtime.h>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include "GpuWireProtocol.hpp"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "cudart.lib")

static constexpr uint16_t PORT = 9998;

struct ClientContext {
    SOCKET sock = INVALID_SOCKET;
    std::thread thread;
    bool active = false;
};

std::vector<ClientContext> g_clients;
std::mutex g_clients_mutex;
std::atomic<bool> g_running{true};

int check_cuda_error(cudaError_t err, const char* file, int line) {
    if (err != cudaSuccess) {
        std::cerr << "CUDA error at " << file << ":" << line << " - " << cudaGetErrorString(err) << "\n";
        return static_cast<int>(err);
    }
    return 0;
}

#define CUDA_CHECK(call) check_cuda_error(call, __FILE__, __LINE__)

void handle_client(ClientContext& ctx) {
    GpuHeader hdr;
    char buffer[8192];

    while (g_running.load() && ctx.active) {
        if (!recv_exact(ctx.sock, &hdr, sizeof(hdr))) {
            break;
        }
        if (hdr.magic != GPU_MAGIC) {
            break;
        }

        uint32_t payload_len = hdr.payload_len;
        if (payload_len > sizeof(buffer)) {
            break;
        }
        if (payload_len > 0 && !recv_exact(ctx.sock, buffer, payload_len)) {
            break;
        }

        GpuHeader resp_hdr;
        resp_hdr.magic = GPU_MAGIC;
        resp_hdr.sequence_id = hdr.sequence_id;
        resp_hdr.reserved = 0;

        int error_code = 0;
        uint32_t resp_payload_len = 0;

        switch (hdr.opcode) {
            case GPU_CMD_MALLOC: {
                if (payload_len < sizeof(GpuMallocReq)) { error_code = 1; break; }
                GpuMallocReq* req = reinterpret_cast<GpuMallocReq*>(buffer);
                void* dev_ptr = nullptr;
                error_code = CUDA_CHECK(cudaMalloc(&dev_ptr, req->size));
                if (error_code == 0) {
                    GpuMallocResp* resp = reinterpret_cast<GpuMallocResp*>(buffer);
                    resp->device_ptr = reinterpret_cast<uint64_t>(dev_ptr);
                    resp->error_code = 0;
                    resp->reserved = 0;
                    resp_payload_len = sizeof(GpuMallocResp);
                    resp_hdr.opcode = GPU_RESP_MALLOC_RESULT;
                } else {
                    resp_hdr.opcode = GPU_RESP_ERROR;
                }
                break;
            }
            case GPU_CMD_MEMCPY_H2D: {
                if (payload_len < sizeof(GpuMemcpyReq)) { error_code = 1; break; }
                GpuMemcpyReq* req = reinterpret_cast<GpuMemcpyReq*>(buffer);
                error_code = CUDA_CHECK(cudaMemcpy(
                    reinterpret_cast<void*>(req->dst_ptr),
                    reinterpret_cast<const void*>(req->src_ptr),
                    req->size,
                    cudaMemcpyHostToDevice
                ));
                if (error_code == 0) {
                    resp_hdr.opcode = GPU_RESP_SUCCESS;
                } else {
                    resp_hdr.opcode = GPU_RESP_ERROR;
                }
                break;
            }
            case GPU_CMD_MEMCPY_D2H: {
                if (payload_len < sizeof(GpuMemcpyReq)) { error_code = 1; break; }
                GpuMemcpyReq* req = reinterpret_cast<GpuMemcpyReq*>(buffer);
                error_code = CUDA_CHECK(cudaMemcpy(
                    reinterpret_cast<void*>(req->dst_ptr),
                    reinterpret_cast<const void*>(req->src_ptr),
                    req->size,
                    cudaMemcpyDeviceToHost
                ));
                if (error_code == 0) {
                    resp_hdr.opcode = GPU_RESP_MEMCPY_D2H;
                    resp_payload_len = 0;
                } else {
                    resp_hdr.opcode = GPU_RESP_ERROR;
                }
                break;
            }
            case GPU_CMD_SYNCHRONIZE: {
                error_code = CUDA_CHECK(cudaDeviceSynchronize());
                resp_hdr.opcode = error_code == 0 ? GPU_RESP_SUCCESS : GPU_RESP_ERROR;
                break;
            }
            case GPU_CMD_FREE: {
                if (payload_len < sizeof(GpuFreeReq)) { error_code = 1; break; }
                GpuFreeReq* req = reinterpret_cast<GpuFreeReq*>(buffer);
                error_code = CUDA_CHECK(cudaFree(reinterpret_cast<void*>(req->device_ptr)));
                resp_hdr.opcode = error_code == 0 ? GPU_RESP_SUCCESS : GPU_RESP_ERROR;
                break;
            }
            default:
                resp_hdr.opcode = GPU_RESP_ERROR;
                break;
        }

        resp_hdr.flags = 0;
        resp_hdr.payload_len = resp_payload_len;

        if (!send_exact(ctx.sock, &resp_hdr, sizeof(resp_hdr))) break;
        if (resp_payload_len > 0 && !send_exact(ctx.sock, buffer, resp_payload_len)) break;
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

    int device_count = 0;
    cudaGetDeviceCount(&device_count);
    if (device_count == 0) {
        std::cerr << "No CUDA devices found\n";
        WSACleanup();
        return 1;
    }
    cudaSetDevice(0);
    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, 0);
    std::cout << "Using GPU: " << prop.name << "\n";

    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) {
        std::cerr << "socket() failed\n";
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
        WSACleanup();
        return 1;
    }

    if (listen(listen_sock, SOMAXCONN) == SOCKET_ERROR) {
        std::cerr << "listen() failed\n";
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }

    std::cout << "GPU server listening on port " << PORT << "\n";

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
    WSACleanup();
    return 0;
}