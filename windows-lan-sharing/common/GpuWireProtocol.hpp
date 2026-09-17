#pragma once

#include <cstdint>
#include <cstring>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma pack(push, 1)

struct GpuHeader {
    uint32_t magic;         // 0x47505531 ("GPU1")
    uint8_t  opcode;        // Command opcode
    uint8_t  flags;         // Flags (async, etc.)
    uint16_t reserved;
    uint32_t sequence_id;   // Request/response correlation ID
    uint32_t payload_len;   // Payload length in bytes
};

#pragma pack(pop)

static constexpr uint32_t GPU_MAGIC = 0x47505531;

enum GpuOpcode : uint8_t {
    GPU_CMD_MALLOC          = 1,
    GPU_CMD_MEMCPY_H2D      = 2,
    GPU_CMD_MEMCPY_D2H      = 3,
    GPU_CMD_SYNCHRONIZE     = 4,
    GPU_CMD_FREE            = 5,
    GPU_RESP_SUCCESS        = 0x80,
    GPU_RESP_ERROR          = 0x81,
    GPU_RESP_MALLOC_RESULT  = 0x82,
    GPU_RESP_MEMCPY_D2H     = 0x83,
};

enum GpuFlags : uint8_t {
    GPU_FLAG_NONE       = 0,
    GPU_FLAG_ASYNC      = 0x01,
    GPU_FLAG_PINNED     = 0x02,
};

#pragma pack(push, 1)
struct GpuMallocReq {
    uint64_t size;
    uint32_t alignment;
    uint32_t reserved;
};

struct GpuMallocResp {
    uint64_t device_ptr;
    int32_t  error_code;
    uint32_t reserved;
};

struct GpuMemcpyReq {
    uint64_t dst_ptr;
    uint64_t src_ptr;
    uint64_t size;
    uint32_t kind; // 0 = H2D, 1 = D2H, 2 = D2D
    uint32_t reserved;
};

struct GpuSyncReq {
    uint64_t stream; // 0 = default stream
};

struct GpuFreeReq {
    uint64_t device_ptr;
};
#pragma pack(pop)

inline bool send_exact(SOCKET sock, const void* buf, size_t len) {
    const char* ptr = static_cast<const char*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        int sent = send(sock, ptr, static_cast<int>(remaining), 0);
        if (sent == SOCKET_ERROR) {
            return false;
        }
        ptr += sent;
        remaining -= sent;
    }
    return true;
}

inline bool recv_exact(SOCKET sock, void* buf, size_t len) {
    char* ptr = static_cast<char*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        int recvd = recv(sock, ptr, static_cast<int>(remaining), 0);
        if (recvd == SOCKET_ERROR) {
            return false;
        }
        if (recvd == 0) {
            return false;
        }
        ptr += recvd;
        remaining -= recvd;
    }
    return true;
}

inline bool send_gpu_header(SOCKET sock, uint8_t opcode, uint8_t flags, uint32_t sequence_id, uint32_t payload_len) {
    GpuHeader hdr;
    hdr.magic = GPU_MAGIC;
    hdr.opcode = opcode;
    hdr.flags = flags;
    hdr.reserved = 0;
    hdr.sequence_id = sequence_id;
    hdr.payload_len = payload_len;
    return send_exact(sock, &hdr, sizeof(hdr));
}

inline bool recv_gpu_header(SOCKET sock, GpuHeader& out_hdr) {
    if (!recv_exact(sock, &out_hdr, sizeof(out_hdr))) {
        return false;
    }
    if (out_hdr.magic != GPU_MAGIC) {
        return false;
    }
    return true;
}