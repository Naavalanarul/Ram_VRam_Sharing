#include <windows.h>
#include <cuda_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include "RemoteHeap.hpp"
#include "TcpPageFaultBackend.hpp"

#define CUDA_CHECK(call) \
    do { \
        cudaError_t err = call; \
        if (err != cudaSuccess) { \
            std::cerr << "CUDA error at " << __FILE__ << ":" << __LINE__ << " - " << cudaGetErrorString(err) << "\n"; \
            return 1; \
        } \
    } while(0)

int main() {
    std::cout << "=== Windows LAN Sharing Integration Test ===\n";

    const char* paging_ip = std::getenv("REMOTE_PAGING_IP");
    if (!paging_ip) paging_ip = "127.0.0.1";

    const char* gpu_ip = std::getenv("REMOTE_GPU_IP");
    if (!gpu_ip) gpu_ip = "127.0.0.1";

    std::cout << "Paging server: " << paging_ip << ":9999\n";
    std::cout << "GPU server: " << gpu_ip << ":9998\n";

    TcpPageFaultBackend paging_backend;
    if (!paging_backend.initialize(paging_ip, 9999)) {
        std::cerr << "Failed to connect to paging server\n";
        return 1;
    }
    std::cout << "Connected to paging server\n";

    RemoteHeap heap(512 * 1024 * 1024); // 512 MB
    if (!heap.base()) {
        std::cerr << "Failed to reserve virtual memory\n";
        return 1;
    }
    std::cout << "Reserved 512 MB at " << heap.base() << "\n";

    if (!heap.initialize(&paging_backend)) {
        std::cerr << "Failed to initialize heap\n";
        return 1;
    }
    std::cout << "Heap initialized with VEH handler\n";

    char* test_ptr = static_cast<char*>(heap.base()) + 1024 * 1024; // 1 MB offset
    std::cout << "Touching memory at offset 1 MB (" << test_ptr << ") to trigger page fault...\n";

    const char* test_data = "Hello from RemoteHeap over LAN!";
    std::strcpy(test_ptr, test_data);
    std::cout << "Write succeeded, read back: " << test_ptr << "\n";

    std::cout << "\nTesting CUDA proxy...\n";
    void* d_ptr = nullptr;
    CUDA_CHECK(cudaMalloc(&d_ptr, 256));
    std::cout << "cudaMalloc succeeded: " << d_ptr << "\n";

    CUDA_CHECK(cudaMemcpy(d_ptr, test_ptr, 64, cudaMemcpyHostToDevice));
    std::cout << "cudaMemcpy H2D succeeded\n";

    CUDA_CHECK(cudaDeviceSynchronize());
    std::cout << "cudaDeviceSynchronize succeeded\n";

    char host_buf[256] = {0};
    CUDA_CHECK(cudaMemcpy(host_buf, d_ptr, 64, cudaMemcpyDeviceToHost));
    std::cout << "cudaMemcpy D2H succeeded\n";
    std::cout << "Data from GPU: " << host_buf << "\n";

    if (std::strcmp(host_buf, test_data) != 0) {
        std::cerr << "DATA MISMATCH!\n";
        return 1;
    }
    std::cout << "Data integrity verified!\n";

    CUDA_CHECK(cudaFree(d_ptr));
    std::cout << "cudaFree succeeded\n";

    std::cout << "\n=== ALL TESTS PASSED ===\n";
    return 0;
}