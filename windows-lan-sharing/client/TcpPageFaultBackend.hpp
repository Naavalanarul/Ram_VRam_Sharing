#pragma once

#include "IPageFaultBackend.hpp"
#include "PagingWireProtocol.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mutex>
#include <atomic>

class TcpPageFaultBackend final : public IPageFaultBackend {
public:
    TcpPageFaultBackend();
    ~TcpPageFaultBackend() override;

    TcpPageFaultBackend(const TcpPageFaultBackend&) = delete;
    TcpPageFaultBackend& operator=(const TcpPageFaultBackend&) = delete;
    TcpPageFaultBackend(TcpPageFaultBackend&&) = delete;
    TcpPageFaultBackend& operator=(TcpPageFaultBackend&&) = delete;

    bool initialize(const char* ip, uint16_t port) override;
    bool fetch_page_cluster(uint64_t addr, size_t size, void* dst) override;
    bool commit_page_diff(uint64_t addr, size_t size, const void* src) override;
    void shutdown() override;

private:
    SOCKET connect_socket(const char* ip, uint16_t port);
    void close_socket();

    SOCKET socket_ = INVALID_SOCKET;
    std::mutex socket_mutex_;
    std::atomic<bool> initialized_{false};
    uint16_t port_ = 0;
    char ip_[64] = {0};
};