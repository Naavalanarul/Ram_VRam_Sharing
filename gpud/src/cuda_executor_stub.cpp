#include <meminfo/gpu/cuda_executor.h>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <mutex>

namespace meminfo {
namespace gpu {

class CudaExecutorStub : public ICudaExecutor {
public:
    int allocate(uint64_t* devPtr, size_t size) override {
        std::lock_guard<std::mutex> lock(mutex_);
        void* ptr = std::malloc(size);
        if (!ptr) return 2; // cudaErrorMemoryAllocation
        *devPtr = reinterpret_cast<uint64_t>(ptr);
        allocations_[*devPtr] = size;
        return 0; // cudaSuccess
    }

    int free(uint64_t devPtr) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = allocations_.find(devPtr);
        if (it == allocations_.end()) {
            return 11; // cudaErrorInvalidValue
        }
        std::free(reinterpret_cast<void*>(devPtr));
        allocations_.erase(it);
        return 0;
    }

    int memcpyHtoD(uint64_t dst, const void* src, size_t size) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (allocations_.find(dst) == allocations_.end()) {
            return 11;
        }
        std::memcpy(reinterpret_cast<void*>(dst), src, size);
        return 0;
    }

    int memcpyDtoH(void* dst, uint64_t src, size_t size) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (allocations_.find(src) == allocations_.end()) {
            return 11;
        }
        std::memcpy(dst, reinterpret_cast<const void*>(src), size);
        return 0;
    }

    int getDeviceProperties(std::string& name, size_t& totalGlobalMem) override {
        name = "Stubbed GPU (Host RAM)";
        totalGlobalMem = 1024 * 1024 * 1024; // Fake 1GB
        return 0;
    }

private:
    std::mutex mutex_;
    std::unordered_map<uint64_t, size_t> allocations_;
};

std::unique_ptr<ICudaExecutor> create_stub_cuda_executor() {
    return std::make_unique<CudaExecutorStub>();
}

} // namespace gpu
} // namespace meminfo
