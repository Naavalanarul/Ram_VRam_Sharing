# MemInfo GPU Daemon (`gpud`) and Client

This module implements the **GPU Sharing** subsystem of MemInfo. It provides a way to execute CUDA operations on a remote GPU over a plain Ethernet LAN connection.

## Components

### `gpud` (GPU Daemon)
A TCP daemon that listens for incoming `GpuRequest` FlatBuffers messages. It decodes these messages and routes them to an implementation of `ICudaExecutor`.

- **Real Executor**: If the project is compiled on a system with the NVIDIA CUDA Toolkit installed, it builds `cuda_executor_real.cpp`, translating requests directly to `cudaMalloc`, `cudaMemcpy`, etc.
- **Stub Executor**: If CUDA is not found during the CMake phase, it falls back to a stub executor that simulates a GPU using standard host RAM (`malloc`, `free`, `memcpy`). This ensures developers can test the remote RPC pipeline safely on non-GPU machines (like MacBooks).

### `GpuClient`
A thread-safe, synchronous C++ client library (`gpu_client.h`). It mirrors standard CUDA runtime APIs but transparently packages the arguments into a FlatBuffer, dispatches it to a background `libuv` TCP thread, and waits for the remote `gpud` to respond.

## Usage

Start the daemon on the machine with the GPU:
```bash
./gpud --config /etc/meminfo/gpud.toml
```

Use the client in your application:
```cpp
#include <meminfo/gpu/gpu_client.h>

// Connect to remote GPU
meminfo::gpu::GpuClient client("192.168.1.100", 9300);

// Print remote GPU details
std::string name;
size_t mem;
client.cudaGetDeviceProperties(name, mem);
std::cout << "Connected to: " << name << " (" << mem << " bytes)" << std::endl;

// Allocate remote device memory
uint64_t dev_ptr = 0;
client.cudaMalloc(&dev_ptr, 1024);

// Copy data Host -> Device
std::vector<uint8_t> h_data(1024, 0xAB);
client.cudaMemcpyHtoD(dev_ptr, h_data.data(), h_data.size());

// Copy data Device -> Host
std::vector<uint8_t> h_out(1024, 0);
client.cudaMemcpyDtoH(h_out.data(), dev_ptr, h_out.size());

// Free
client.cudaFree(dev_ptr);
```
