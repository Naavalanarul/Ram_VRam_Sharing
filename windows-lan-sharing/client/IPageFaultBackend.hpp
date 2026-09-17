#pragma once

#include <cstdint>
#include <string>

class IPageFaultBackend {
public:
    virtual ~IPageFaultBackend() = default;

    virtual bool initialize(const char* ip, uint16_t port) = 0;
    virtual bool fetch_page_cluster(uint64_t addr, size_t size, void* dst) = 0;
    virtual bool commit_page_diff(uint64_t addr, size_t size, const void* src) = 0;
    virtual void shutdown() = 0;
};