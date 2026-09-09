# MemInfo: LAN RAM + GPU Sharing

MemInfo is a high-performance C++ backend for a LAN-based resource-sharing system. It enables devices on a local network to pool their unused RAM and GPU resources, treating remote LAN nodes as transparently accessible memory tiers and remote execution targets.

## Features

- **RAM Sharing (`memoryd` + `memclient`)**: Transparently spills memory to idle RAM on other devices on the LAN using an LRU read-through / write-back cache, pushing data over TCP using FlatBuffers with CRC32C verification.
- **GPU Sharing (`gpud` + `GpuClient`)**: Allows devices without GPUs (or busy GPUs) to offload CUDA compute calls (`cudaMalloc`, `cudaMemcpy`, etc.) to a remote node on the LAN.
- **Auto-Discovery (`discoveryd`)**: Zero-configuration UDP multicast LAN discovery to dynamically discover available memory daemons and GPU daemons.

## Project Structure

- **`common/`**: Shared utilities (UUID, CRC32C, TOML configs, spdlog integration) and FlatBuffers schemas.
- **`discoveryd/`**: UDP multicast peer discovery and UDS control socket server.
- **`memoryd/`**: TCP daemon serving remote RAM using a lock-free O(1) Slab Allocator.
- **`memclient/`**: Client library implementing transparent LRU eviction and background TCP syncing.
- **`gpud/`**: GPU daemon and client for forwarding CUDA calls. Includes a stub fallback for systems without a physical GPU.
- **`integration/`**: End-to-end integration tests.

## Build Requirements

- CMake 3.16+
- C++17 compliant compiler (GCC 9+, Clang 10+, AppleClang)
- *(Optional)* NVIDIA CUDA Toolkit (if missing, `gpud` falls back to simulating a GPU with host RAM)

MemInfo handles its own dependencies (libuv, FlatBuffers, spdlog, toml++, CLI11, GoogleTest) automatically via CMake `FetchContent`.

## Building

```bash
# Generate build system
cmake -B build/release -DCMAKE_BUILD_TYPE=Release

# Build all components
cmake --build build/release -j8
```

## Running a Local Cluster

For testing on a single machine:

1. **Start Discovery Daemon**:
   ```bash
   ./build/release/discoveryd/discoveryd
   ```
2. **Start Memory Daemon**:
   ```bash
   ./build/release/memoryd/memoryd
   ```
3. **Start GPU Daemon**:
   ```bash
   ./build/release/gpud/gpud
   ```
4. **Test with Memory Client**:
   ```bash
   ./build/release/memclient/memclient_cli --max-local 1048576
   ```

*Note: In a production environment, you would run these daemons on different physical nodes connected to the same LAN subnet.*

## Testing
To run the full suite of unit and integration tests:
```bash
cmake -B build/debug -DCMAKE_BUILD_TYPE=Debug -DMEMINFO_BUILD_TESTS=ON
cmake --build build/debug -j8
cd build/debug && ctest --output-on-failure
```
