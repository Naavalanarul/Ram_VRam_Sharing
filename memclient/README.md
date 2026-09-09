# MemInfo Memory Client (`memclient`)

`memclient` is the client-side library and CLI tool that applications use to allocate, read, write, and free remote RAM on the LAN tier. It acts as an automatic LRU cache manager to give applications the illusion of massive contiguous local memory.

## Architecture

1. **`LRUCache`**: The local hot tier. Applications interact with handles (`handle_t`). If data is recently used and fits within the `max_local_bytes_` limit, it lives purely in local memory (fast path).
2. **Eviction Engine**: When `LRUCache` fills up, the least recently used block is automatically evicted (pushed over TCP using size-prefixed FlatBuffers) to a remote `memoryd` peer on the LAN.
3. **Read-Through / Write-Back**: When the application reads an evicted block, `memclient` pulls it synchronously back from `memoryd`, reinserting it into the local `LRUCache` (potentially evicting something else).
4. **Peer Discovery**: Uses `discoveryd`'s Unix Domain Socket to find available remote `memoryd` instances.

## Building and Using

The library provides `client_api.h`.

```cpp
#include <meminfo/client/client_api.h>

// Initialize with a 1GB local LRU limit
meminfo::client::MemoryClient client(1024 * 1024 * 1024);

// Allocate 4KB (lives in local cache)
handle_t handle = client.allocate(4096);

// Write data
std::vector<uint8_t> payload = { ... };
client.write(handle, 0, payload);

// Read data (will transparently fetch from LAN if evicted)
std::vector<uint8_t> result = client.read(handle, 0, 4096);

// Free
client.free(handle);
```

## CLI Usage

A simple CLI is provided for manual testing:
```bash
./memclient_cli --max-local 1048576
```
