# MemInfo Control API

The MemInfo ecosystem uses a **Control API** to interface with the core `discoveryd` daemon locally. This API is exposed via a Unix Domain Socket (UDS) and utilizes FlatBuffers for lightweight serialization.

By default, `discoveryd` listens on:
```
/var/run/meminfo_discovery.sock
```

*(Note: On systems where `/var/run` requires root privileges, this can be configured to a user-local directory via `discoveryd.toml`)*.

## FlatBuffers Schema (`control.fbs`)

All requests are encapsulated in a `ControlRequest` root table, and all responses are returned as a `ControlResponse` root table.

### Operations

#### `LIST_PEERS` (OpCode: 0)
Queries the daemon for the current active routing table of LAN peers.

**Request:**
```json
{
  "command": "LIST_PEERS",
  "request_id": 12345
}
```

**Response:**
Returns a list of `PeerInfo` objects representing all alive peers detected via UDP multicast.
```json
{
  "request_id": 12345,
  "success": true,
  "peers": [
    {
      "hostname": "ubuntu-node-1",
      "address": "192.168.1.100",
      "free_ram_bytes": 17179869184,
      "memory_port": 9200,
      "gpu_port": 9300,
      "state": "ACTIVE"
    }
  ]
}
```

### Usage in Clients
Clients (like `memclient` or `gpuclient`) automatically use this UDS socket upon initialization to populate their connection pools. They establish a synchronous connection to the socket, send the size-prefixed `ControlRequest`, and block until the `ControlResponse` is returned with the routing table.

Because `discoveryd` tracks heartbeats and TTLs in the background, clients are guaranteed an up-to-date snapshot of the network state without having to implement complex multicast listening logic themselves.
