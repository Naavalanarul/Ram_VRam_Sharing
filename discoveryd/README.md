# MemInfo Discovery Daemon (`discoveryd`)

`discoveryd` is the control plane component of the MemInfo cluster. It is responsible for LAN peer discovery using UDP multicast and providing local status to CLI tools via a Unix domain socket.

## Architecture

1. **Announcer**: Periodically (default 1s) broadcasts a FlatBuffers `Announcement` message containing the local node's UUID, hostname, available RAM/VRAM, and daemon ports to the multicast group `239.255.73.77:9100`.
2. **Listener**: Subscribes to the multicast group to receive `Announcement` messages from other nodes on the LAN and updates the internal `PeerTable`.
3. **PeerTable**: Tracks the state of all discovered nodes. Nodes that fail to announce for `peer_ttl_seconds` (default 10s) are marked as `STALE` and eventually evicted.
4. **Control Socket**: Listens on a Unix domain socket (default `/var/run/meminfo_discovery.sock`) to respond to local queries (e.g., `LIST_PEERS`).

## Configuration

`discoveryd` uses TOML for configuration. By default, it looks for `/etc/meminfo/discoveryd.toml`.
You can specify a custom config path using the `-c` flag.

```toml
[discovery]
listen_address = "0.0.0.0"
memory_port = 9200
gpu_port = 9300
control_socket = "/var/run/meminfo_discovery.sock"
multicast_group = "239.255.73.77"
multicast_port = 9100
announce_interval_ms = 1000
peer_ttl_seconds = 10
```

## Running

```bash
./discoveryd -c /path/to/discoveryd.toml -l info
```

## Testing

A 3-node simulated LAN test is provided in `tests/test_multi_node.cpp`. This spins up 3 daemons on loopback multicast and verifies they correctly discover each other.

```bash
ctest -R test_discoveryd
```
