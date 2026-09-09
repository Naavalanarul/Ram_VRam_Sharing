#pragma once
#include <cstdint>
#include <string>
#include <cstddef>
#include <memory>

namespace meminfo {
namespace gpu {

// Abstract interface for executing CUDA operations.
// Allows us to swap between a real CUDA implementation and a stub/mock for testing.
class ICudaExecutor {
public:
    virtual ~ICudaExecutor() = default;

    // Allocate device memory. Returns CUDA error code (0 = success).
    virtual int allocate(uint64_t* devPtr, size_t size) = 0;

    // Free device memory. Returns CUDA error code.
    virtual int free(uint64_t devPtr) = 0;

    // Copy from host memory to device memory.
    virtual int memcpyHtoD(uint64_t dst, const void* src, size_t size) = 0;

    // Copy from device memory to host memory.
    virtual int memcpyDtoH(void* dst, uint64_t src, size_t size) = 0;

    // Get basic device properties (e.g. name and total global memory).
    virtual int getDeviceProperties(std::string& name, size_t& totalGlobalMem) = 0;
};

// Factory functions to create the appropriate executor
std::unique_ptr<ICudaExecutor> create_real_cuda_executor();
std::unique_ptr<ICudaExecutor> create_stub_cuda_executor();

} // namespace gpu
} // namespace meminfo
