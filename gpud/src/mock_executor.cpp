#include <meminfo/gpu/mock_executor.h>
namespace meminfo { namespace gpu {
MockExecutor::MockExecutor() {}
MockExecutor::~MockExecutor() {}
void MockExecutor::init() {}
void MockExecutor::shutdown() {}
void MockExecutor::alloc() {}
void MockExecutor::free() {}
void MockExecutor::copyToGpu() {}
void MockExecutor::copyFromGpu() {}
}}
