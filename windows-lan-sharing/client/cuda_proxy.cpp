#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <shared_mutex>
#include <cstdlib>
#include <iostream>
#include "GpuWireProtocol.hpp"
#include "DevicePointerTracker.hpp"

#pragma comment(lib, "ws2_32.lib")

static SOCKET g_gpu_socket = INVALID_SOCKET;
static std::mutex g_socket_mutex;
static std::atomic<uint32_t> g_sequence_id{1};
static std::atomic<bool> g_initialized{false};
static char g_server_ip[64] = "127.0.0.1";

static DevicePointerTracker g_tracker;

static bool send_gpu_cmd(uint8_t opcode, uint8_t flags, const void* payload, uint32_t payload_len, void* resp_buf, uint32_t resp_len) {
    std::lock_guard<std::mutex> lock(g_socket_mutex);
    if (g_gpu_socket == INVALID_SOCKET) return false;

    uint32_t seq = g_sequence_id.fetch_add(1);
    if (!send_gpu_header(g_gpu_socket, opcode, flags, seq, payload_len)) return false;
    if (payload_len > 0 && !send_exact(g_gpu_socket, payload, payload_len)) return false;

    GpuHeader resp_hdr;
    if (!recv_gpu_header(g_gpu_socket, resp_hdr)) return false;
    if (resp_hdr.opcode == GPU_RESP_ERROR) return false;
    if (resp_hdr.payload_len > resp_len) return false;
    if (resp_hdr.payload_len > 0 && !recv_exact(g_gpu_socket, resp_buf, resp_hdr.payload_len)) return false;

    return true;
}

static bool init_winsock() {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

static SOCKET connect_gpu_server(const char* ip, uint16_t port) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return INVALID_SOCKET;

    int optval = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&optval), sizeof(optval));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        closesocket(sock);
        return INVALID_SOCKET;
    }

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(sock);
        return INVALID_SOCKET;
    }
    return sock;
}

extern "C" __declspec(dllexport) bool InitializeProxy() {
    if (g_initialized.exchange(true)) return true;

    if (!init_winsock()) return false;

    const char* env_ip = std::getenv("REMOTE_GPU_IP");
    if (env_ip) {
        strncpy_s(g_server_ip, env_ip, sizeof(g_server_ip) - 1);
    }

    g_gpu_socket = connect_gpu_server(g_server_ip, 9998);
    return g_gpu_socket != INVALID_SOCKET;
}

extern "C" __declspec(dllexport) void ShutdownProxy() {
    if (!g_initialized.exchange(false)) return;

    std::lock_guard<std::mutex> lock(g_socket_mutex);
    if (g_gpu_socket != INVALID_SOCKET) {
        closesocket(g_gpu_socket);
        g_gpu_socket = INVALID_SOCKET;
    }
    WSACleanup();
}

extern "C" __declspec(dllexport) int cudaMalloc(void** devPtr, size_t size) {
    if (!g_initialized.load() && !InitializeProxy()) return 1;

    GpuMallocReq req;
    req.size = size;
    req.alignment = 256;
    req.reserved = 0;

    GpuMallocResp resp;
    if (!send_gpu_cmd(GPU_CMD_MALLOC, GPU_FLAG_NONE, &req, sizeof(req), &resp, sizeof(resp))) {
        return 1;
    }
    if (resp.error_code != 0) return resp.error_code;

    *devPtr = g_tracker.allocate(resp.device_ptr, size);
    return 0;
}

extern "C" __declspec(dllexport) int cudaFree(void* devPtr) {
    if (!g_initialized.load()) return 1;

    uint64_t remote_ptr;
    if (!g_tracker.get_remote(devPtr, remote_ptr)) return 1;

    GpuFreeReq req;
    req.device_ptr = remote_ptr;

    char resp_buf[16];
    if (!send_gpu_cmd(GPU_CMD_FREE, GPU_FLAG_NONE, &req, sizeof(req), resp_buf, sizeof(resp_buf))) {
        return 1;
    }

    g_tracker.free(devPtr);
    return 0;
}

extern "C" __declspec(dllexport) int cudaMemcpy(void* dst, const void* src, size_t count, int kind) {
    if (!g_initialized.load() && !InitializeProxy()) return 1;

    bool is_h2d = (kind == 1);
    bool is_d2h = (kind == 2);

    uint64_t remote_dst = 0, remote_src = 0;

    if (is_h2d) {
        if (!g_tracker.get_remote(dst, remote_dst)) return 1;
        remote_src = reinterpret_cast<uintptr_t>(src);
    } else if (is_d2h) {
        if (!g_tracker.get_remote(src, remote_src)) return 1;
        remote_dst = reinterpret_cast<uintptr_t>(dst);
    } else {
        return 1;
    }

    GpuMemcpyReq req;
    req.dst_ptr = is_h2d ? remote_dst : remote_src;
    req.src_ptr = is_h2d ? remote_src : remote_dst;
    req.size = count;
    req.kind = is_h2d ? 0 : 1;
    req.reserved = 0;

    uint8_t flags = is_h2d ? GPU_FLAG_ASYNC : GPU_FLAG_NONE;
    char resp_buf[16];
    if (!send_gpu_cmd(is_h2d ? GPU_CMD_MEMCPY_H2D : GPU_CMD_MEMCPY_D2H, flags, &req, sizeof(req), resp_buf, sizeof(resp_buf))) {
        return 1;
    }

    if (!is_h2d) {
        if (!recv_exact(g_gpu_socket, dst, count)) return 1;
    }

    return 0;
}

extern "C" __declspec(dllexport) int cudaDeviceSynchronize() {
    if (!g_initialized.load()) return 1;

    GpuSyncReq req;
    req.stream = 0;
    char resp_buf[16];
    return send_gpu_cmd(GPU_CMD_SYNCHRONIZE, GPU_FLAG_NONE, &req, sizeof(req), resp_buf, sizeof(resp_buf)) ? 0 : 1;
}

extern "C" __declspec(dllexport) int cudaGetLastError() {
    return 0;
}

extern "C" __declspec(dllexport) const char* cudaGetErrorString(int error) {
    static const char* unknown = "unknown error";
    return unknown;
}

extern "C" __declspec(dllexport) int cudaMallocHost(void** ptr, size_t size) {
    *ptr = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return *ptr ? 0 : 1;
}

extern "C" __declspec(dllexport) int cudaFreeHost(void* ptr) {
    return VirtualFree(ptr, 0, MEM_RELEASE) ? 0 : 1;
}

extern "C" __declspec(dllexport) int cudaGetDeviceCount(int* count) {
    *count = 1;
    return 0;
}

extern "C" __declspec(dllexport) int cudaSetDevice(int device) {
    return 0;
}

extern "C" __declspec(dllexport) int cudaGetDevice(int* device) {
    *device = 0;
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            break;
        case DLL_PROCESS_DETACH:
            ShutdownProxy();
            break;
    }
    return TRUE;
}