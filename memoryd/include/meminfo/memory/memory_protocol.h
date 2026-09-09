#pragma once
#include <vector>
#include <cstdint>
#include <meminfo/memory/page_tracker.h>

namespace meminfo {
namespace memory {

class MemoryProtocol {
public:
    // Processes a request and returns the serialized response buffer
    static std::vector<uint8_t> process_request(PageTracker* page_tracker, uint64_t session_id, const uint8_t* data, size_t size);
};

} // namespace memory
} // namespace meminfo