// RemoteHeap: ordinary pointer accesses backed by a peer.
//
// The property under test is the one the whole design rests on -- a region much
// larger than the local budget can be written and read through a plain pointer,
// the process holds no more than the budget at any point, and the bytes survive
// a round trip through the peer.

#include <gtest/gtest.h>

#include <meminfo/client/client_api.h>
#include <meminfo/client/remote_heap.h>
#include <meminfo/common/config.h>
#include <meminfo/memory/memory_daemon.h>
#include <meminfo/platform/IPageFaultBackend.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

using namespace meminfo;
using namespace meminfo::client;
using namespace meminfo::memory;

namespace {

constexpr size_t kPoolBytes = 32u * 1024u * 1024u;
constexpr size_t kMaxLocalBytes = 64u * 1024u;
constexpr size_t kHeapBytes = 4u * 1024u * 1024u;
constexpr size_t kBudgetBytes = 256u * 1024u;
constexpr size_t kPageBytes = 64u * 1024u;

// Spins up a memoryd on an OS-assigned port for the lifetime of a test.
class LocalDaemon {
public:
    explicit LocalDaemon(const std::string& config_name) {
        const auto path = std::filesystem::temp_directory_path() / config_name;
        {
            std::ofstream out(path);
            out << "[memory]\n"
                << "listen_address = \"127.0.0.1\"\n"
                << "port = 0\n"
                << "total_reserved_bytes = " << kPoolBytes << "\n"
                << "page_size_bytes = 4096\n";
        }

        Config config(path.string());
        daemon_ = std::make_unique<MemoryDaemon>(config);
        thread_ = std::thread([this]() { daemon_->run(); });

        for (int i = 0; i < 500 && port_ == 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            port_ = daemon_->get_listen_port();
        }
    }

    ~LocalDaemon() {
        daemon_->stop();
        if (thread_.joinable()) thread_.join();
    }

    int port() const { return port_; }

private:
    std::unique_ptr<MemoryDaemon> daemon_;
    std::thread thread_;
    int port_ = 0;
};

uint8_t expected_byte(size_t index) {
    // Deliberately not a function of the low bits alone, so a page swapped with
    // its neighbour is caught rather than matching by coincidence.
    return static_cast<uint8_t>((index * 1103515245u + 12345u) >> 16);
}

bool page_faults_available() {
    auto backend = platform::create_page_fault_backend();
    return backend && backend->is_supported();
}

} // namespace

TEST(RemoteHeapTest, PointerAccessSurvivesEvictionToPeer) {
    if (!page_faults_available()) {
        GTEST_SKIP() << "Page-fault backend unavailable on this platform/permissions";
    }

    LocalDaemon daemon("meminfo_test_remote_heap.toml");
    ASSERT_NE(daemon.port(), 0) << "memoryd did not bind a port";

    MemoryClient client(kMaxLocalBytes, "", "127.0.0.1", daemon.port());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    RemoteHeapConfig config;
    config.capacity_bytes = kHeapBytes;
    config.local_budget_bytes = kBudgetBytes;
    config.page_bytes = kPageBytes;

    RemoteHeap heap(client, config);
    ASSERT_NE(heap.data(), nullptr);
    ASSERT_GE(heap.size(), kHeapBytes);

    auto* bytes = static_cast<uint8_t*>(heap.data());
    const size_t total = heap.size();

    // Plain stores. No API call anywhere in this loop -- that is the point.
    for (size_t i = 0; i < total; ++i) {
        bytes[i] = expected_byte(i);
    }

    // The process never held more than the budget, however large the region is.
    {
        const auto stats = heap.stats();
        EXPECT_LE(stats.resident_bytes, kBudgetBytes)
            << "resident set exceeded the local budget; nothing would plateau";
        EXPECT_GT(stats.evictions, 0u) << "nothing was ever handed back to the OS";
    }

    // Push everything out, so the only copy of the data is on the peer.
    heap.flush_and_evict_all();
    {
        const auto stats = heap.stats();
        EXPECT_EQ(stats.resident_bytes, 0u) << "pages still held locally after a full evict";
        EXPECT_GT(stats.flushes, 0u) << "dirty pages were never written back";
    }

    // Plain loads. Every one of these faults, fetches from the peer, and
    // resumes; if dirty tracking or the flush path were wrong, the written
    // bytes would not come back.
    size_t mismatches = 0;
    size_t first_mismatch = 0;
    for (size_t i = 0; i < total; ++i) {
        if (bytes[i] != expected_byte(i)) {
            if (mismatches == 0) first_mismatch = i;
            ++mismatches;
        }
    }
    EXPECT_EQ(mismatches, 0u) << mismatches << " bytes differ, first at offset " << first_mismatch;

    const auto stats = heap.stats();
    EXPECT_LE(stats.resident_bytes, kBudgetBytes);
    EXPECT_GE(stats.fetches, total / kPageBytes)
        << "fewer fetches than pages: the region cannot have been served from the peer";
}

// A region far larger than the machine would comfortably hold is still only a
// reservation until it is touched, and the budget still caps what is resident.
TEST(RemoteHeapTest, SparseAccessOnlyFetchesTouchedPages) {
    if (!page_faults_available()) {
        GTEST_SKIP() << "Page-fault backend unavailable on this platform/permissions";
    }

    LocalDaemon daemon("meminfo_test_remote_heap_sparse.toml");
    ASSERT_NE(daemon.port(), 0) << "memoryd did not bind a port";

    MemoryClient client(kMaxLocalBytes, "", "127.0.0.1", daemon.port());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    RemoteHeapConfig config;
    config.capacity_bytes = kHeapBytes;
    config.local_budget_bytes = kBudgetBytes;
    config.page_bytes = kPageBytes;

    RemoteHeap heap(client, config);
    auto* bytes = static_cast<uint8_t*>(heap.data());

    EXPECT_EQ(heap.stats().resident_bytes, 0u) << "reserving must not make anything resident";

    // Touch one byte in each of three pages, spread across the region.
    const size_t touched[] = {0, kPageBytes * 5 + 17, kPageBytes * 30 + 4095};
    for (size_t offset : touched) {
        bytes[offset] = expected_byte(offset);
    }

    const auto stats = heap.stats();
    EXPECT_EQ(stats.fetches, 3u) << "an untouched page was fetched";
    EXPECT_EQ(stats.resident_bytes, 3 * kPageBytes);

    for (size_t offset : touched) {
        EXPECT_EQ(bytes[offset], expected_byte(offset)) << "at offset " << offset;
    }
}
