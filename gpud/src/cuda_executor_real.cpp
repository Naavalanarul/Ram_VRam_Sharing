#include <meminfo/gpu/cuda_executor.h>

#ifdef MEMINFO_ENABLE_CUDA
#include <cuda_runtime.h>

namespace meminfo {
namespace gpu {

class CudaExecutorReal : public ICudaExecutor {
public:
    int allocate(uint64_t* devPtr, size_t size) override {
        void* ptr = nullptr;
        cudaError_t err = cudaMalloc(&ptr, size);
        if (err == cudaSuccess) {
            *devPtr = reinterpret_cast<uint64_t>(ptr);
        }
        return static_cast<int>(err);
    }

    int free(uint64_t devPtr) override {
        return static_cast<int>(cudaFree(reinterpret_cast<void*>(devPtr)));
    }

    int memcpyHtoD(uint64_t dst, const void* src, size_t size) override {
        return static_cast<int>(cudaMemcpy(reinterpret_cast<void*>(dst), src, size, cudaMemcpyHostToDevice));
    }

    int memcpyDtoH(void* dst, uint64_t src, size_t size) override {
        return static_cast<int>(cudaMemcpy(dst, reinterpret_cast<const void*>(src), size, cudaMemcpyDeviceToHost));
    }

    int getDeviceProperties(std::string& name, size_t& totalGlobalMem) override {
        cudaDeviceProp prop;
        cudaError_t err = cudaGetDeviceProperties(&prop, 0); // Assuming device 0 for now
        if (err == cudaSuccess) {
            name = prop.name;
            totalGlobalMem = prop.totalGlobalMem;
        }
        return static_cast<int>(err);
    }
};

std::unique_ptr<ICudaExecutor> create_real_cuda_executor() {
    return std::make_unique<CudaExecutorReal>();
}

} // namespace gpu
} // namespace meminfo

#else // MEMINFO_ENABLE_CUDA

namespace meminfo {
namespace gpu {

std::unique_ptr<ICudaExecutor> create_real_cuda_executor() {
    // If compiled without CUDA, fallback to stub or throw
    return nullptr;
}

} // namespace gpu
} // namespace meminfo

#endif // MEMINFO_ENABLE_CUDA
