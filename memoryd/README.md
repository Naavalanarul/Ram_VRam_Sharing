# MemInfo Memory Daemon (`memoryd`)

`memoryd` is the core data-plane daemon responsible for providing remote RAM to the LAN. It pre-allocates a large contiguous block of memory and serves it to authorized clients over a fast, async TCP protocol.

## Architecture

1. **Slab Allocator**: Pre-allocates a fixed block of memory (e.g., 256MB) partitioned into fixed-size pages (e.g., 4KB). Provides an `O(1)` free-list allocator.
2. **Page Tracker**: Maps logical, contiguous client allocations (identified by a `handle_t`) to the scattered physical pages backing them.
3. **Client Session**: Manages a single TCP connection. Decodes `SizePrefixed` FlatBuffers (`meminfo.memory.MemoryRequest`), routes them to the `PageTracker`, checks `CRC32C` integrity on `WRITE`, and serializes responses (`meminfo.memory.MemoryResponse`).

## Configuration

`memoryd` uses TOML for configuration. By default, it looks for `/etc/meminfo/memoryd.toml`.

```toml
[memory]
listen_address = "0.0.0.0"
port = 9200
total_reserved_bytes = 268435456  # 256MB pre-allocated
page_size_bytes = 4096            # 4KB pages
```

## Running

```bash
./memoryd -c /path/to/memoryd.toml -l info
```

## Protocol Specs

`memoryd` implements a straightforward Remote Procedure Call interface via `meminfo.memory` FlatBuffers schemas:
- `ALLOC`: Requests `N` bytes. Returns a logical `handle`.
- `WRITE`: Requires `handle`, `offset`, `data`, and a `CRC32C` checksum of the payload.
- `READ`: Requires `handle`, `offset`, `size`. Returns `data` and its `CRC32C` checksum.
- `FREE`: Explicitly frees a handle.

*Note: If a TCP connection abruptly terminates, `memoryd` guarantees that all handles allocated during that session are automatically freed back to the slab.*
