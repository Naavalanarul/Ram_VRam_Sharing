# MemInfo Security Considerations

## Current Trust Model (v1)

In v1, all nodes on the same LAN are treated as a single trust domain. There is:
- **No encryption** of data in transit (memory pages, GPU data, control commands)
- **No authentication** of peers (any node that can reach the multicast group can join)
- **No authorization** (any joined peer can allocate/read/write/free memory on any other peer)
- **No encryption at rest** for the memory slab

The control socket (Unix domain socket) provides basic OS-level access control via
filesystem permissions.

## Defensive Measures in v1

Despite the trust-based model, v1 does implement defensive parsing:
- All incoming FlatBuffers messages are verified with `flatbuffers::Verifier` before access
- Message lengths are validated against maximum sizes
- Protocol version mismatches are detected and rejected with clear logging
- Invalid handles, out-of-bounds offsets, and malformed packets never cause crashes
- CRC32C checksums verify data integrity on memory pages

## TODO: v2 Security Enhancements

### Transport Encryption
- [ ] TLS 1.3 for all TCP connections (memory data plane, GPU forwarding)
- [ ] DTLS for UDP discovery (or switch discovery to TCP+TLS)
- [ ] Use OpenSSL or BoringSSL

### Authentication
- [ ] Pre-shared key (PSK) for simple deployments
- [ ] Mutual TLS (mTLS) with per-node certificates for production
- [ ] Certificate auto-provisioning (ACME-like for LAN)

### Authorization
- [ ] Per-node access control lists (which nodes can allocate memory on which)
- [ ] Quota enforcement (max memory per remote node)
- [ ] Read-only vs read-write access to remote memory

### Data Protection
- [ ] Encrypted pages at rest in the memory slab (AES-256-GCM)
- [ ] Secure erasure on free (zero-fill or crypto-erase)

### Control Socket
- [ ] Peer credential verification via `SO_PEERCRED` (Linux)
- [ ] Fine-grained command authorization

### Audit
- [ ] Audit log of all peer connections, allocations, and access events
- [ ] Tamper-evident logging
