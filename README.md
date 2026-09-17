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
- **`tools/`**: Hand-run diagnostics — `remote_heap_probe` (watch a peer-backed region plateau) and `win_veh_probe` (Windows page-fault validation).
- **`hook/`**: Windows-only malloc interposer (`meminfo_hook.dll`) and its launcher, which route a host application's large allocations onto a peer.

## Two Windows implementations

The repository currently holds two independent takes on remote paging. They do
not share code and neither replaces the other.

**The main tree** (`platform/`, `memclient/`, `memoryd/`, `discoveryd/`, `hook/`,
`tools/`) is what `cmake -B build` at the repository root builds, and what CI
compiles and tests on Linux, macOS and Windows. Its `RemoteHeap` fetches pages
on fault, tracks which pages have been written, and evicts to a local budget so
the process's memory footprint plateaus. The Windows malloc interposer in
`hook/` sits on top of it. Start from [`docs/DEMO_RUNBOOK.md`](docs/DEMO_RUNBOOK.md).

**`windows-lan-sharing/`** is a separate Windows-only proof-of-concept with its
own wire protocol, paging server and a CUDA proxy DLL for forwarding GPU calls.
It is not referenced by the root `CMakeLists.txt`, so it is not built or tested
by the normal build or by CI; it has its own `CMakeLists.txt` and requires
Windows, MSVC and the CUDA toolkit. Adding it to the root build as-is would
fail configuration on Linux and macOS, which its own `CMakeLists.txt` rejects
by design. Build it separately if you want it.

## Transparent pointer access (`RemoteHeap`)

Above the handle-based `memclient` API sits `RemoteHeap`: a region of ordinary
memory whose contents live on a peer. Reads and writes are plain pointer
accesses with no API to call — touching an absent page faults, the handler
fetches that page from the peer, and the access resumes. Once resident bytes
exceed a configured budget the oldest pages are flushed if dirty and handed back
to the operating system, so the process's memory figure plateaus instead of
climbing.

`tools/remote_heap_probe` demonstrates it:

```bash
./build/tools/remote_heap_probe --peer 192.168.1.42 --port 9200 \
    --size 4G --budget 256M
```

On Windows, `hook/` puts this behind a Detours malloc interposer so an unmodified
application's large allocations are served from a peer. See
[`docs/DEMO_RUNBOOK.md`](docs/DEMO_RUNBOOK.md) for setup, tuning and the
validation steps to run before relying on it.

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
