#!/bin/bash
#
# Manual local-cluster helper. The automated end-to-end test no longer uses
# this script: integration/test_cluster.cpp drives the same daemons through
# libuv so it runs on Windows too. Kept for hands-on runs on POSIX, where it is
# handy to leave the cluster up and poke at it.
set -e

echo "======================================"
echo " MemInfo Local Cluster Test Script"
echo "======================================"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
# The build directory is supplied by the caller (the integration test passes the
# directory CMake actually configured); fall back to the documented default.
BUILD_DIR="${MEMINFO_BUILD_DIR:-$ROOT_DIR/build/debug}"
SOCKET_PATH="/tmp/meminfo_test_discovery.sock"

cd "$ROOT_DIR"

DISC_PID=""
MEM_PID=""
GPU_PID=""

# `set -e` aborts on the first failure, so tear the daemons down from a trap
# rather than from a final line that may never be reached.
cleanup() {
    kill $DISC_PID $MEM_PID $GPU_PID 2>/dev/null || true
    rm -f /tmp/meminfo_test_*.toml
    rm -f "$SOCKET_PATH"
}
trap cleanup EXIT

# Ensure build exists
if [ ! -d "$BUILD_DIR" ]; then
    echo "Building project first..."
    cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug
    cmake --build "$BUILD_DIR" -j8
fi

echo "[1/4] Starting discoveryd..."
cat << EOF > /tmp/meminfo_test_discovery.toml
[discovery]
control_socket = "$SOCKET_PATH"
multicast_group = "239.255.0.1"
multicast_port = 8123
announce_interval_ms = 500
peer_timeout_ms = 3000
use_loopback = true
EOF

"$BUILD_DIR"/discoveryd/discoveryd --config /tmp/meminfo_test_discovery.toml > /tmp/discoveryd.log 2>&1 &
DISC_PID=$!
sleep 1 # wait for UDS

echo "[2/4] Starting memoryd..."
cat << EOF > /tmp/meminfo_test_memoryd.toml
[memory]
listen_address = "127.0.0.1"
port = 9255
total_reserved_bytes = 10485760 # 10MB
page_size_bytes = 4096
EOF

"$BUILD_DIR"/memoryd/memoryd --config /tmp/meminfo_test_memoryd.toml > /tmp/memoryd.log 2>&1 &
MEM_PID=$!

echo "[3/4] Starting gpud..."
cat << EOF > /tmp/meminfo_test_gpud.toml
[gpu]
listen_address = "127.0.0.1"
port = 9355
EOF

"$BUILD_DIR"/gpud/gpud --config /tmp/meminfo_test_gpud.toml > /tmp/gpud.log 2>&1 &
GPU_PID=$!

sleep 2 # Let daemons announce to discoveryd

echo "[4/4] Running memclient CLI..."
# We pass the custom discovery socket to memclient_cli
"$BUILD_DIR"/memclient/memclient_cli --socket $SOCKET_PATH --max-local 2048

echo "======================================"
echo " Cluster Test Complete! Cleaning up..."
echo "======================================"

echo "Success!"
