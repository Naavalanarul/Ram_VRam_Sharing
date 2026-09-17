#include "TcpPageFaultBackend.hpp"
#include <iostream>

TcpPageFaultBackend::TcpPageFaultBackend() = default;

TcpPageFaultBackend::~TcpPageFaultBackend() {
    shutdown();
}

SOCKET TcpPageFaultBackend::connect_socket(const char* ip, uint16_t port) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }

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

void TcpPageFaultBackend::close_socket() {
    std::lock_guard<std::mutex> lock(socket_mutex_);
    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
}

bool TcpPageFaultBackend::initialize(const char* ip, uint16_t port) {
    if (initialized_.load()) {
        return true;
    }

    SOCKET sock = connect_socket(ip, port);
    if (sock == INVALID_SOCKET) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        socket_ = sock;
        port_ = port;
        strncpy_s(ip_, ip, sizeof(ip_) - 1);
    }

    initialized_.store(true);
    return true;
}

bool TcpPageFaultBackend::fetch_page_cluster(uint64_t addr, size_t size, void* dst) {
    if (!initialized_.load()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(socket_mutex_);
    if (socket_ == INVALID_SOCKET) {
        return false;
    }

    uint64_t cluster_addr = align_down_cluster(addr);
    uint32_t page_count = static_cast<uint32_t>((size + PAGE_SIZE - 1) / PAGE_SIZE);
    if (page_count == 0) page_count = 1;
    if (page_count > PAGES_PER_CLUSTER) page_count = PAGES_PER_CLUSTER;
    uint32_t payload_len = page_count * PAGE_SIZE;

    if (!send_paging_header(socket_, PAGING_OP_READ_CLUSTER, cluster_addr, page_count, payload_len)) {
        close_socket();
        initialized_.store(false);
        return false;
    }

    if (!recv_exact(socket_, dst, payload_len)) {
        close_socket();
        initialized_.store(false);
        return false;
    }

    return true;
}

bool TcpPageFaultBackend::commit_page_diff(uint64_t addr, size_t size, const void* src) {
    if (!initialized_.load()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(socket_mutex_);
    if (socket_ == INVALID_SOCKET) {
        return false;
    }

    uint64_t cluster_addr = align_down_cluster(addr);
    uint32_t page_count = static_cast<uint32_t>((size + PAGE_SIZE - 1) / PAGE_SIZE);
    if (page_count == 0) page_count = 1;
    if (page_count > PAGES_PER_CLUSTER) page_count = PAGES_PER_CLUSTER;
    uint32_t payload_len = page_count * PAGE_SIZE;

    if (!send_paging_header(socket_, PAGING_OP_WRITE_DIFF, cluster_addr, page_count, payload_len)) {
        close_socket();
        initialized_.store(false);
        return false;
    }

    if (!send_exact(socket_, src, payload_len)) {
        close_socket();
        initialized_.store(false);
        return false;
    }

    return true;
}

void TcpPageFaultBackend::shutdown() {
    close_socket();
    initialized_.store(false);
}