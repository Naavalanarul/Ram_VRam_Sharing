#pragma once

#include <cstdint>
#include <cstring>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma pack(push, 1)

struct PagingHeader {
    uint32_t magic;       // 0x52414D31 ("RAM1")
    uint8_t  opcode;      // 1 = READ_CLUSTER, 2 = WRITE_DIFF
    uint8_t  reserved[3];
    uint64_t address;     // Virtual address (cluster-aligned)
    uint32_t page_count;  // Number of 4 KiB pages in cluster (typically 16 for 64 KiB)
    uint32_t payload_len; // Payload length in bytes
};

#pragma pack(pop)

static constexpr uint32_t PAGING_MAGIC = 0x52414D31;
static constexpr uint8_t  PAGING_OP_READ_CLUSTER  = 1;
static constexpr uint8_t  PAGING_OP_WRITE_DIFF    = 2;
static constexpr size_t   CLUSTER_SIZE            = 64 * 1024; // 64 KiB
static constexpr size_t   PAGE_SIZE               = 4096;
static constexpr uint32_t PAGES_PER_CLUSTER       = CLUSTER_SIZE / PAGE_SIZE;

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

inline bool send_paging_header(SOCKET sock, uint8_t opcode, uint64_t address, uint32_t page_count, uint32_t payload_len) {
    PagingHeader hdr;
    hdr.magic = PAGING_MAGIC;
    hdr.opcode = opcode;
    hdr.reserved[0] = hdr.reserved[1] = hdr.reserved[2] = 0;
    hdr.address = address;
    hdr.page_count = page_count;
    hdr.payload_len = payload_len;
    return send_exact(sock, &hdr, sizeof(hdr));
}

inline bool recv_paging_header(SOCKET sock, PagingHeader& out_hdr) {
    if (!recv_exact(sock, &out_hdr, sizeof(out_hdr))) {
        return false;
    }
    if (out_hdr.magic != PAGING_MAGIC) {
        return false;
    }
    return true;
}

inline uint64_t align_down_cluster(uint64_t addr) {
    return addr & ~(CLUSTER_SIZE - 1);
}

inline uint64_t align_up_cluster(uint64_t addr) {
    return (addr + CLUSTER_SIZE - 1) & ~(CLUSTER_SIZE - 1);
}