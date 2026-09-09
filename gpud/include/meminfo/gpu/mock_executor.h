#pragma once
#include <meminfo/gpu/cuda_executor.h>
namespace meminfo {
namespace gpu {
class MockExecutor : public ICudaExecutor {
public:
    MockExecutor();
    ~MockExecutor() override;
    void init() override;
    void shutdown() override;
    void alloc() override;
    void free() override;
    void copyToGpu() override;
    void copyFromGpu() override;
};
}
}
