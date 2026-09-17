// Malloc interposer: routes a host process's large allocations onto a peer.
//
// Loaded into the target (GIMP) at process creation by meminfo_launch, which
// uses DetourCreateProcessWithDllEx so the hooks are in place before the
// program's first instruction runs. Allocations at or above a size threshold
// are served from a RemoteHeap -- address space backed by another machine's RAM
// -- and everything smaller goes straight to the real allocator. Small,
// short-lived allocations are not worth a network page fault, and intercepting
// them multiplies the risk for no gain.
//
// Two modes, set by MEMINFO_HOOK_MODE:
//
//   log     Counts and reports allocations without redirecting any of them.
//           This is Phase 0 step 3: it answers whether the host's big buffers
//           even arrive through the C runtime, or whether they come from a
//           private allocator that this technique cannot see.
//
//   remote  Redirects allocations at or above the threshold. The real thing.
//
// Configuration, all through the environment so the target needs no arguments:
//
//   MEMINFO_HOOK_MODE      log | remote                  (default log)
//   MEMINFO_PEER           peer IP, bypassing discovery   (default unset)
//   MEMINFO_PORT           peer memoryd port              (default 9200)
//   MEMINFO_SOCKET         discoveryd control socket      (default meminfo_discovery_ctl)
//   MEMINFO_THRESHOLD      bytes; smaller allocations pass through (default 1048576)
//   MEMINFO_HEAP_SIZE      remote region size in bytes    (default 4 GiB)
//   MEMINFO_BUDGET         resident bytes before eviction (default 512 MiB)
//   MEMINFO_PAGE           transfer granularity in bytes  (default 65536)
//   MEMINFO_MAX_LOCAL      MemoryClient cache ceiling     (default 1 MiB)
//   MEMINFO_LOG            path for this DLL's log        (default meminfo_hook.log
//                                                          beside the executable)

#include <windows.h>
#include <detours.h>

#include <meminfo/client/client_api.h>
#include <meminfo/client/heap_arena.h>
#include <meminfo/client/remote_heap.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

namespace {

// ── configuration ────────────────────────────────────────────────────────────

struct Config {
    bool remote_mode = false;
    std::string peer_ip;
    int peer_port = 9200;
    std::string discovery_socket = "meminfo_discovery_ctl";
    size_t threshold = 1u << 20;
    size_t heap_size = size_t{4} << 30;
    size_t budget = size_t{512} << 20;
    size_t page_bytes = 64u << 10;
    size_t max_local = 1u << 20;
    std::string log_path = "meminfo_hook.log";
};

Config g_config;
FILE* g_log = nullptr;
std::mutex g_log_mutex;

void log_line(const char* fmt, ...) {
    if (!g_log) return;
    std::lock_guard<std::mutex> lock(g_log_mutex);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_log, fmt, args);
    va_end(args);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

std::string env_string(const char* name, const std::string& fallback) {
    char buf[1024];
    const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return fallback;
    return std::string(buf, n);
}

size_t env_size(const char* name, size_t fallback) {
    const std::string text = env_string(name, "");
    if (text.empty()) return fallback;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
    if (end == text.c_str() || value == 0) return fallback;
    return static_cast<size_t>(value);
}

void load_config() {
    g_config.remote_mode = env_string("MEMINFO_HOOK_MODE", "log") == "remote";
    g_config.peer_ip = env_string("MEMINFO_PEER", "");
    g_config.peer_port = static_cast<int>(env_size("MEMINFO_PORT", 9200));
    g_config.discovery_socket = env_string("MEMINFO_SOCKET", "meminfo_discovery_ctl");
    g_config.heap_size = env_size("MEMINFO_HEAP_SIZE", size_t{4} << 30);
    g_config.budget = env_size("MEMINFO_BUDGET", size_t{512} << 20);
    g_config.page_bytes = env_size("MEMINFO_PAGE", 64u << 10);
    g_config.max_local = env_size("MEMINFO_MAX_LOCAL", 1u << 20);
    g_config.log_path = env_string("MEMINFO_LOG", "meminfo_hook.log");

    // Servicing a fault buffers a page and builds a protocol message, and those
    // allocations run through these same hooks. Keeping the threshold well
    // above the page size means none of them qualify for redirection, so the
    // fault path cannot allocate out of the heap it is servicing.
    // RemoteHeap::servicing_fault() is the belt to this braces.
    const size_t floor = g_config.page_bytes * 8;
    g_config.threshold = env_size("MEMINFO_THRESHOLD", 1u << 20);
    if (g_config.threshold < floor) {
        g_config.threshold = floor;
    }
}

// ── reentrancy ───────────────────────────────────────────────────────────────

// Set while this thread is inside a hook. Everything the redirection path does
// -- constructing the client, talking to the peer, logging -- allocates, and
// those allocations must go to the real heap rather than back through here.
thread_local int g_depth = 0;

struct Reentry {
    Reentry() { ++g_depth; }
    ~Reentry() { --g_depth; }
};

bool should_pass_through() {
    // g_depth: an allocation made by the redirection path itself.
    // servicing_fault(): an allocation made while answering a page fault, which
    //   can happen on any thread, including one that never entered a hook.
    return g_depth > 0 || meminfo::client::RemoteHeap::servicing_fault();
}

// ── redirected heap ──────────────────────────────────────────────────────────

struct RemoteState {
    std::unique_ptr<meminfo::client::MemoryClient> client;
    std::unique_ptr<meminfo::client::RemoteHeap> heap;
    std::unique_ptr<meminfo::client::HeapArena> arena;
    std::atomic<bool> ready{false};
    std::atomic<bool> failed{false};
    std::mutex init_mutex;
};

RemoteState g_state;

std::atomic<uint64_t> g_redirected{0};
std::atomic<uint64_t> g_redirected_bytes{0};
std::atomic<uint64_t> g_passed_through{0};
std::atomic<uint64_t> g_large_seen{0};
std::atomic<uint64_t> g_large_bytes{0};

// Built lazily rather than in DllMain: this opens sockets and starts threads,
// none of which is legal under the loader lock.
bool ensure_initialized() {
    if (g_state.ready.load(std::memory_order_acquire)) return true;
    if (g_state.failed.load(std::memory_order_acquire)) return false;

    std::lock_guard<std::mutex> lock(g_state.init_mutex);
    if (g_state.ready.load(std::memory_order_relaxed)) return true;
    if (g_state.failed.load(std::memory_order_relaxed)) return false;

    try {
        if (!g_config.peer_ip.empty()) {
            log_line("init: connecting directly to %s:%d", g_config.peer_ip.c_str(), g_config.peer_port);
            g_state.client = std::make_unique<meminfo::client::MemoryClient>(
                g_config.max_local, "", g_config.peer_ip, g_config.peer_port);
        } else {
            log_line("init: using discovery socket %s", g_config.discovery_socket.c_str());
            g_state.client = std::make_unique<meminfo::client::MemoryClient>(
                g_config.max_local, g_config.discovery_socket);
        }

        meminfo::client::RemoteHeapConfig heap_config;
        heap_config.capacity_bytes = g_config.heap_size;
        heap_config.local_budget_bytes = g_config.budget;
        heap_config.page_bytes = g_config.page_bytes;

        g_state.heap = std::make_unique<meminfo::client::RemoteHeap>(*g_state.client, heap_config);
        g_state.arena = std::make_unique<meminfo::client::HeapArena>(g_state.heap->data(),
                                                                    g_state.heap->size());

        log_line("init: %zu byte region at %p, budget %zu, threshold %zu",
                 g_state.heap->size(), g_state.heap->data(), g_config.budget, g_config.threshold);
        g_state.ready.store(true, std::memory_order_release);
        return true;
    } catch (const std::exception& e) {
        // Never fatal. The host keeps running on its own allocator; the only
        // loss is that nothing is offloaded.
        log_line("init FAILED: %s -- every allocation will use the real heap", e.what());
        g_state.failed.store(true, std::memory_order_release);
        g_state.arena.reset();
        g_state.heap.reset();
        g_state.client.reset();
        return false;
    }
}

void* remote_allocate(size_t bytes, size_t alignment) {
    if (!g_config.remote_mode) return nullptr;
    if (!ensure_initialized()) return nullptr;

    void* p = g_state.arena->allocate(bytes, alignment);
    if (p) {
        g_redirected.fetch_add(1, std::memory_order_relaxed);
        g_redirected_bytes.fetch_add(bytes, std::memory_order_relaxed);
    }
    return p;
}

bool arena_owns(const void* p) {
    return g_state.ready.load(std::memory_order_acquire) && g_state.arena && g_state.arena->owns(p);
}

// ── CRT hooking ──────────────────────────────────────────────────────────────

// GIMP's official Windows builds come from MSYS2 and link msvcrt.dll, while an
// MSVC-built program uses ucrtbase.dll and the api-ms-win-crt-* forwarders. A
// single process can have more than one of these loaded at once, and a block
// from one CRT's malloc must be returned to that same CRT's free -- so each
// module gets its own slot with its own trampolines, and the detour for a slot
// only ever calls back into that slot.
constexpr int kMaxSlots = 4;

struct CrtSlot {
    const char* module_name = nullptr;
    HMODULE module = nullptr;

    void* (__cdecl* real_malloc)(size_t) = nullptr;
    void* (__cdecl* real_calloc)(size_t, size_t) = nullptr;
    void* (__cdecl* real_realloc)(void*, size_t) = nullptr;
    void  (__cdecl* real_free)(void*) = nullptr;
    size_t (__cdecl* real_msize)(void*) = nullptr;
};

CrtSlot g_slots[kMaxSlots];
int g_slot_count = 0;

void note_large(size_t bytes) {
    g_large_seen.fetch_add(1, std::memory_order_relaxed);
    g_large_bytes.fetch_add(bytes, std::memory_order_relaxed);
    // Only the first few, and then powers of two, or a busy host floods the log.
    const uint64_t seen = g_large_seen.load(std::memory_order_relaxed);
    if (seen <= 16 || (seen & (seen - 1)) == 0) {
        log_line("large allocation #%llu: %zu bytes",
                 static_cast<unsigned long long>(seen), bytes);
    }
}

template <int Slot>
void* __cdecl hooked_malloc(size_t bytes) {
    CrtSlot& slot = g_slots[Slot];

    if (bytes < g_config.threshold || should_pass_through()) {
        g_passed_through.fetch_add(1, std::memory_order_relaxed);
        return slot.real_malloc(bytes);
    }

    const Reentry guard;
    note_large(bytes);

    if (void* p = remote_allocate(bytes, 16)) {
        return p;
    }
    // Arena full or fragmented, or remote setup failed: the real heap still
    // works, so the host does not care.
    return slot.real_malloc(bytes);
}

template <int Slot>
void* __cdecl hooked_calloc(size_t count, size_t size) {
    CrtSlot& slot = g_slots[Slot];

    size_t bytes = 0;
    // A count*size overflow would under-allocate and hand back a block the
    // caller then writes past, so refuse it the way the CRT does.
    if (count != 0 && size > (static_cast<size_t>(-1) / count)) {
        return nullptr;
    }
    bytes = count * size;

    if (bytes < g_config.threshold || should_pass_through()) {
        return slot.real_calloc(count, size);
    }

    const Reentry guard;
    note_large(bytes);

    void* p = remote_allocate(bytes, 16);
    if (!p) return slot.real_calloc(count, size);

    // Zeroing touches every page, so a calloc of the whole region pulls the
    // whole region across. Correctness first: a recycled arena block carries
    // whatever the previous owner left in it.
    std::memset(p, 0, bytes);
    return p;
}

template <int Slot>
void __cdecl hooked_free(void* p) {
    CrtSlot& slot = g_slots[Slot];

    if (p && arena_owns(p)) {
        const Reentry guard;
        g_state.arena->deallocate(p);
        return;
    }
    slot.real_free(p);
}

template <int Slot>
size_t __cdecl hooked_msize(void* p) {
    CrtSlot& slot = g_slots[Slot];

    if (p && arena_owns(p)) {
        const Reentry guard;
        return g_state.arena->block_size(p);
    }
    return slot.real_msize(p);
}

template <int Slot>
void* __cdecl hooked_realloc(void* p, size_t bytes) {
    CrtSlot& slot = g_slots[Slot];

    if (!p) return hooked_malloc<Slot>(bytes);
    if (bytes == 0) {
        hooked_free<Slot>(p);
        return nullptr;
    }

    if (arena_owns(p)) {
        const Reentry guard;
        const size_t old_bytes = g_state.arena->block_size(p);

        void* dst = remote_allocate(bytes, 16);
        if (!dst) {
            // Falling back to the real heap is fine, and moving the data out is
            // what keeps the block usable.
            dst = slot.real_malloc(bytes);
            if (!dst) return nullptr;
        }
        std::memcpy(dst, p, old_bytes < bytes ? old_bytes : bytes);
        g_state.arena->deallocate(p);
        return dst;
    }

    // A foreign block growing past the threshold could be moved into the arena,
    // but only if its current size is known. Without _msize there is no safe
    // copy length, so leave it with the allocator that owns it.
    if (bytes < g_config.threshold || should_pass_through() || !slot.real_msize) {
        return slot.real_realloc(p, bytes);
    }

    const Reentry guard;
    note_large(bytes);

    const size_t old_bytes = slot.real_msize(p);
    void* dst = remote_allocate(bytes, 16);
    if (!dst) return slot.real_realloc(p, bytes);

    std::memcpy(dst, p, old_bytes < bytes ? old_bytes : bytes);
    slot.real_free(p);
    return dst;
}

// Detours needs the address of the pointer it rewrites, and one detour function
// per slot, so the table is built by instantiation rather than by loop.
template <int Slot>
bool attach_slot() {
    CrtSlot& slot = g_slots[Slot];
    bool any = false;

    if (slot.real_malloc && DetourAttach(reinterpret_cast<PVOID*>(&slot.real_malloc),
                                         reinterpret_cast<PVOID>(hooked_malloc<Slot>)) == NO_ERROR) {
        any = true;
    }
    if (slot.real_calloc && DetourAttach(reinterpret_cast<PVOID*>(&slot.real_calloc),
                                         reinterpret_cast<PVOID>(hooked_calloc<Slot>)) == NO_ERROR) {
        any = true;
    }
    if (slot.real_realloc && DetourAttach(reinterpret_cast<PVOID*>(&slot.real_realloc),
                                          reinterpret_cast<PVOID>(hooked_realloc<Slot>)) == NO_ERROR) {
        any = true;
    }
    if (slot.real_free && DetourAttach(reinterpret_cast<PVOID*>(&slot.real_free),
                                       reinterpret_cast<PVOID>(hooked_free<Slot>)) == NO_ERROR) {
        any = true;
    }
    if (slot.real_msize && DetourAttach(reinterpret_cast<PVOID*>(&slot.real_msize),
                                        reinterpret_cast<PVOID>(hooked_msize<Slot>)) == NO_ERROR) {
        any = true;
    }
    return any;
}

template <int Slot>
void detach_slot() {
    CrtSlot& slot = g_slots[Slot];
    if (slot.real_malloc) DetourDetach(reinterpret_cast<PVOID*>(&slot.real_malloc),
                                       reinterpret_cast<PVOID>(hooked_malloc<Slot>));
    if (slot.real_calloc) DetourDetach(reinterpret_cast<PVOID*>(&slot.real_calloc),
                                       reinterpret_cast<PVOID>(hooked_calloc<Slot>));
    if (slot.real_realloc) DetourDetach(reinterpret_cast<PVOID*>(&slot.real_realloc),
                                        reinterpret_cast<PVOID>(hooked_realloc<Slot>));
    if (slot.real_free) DetourDetach(reinterpret_cast<PVOID*>(&slot.real_free),
                                     reinterpret_cast<PVOID>(hooked_free<Slot>));
    if (slot.real_msize) DetourDetach(reinterpret_cast<PVOID*>(&slot.real_msize),
                                      reinterpret_cast<PVOID>(hooked_msize<Slot>));
}

bool fill_slot(CrtSlot& slot, const char* module_name) {
    HMODULE module = GetModuleHandleA(module_name);
    if (!module) return false;

    auto resolve = [module](const char* name) {
        return reinterpret_cast<void*>(GetProcAddress(module, name));
    };

    void* m = resolve("malloc");
    void* f = resolve("free");
    if (!m || !f) return false; // a CRT without both is not one we can use

    slot.module_name = module_name;
    slot.module = module;
    slot.real_malloc = reinterpret_cast<decltype(slot.real_malloc)>(m);
    slot.real_free = reinterpret_cast<decltype(slot.real_free)>(f);
    slot.real_calloc = reinterpret_cast<decltype(slot.real_calloc)>(resolve("calloc"));
    slot.real_realloc = reinterpret_cast<decltype(slot.real_realloc)>(resolve("realloc"));
    slot.real_msize = reinterpret_cast<decltype(slot.real_msize)>(resolve("_msize"));
    return true;
}

// Ordered so the module a program most likely allocates from is found first.
// Only modules already loaded are considered: forcing a CRT into the process
// that it does not otherwise use would hook a heap nothing allocates from.
const char* const kCrtModules[] = {
    "ucrtbase.dll",                 // MSVC / modern Windows CRT
    "msvcrt.dll",                   // MSYS2 and MinGW builds, GIMP's among them
    "api-ms-win-crt-heap-l1-1-0.dll",
    "msvcr120.dll",
};

std::mutex g_hook_mutex;
bool g_slot_taken[kMaxSlots] = {false, false, false, false};

// Attaches whichever of the listed CRTs are loaded and not already hooked.
//
// Run more than once on purpose. At DLL_PROCESS_ATTACH the only CRT in the
// process is generally this DLL's own: the target's is pulled in later, by its
// own imports. GIMP's Windows builds come from MSYS2 and use msvcrt.dll, which
// is usually one of those later loads, so hooking once at attach time would
// miss the very allocator the demo depends on. The LoadLibraryExW hook below
// calls back here after each successful load.
bool scan_and_attach() {
    std::lock_guard<std::mutex> lock(g_hook_mutex);

    int first_new = g_slot_count;
    for (const char* name : kCrtModules) {
        if (g_slot_count >= kMaxSlots) break;

        // Skip a module already occupying a slot.
        bool already = false;
        for (int i = 0; i < g_slot_count; ++i) {
            if (g_slots[i].module_name && std::strcmp(g_slots[i].module_name, name) == 0) {
                already = true;
                break;
            }
        }
        if (already) continue;

        if (fill_slot(g_slots[g_slot_count], name)) {
            log_line("found CRT %s", name);
            ++g_slot_count;
        }
    }

    if (g_slot_count == first_new) {
        return false; // nothing new this time round
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    bool any = false;
    // Each slot is attached exactly once; re-attaching would chain a detour
    // onto its own trampoline and recurse.
    if (g_slot_count > 0 && !g_slot_taken[0]) { any |= attach_slot<0>(); g_slot_taken[0] = true; }
    if (g_slot_count > 1 && !g_slot_taken[1]) { any |= attach_slot<1>(); g_slot_taken[1] = true; }
    if (g_slot_count > 2 && !g_slot_taken[2]) { any |= attach_slot<2>(); g_slot_taken[2] = true; }
    if (g_slot_count > 3 && !g_slot_taken[3]) { any |= attach_slot<3>(); g_slot_taken[3] = true; }

    const LONG rc = DetourTransactionCommit();
    if (rc != NO_ERROR) {
        log_line("DetourTransactionCommit failed: %ld", rc);
        return false;
    }

    log_line("hooks now cover %d CRT module(s), mode=%s, threshold=%zu bytes",
             g_slot_count, g_config.remote_mode ? "remote" : "log", g_config.threshold);
    return any;
}

// LoadLibraryExW is hooked purely as a trigger for the rescan above.
HMODULE (WINAPI* g_real_load_library_ex_w)(LPCWSTR, HANDLE, DWORD) = nullptr;

HMODULE WINAPI hooked_load_library_ex_w(LPCWSTR name, HANDLE file, DWORD flags) {
    HMODULE module = g_real_load_library_ex_w(name, file, flags);
    if (module) {
        // DONT_RESOLVE / AS_DATAFILE loads are not executable images, so nothing
        // will ever call through them.
        const DWORD data_only = LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE |
                                LOAD_LIBRARY_AS_IMAGE_RESOURCE;
        if ((flags & data_only) == 0 && !should_pass_through()) {
            const Reentry guard;
            scan_and_attach();
        }
    }
    return module;
}

bool install_hooks() {
    if (HMODULE kernel32 = GetModuleHandleA("kernel32.dll")) {
        g_real_load_library_ex_w = reinterpret_cast<decltype(g_real_load_library_ex_w)>(
            reinterpret_cast<void*>(GetProcAddress(kernel32, "LoadLibraryExW")));
    }

    if (g_real_load_library_ex_w) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(reinterpret_cast<PVOID*>(&g_real_load_library_ex_w),
                     reinterpret_cast<PVOID>(hooked_load_library_ex_w));
        if (DetourTransactionCommit() != NO_ERROR) {
            log_line("could not hook LoadLibraryExW; a CRT loaded later will be missed");
            g_real_load_library_ex_w = nullptr;
        }
    } else {
        log_line("LoadLibraryExW not found; a CRT loaded later will be missed");
    }

    const bool any = scan_and_attach();
    if (g_slot_count == 0) {
        log_line("no C runtime loaded yet; waiting for one to appear");
    }
    return any;
}

void remove_hooks() {
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (g_slot_count > 0) detach_slot<0>();
    if (g_slot_count > 1) detach_slot<1>();
    if (g_slot_count > 2) detach_slot<2>();
    if (g_slot_count > 3) detach_slot<3>();
    DetourTransactionCommit();
}

void report() {
    log_line("--- summary ---");
    log_line("allocations at or above the threshold: %llu, totalling %llu bytes",
             static_cast<unsigned long long>(g_large_seen.load()),
             static_cast<unsigned long long>(g_large_bytes.load()));
    log_line("redirected to the peer: %llu, totalling %llu bytes",
             static_cast<unsigned long long>(g_redirected.load()),
             static_cast<unsigned long long>(g_redirected_bytes.load()));
    log_line("passed straight through: %llu",
             static_cast<unsigned long long>(g_passed_through.load()));

    if (g_large_seen.load() == 0) {
        log_line("NOTE: not one allocation reached the threshold. The host is not getting "
                 "its large buffers from the C runtime -- check it is running with "
                 "G_SLICE=always-malloc, and expect to need a different interception "
                 "point if that does not change this number.");
    }

    if (g_state.ready.load() && g_state.heap) {
        const auto s = g_state.heap->stats();
        log_line("heap: %zu bytes resident, %zu fetches, %zu flushes, %zu evictions",
                 s.resident_bytes, s.fetches, s.flushes, s.evictions);
    }
}

} // namespace

BOOL APIENTRY DllMain(HMODULE /*module*/, DWORD reason, LPVOID reserved) {
    if (DetourIsHelperProcess()) {
        // rundll32 running the Detours helper: do nothing at all.
        return TRUE;
    }

    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        // Restores the target's import table, which DetourCreateProcessWithDllEx
        // rewrote to get this DLL loaded.
        DetourRestoreAfterWith();

        load_config();

        // fopen is safe under the loader lock; sockets and threads are not,
        // which is why the client is built lazily on first use instead.
        g_log = std::fopen(g_config.log_path.c_str(), "w");
        log_line("meminfo_hook attached, mode=%s",
                 g_config.remote_mode ? "remote" : "log");

        install_hooks();
        break;
    }
    case DLL_PROCESS_DETACH: {
        report();

        // Deliberately not torn down here.
        //
        // ~MemoryClient joins its network and discovery threads, and joining a
        // thread from DllMain deadlocks: this code holds the loader lock, and
        // an exiting thread needs that same lock to run its own DLL_THREAD_DETACH
        // notifications. The process is going away regardless, and the kernel
        // reclaims the sockets, threads and address space, so leaking here is
        // both correct and the only safe option.
        (void)g_state.arena.release();
        (void)g_state.heap.release();
        (void)g_state.client.release();

        // reserved is non-null when the whole process is exiting rather than
        // this DLL being unloaded on its own. Nothing will call the detoured
        // functions again, and unwinding the transaction at that point risks
        // more than it saves.
        if (reserved == nullptr) {
            remove_hooks();
        }

        if (g_log) {
            std::fclose(g_log);
            g_log = nullptr;
        }
        break;
    }
    default:
        break;
    }
    return TRUE;
}
