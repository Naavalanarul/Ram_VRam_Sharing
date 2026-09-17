// RemoteHeap probe.
//
// Not the demo, and not a unit test: this is the thing you run on the two real
// laptops to watch the numbers move. It allocates a region far larger than its
// local budget, writes to it through a plain pointer, reads it back, and prints
// this process's working set as it goes.
//
// What to expect, with Task Manager open on both machines:
//
//   * this process's memory climbs to roughly the local budget and then flat-
//     lines, however large --size is;
//   * memoryd on the peer climbs by roughly --size;
//   * the verification pass at the end reports no mismatches, which is what
//     says the bytes really made the round trip rather than being quietly lost.
//
// Examples:
//   remote_heap_probe --peer 192.168.1.42 --port 9200 --size 4G --budget 256M
//   remote_heap_probe --socket meminfo_discovery_ctl --size 4G --budget 256M

#include <CLI/CLI.hpp>
#include <meminfo/client/client_api.h>
#include <meminfo/client/remote_heap.h>
#include <meminfo/common/logging.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>

#if defined(_WIN32)
#  include <windows.h>
#  include <psapi.h>
#else
#  include <cstdio>
#endif

namespace {

// Bytes this process currently has in physical memory: working set on Windows,
// resident set size on Linux. Zero when it cannot be determined.
uint64_t process_resident_bytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return static_cast<uint64_t>(pmc.WorkingSetSize);
    }
    return 0;
#else
    std::FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long total_pages = 0, resident_pages = 0;
    const int matched = std::fscanf(f, "%lu %lu", &total_pages, &resident_pages);
    std::fclose(f);
    if (matched != 2) return 0;
    return static_cast<uint64_t>(resident_pages) * 4096u;
#endif
}

std::string human(uint64_t bytes) {
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < sizeof(units) / sizeof(units[0])) {
        value /= 1024.0;
        ++unit;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f %s", value, units[unit]);
    return buf;
}

// Accepts plain byte counts and K/M/G suffixes, so --size 4G reads naturally.
bool parse_size(const std::string& text, size_t& out) {
    if (text.empty()) return false;
    size_t multiplier = 1;
    std::string digits = text;

    const char suffix = static_cast<char>(std::toupper(static_cast<unsigned char>(text.back())));
    if (suffix == 'K' || suffix == 'M' || suffix == 'G') {
        digits = text.substr(0, text.size() - 1);
        multiplier = (suffix == 'K') ? 1024u : (suffix == 'M') ? 1024u * 1024u : 1024u * 1024u * 1024u;
    }

    try {
        const unsigned long long value = std::stoull(digits);
        out = static_cast<size_t>(value) * multiplier;
        return out != 0;
    } catch (const std::exception&) {
        return false;
    }
}

uint8_t expected_byte(size_t index) {
    return static_cast<uint8_t>((index * 1103515245u + 12345u) >> 16);
}

} // namespace

int main(int argc, char** argv) {
    CLI::App app{"RemoteHeap probe: writes and reads a peer-backed region through a plain pointer"};

    std::string peer_ip;
    app.add_option("--peer", peer_ip, "Peer IP to use directly, bypassing discovery");

    int peer_port = 9200;
    app.add_option("--port", peer_port, "Peer memoryd port (with --peer)");

    std::string discovery_socket = "meminfo_discovery_ctl";
    app.add_option("-s,--socket", discovery_socket, "discoveryd control socket (when --peer is not given)");

    std::string size_text = "1G";
    app.add_option("--size", size_text, "Region size, e.g. 512M or 4G");

    std::string budget_text = "256M";
    app.add_option("--budget", budget_text, "Local resident budget, e.g. 256M");

    std::string page_text = "64K";
    app.add_option("--page", page_text, "Transfer granularity, e.g. 64K");

    size_t max_local = 1024 * 1024;
    app.add_option("--max-local", max_local, "MemoryClient local cache ceiling in bytes");

    bool hold = false;
    app.add_flag("--hold", hold, "Stay running after the check, so Task Manager can be read at leisure");

    CLI11_PARSE(app, argc, argv);

    size_t size_bytes = 0, budget_bytes = 0, page_bytes = 0;
    if (!parse_size(size_text, size_bytes)) {
        std::cerr << "Could not parse --size: " << size_text << "\n";
        return 2;
    }
    if (!parse_size(budget_text, budget_bytes)) {
        std::cerr << "Could not parse --budget: " << budget_text << "\n";
        return 2;
    }
    if (!parse_size(page_text, page_bytes)) {
        std::cerr << "Could not parse --page: " << page_text << "\n";
        return 2;
    }

    try {
        meminfo::init_logging("remote_heap_probe", "logs", "info");

        const uint64_t baseline = process_resident_bytes();
        std::cout << "Process resident at start: " << human(baseline) << "\n";

        std::unique_ptr<meminfo::client::MemoryClient> client;
        if (!peer_ip.empty()) {
            std::cout << "Using peer " << peer_ip << ":" << peer_port << " directly\n";
            client = std::make_unique<meminfo::client::MemoryClient>(max_local, "", peer_ip, peer_port);
        } else {
            std::cout << "Using discovery socket " << discovery_socket << "\n";
            client = std::make_unique<meminfo::client::MemoryClient>(max_local, discovery_socket);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        meminfo::client::RemoteHeapConfig config;
        config.capacity_bytes = size_bytes;
        config.local_budget_bytes = budget_bytes;
        config.page_bytes = page_bytes;

        meminfo::client::RemoteHeap heap(*client, config);
        auto* bytes = static_cast<uint8_t*>(heap.data());
        const size_t total = heap.size();

        std::cout << "Region: " << human(total) << " at " << heap.data()
                  << ", local budget " << human(budget_bytes)
                  << ", page " << human(heap.page_bytes()) << "\n\n";

        // --- write pass -------------------------------------------------
        std::cout << "Writing through a plain pointer...\n";
        const auto write_start = std::chrono::steady_clock::now();
        size_t next_report = total / 8;
        for (size_t i = 0; i < total; ++i) {
            bytes[i] = expected_byte(i);
            if (i >= next_report) {
                const auto s = heap.stats();
                std::cout << "  " << human(i) << " written"
                          << " | this process: " << human(process_resident_bytes())
                          << " | heap resident: " << human(s.resident_bytes)
                          << " | fetches " << s.fetches
                          << " flushes " << s.flushes
                          << " evictions " << s.evictions << "\n";
                next_report += total / 8;
            }
        }
        const auto write_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - write_start).count();
        std::cout << "Write pass took " << write_ms << " ms\n\n";

        // --- push everything out ----------------------------------------
        std::cout << "Flushing and evicting everything, so the only copy is on the peer...\n";
        heap.flush_and_evict_all();
        std::cout << "  this process: " << human(process_resident_bytes())
                  << " | heap resident: " << human(heap.stats().resident_bytes) << "\n\n";

        // --- read-back and verify ---------------------------------------
        std::cout << "Reading back and verifying...\n";
        size_t mismatches = 0;
        size_t first_mismatch = 0;
        const auto read_start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < total; ++i) {
            if (bytes[i] != expected_byte(i)) {
                if (mismatches == 0) first_mismatch = i;
                ++mismatches;
            }
        }
        const auto read_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - read_start).count();

        const auto final_stats = heap.stats();
        std::cout << "Read pass took " << read_ms << " ms\n";
        std::cout << "  fetches " << final_stats.fetches
                  << " | flushes " << final_stats.flushes
                  << " | evictions " << final_stats.evictions << "\n";
        std::cout << "  this process: " << human(process_resident_bytes())
                  << " (started at " << human(baseline) << ")\n";
        std::cout << "  heap resident: " << human(final_stats.resident_bytes)
                  << " of a " << human(total) << " region\n";

        if (mismatches != 0) {
            std::cout << "\nFAILED: " << mismatches << " bytes differ, first at offset "
                      << first_mismatch << "\n";
            return 1;
        }
        std::cout << "\nOK: every byte of " << human(total)
                  << " round-tripped through the peer.\n";

        if (hold) {
            std::cout << "Holding. Ctrl-C when you are done reading Task Manager.\n";
            for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
