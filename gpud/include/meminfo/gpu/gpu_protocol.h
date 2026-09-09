#pragma once
#include <vector>
#include <cstdint>
#include <meminfo/gpu/cuda_executor.h>

namespace meminfo {
namespace gpu {
class GpuProtocol {
public:
    // Processes a request and returns the serialized response buffer
    static std::vector<uint8_t> process_request(ICudaExecutor* executor, const uint8_t* data, size_t size);
};
}
}
