// Phase 0, step 2: does the vectored-exception technique actually work here?
//
// Everything in this project's Windows path rests on one assumption -- that a
// PAGE_NOACCESS page can be touched, trapped by a vectored exception handler,
// made accessible, and the faulting instruction resumed. If that does not hold
// on the demo machine (a security product hooking exception dispatch, a
// hardened CFG policy, a debugger intercepting first-chance exceptions), then
// nothing above it can work either, and it is worth finding out in ten seconds
// rather than on stage.
//
// Deliberately standalone: no project headers, no dependencies beyond the Win32
// API, so it can also be compiled by hand:
//
//   cl /EHsc /W4 win_veh_probe.cpp
//
// Exit code 0 means the technique works. Anything else means stop and pick the
// fallback target before investing further.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

void* g_region = nullptr;
size_t g_region_size = 0;
volatile LONG g_read_faults = 0;
volatile LONG g_write_faults = 0;

LONG WINAPI probe_handler(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // [0] is 0 for a read, 1 for a write, 8 for a DEP violation.
    // [1] is the address that could not be accessed.
    const bool is_write = record->ExceptionInformation[0] == 1;
    auto* fault_addr = reinterpret_cast<uint8_t*>(record->ExceptionInformation[1]);

    auto* base = static_cast<uint8_t*>(g_region);
    if (!base || fault_addr < base || fault_addr >= base + g_region_size) {
        return EXCEPTION_CONTINUE_SEARCH; // a real crash somewhere else
    }

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const size_t page_size = si.dwPageSize;
    auto* page = reinterpret_cast<uint8_t*>(
        reinterpret_cast<uintptr_t>(fault_addr) & ~static_cast<uintptr_t>(page_size - 1));

    if (is_write) {
        InterlockedIncrement(&g_write_faults);
        DWORD old = 0;
        if (!VirtualProtect(page, page_size, PAGE_READWRITE, &old)) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
    } else {
        InterlockedIncrement(&g_read_faults);
        DWORD old = 0;
        // Writable first, so the fill below does not itself fault straight back
        // into this handler, then read-only so the next *write* faults again.
        // That second fault is the whole basis of dirty-page tracking.
        if (!VirtualProtect(page, page_size, PAGE_READWRITE, &old)) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        std::memset(page, 0xAB, page_size);
        if (!VirtualProtect(page, page_size, PAGE_READONLY, &old)) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }

    return EXCEPTION_CONTINUE_EXECUTION;
}

bool check(bool condition, const char* what) {
    std::printf("  [%s] %s\n", condition ? "ok  " : "FAIL", what);
    return condition;
}

} // namespace

int main() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const size_t page_size = si.dwPageSize;
    std::printf("Page size: %zu bytes\n\n", page_size);

    bool all_ok = true;

    PVOID handle = AddVectoredExceptionHandler(1, probe_handler);
    all_ok &= check(handle != nullptr, "AddVectoredExceptionHandler installed a handler");
    if (!handle) {
        std::printf("\nFAILED: no vectored handler, nothing else can be tested.\n");
        return 1;
    }

    // MEM_RESERVE only, mirroring what the real backend does: no commit charge
    // until a page is actually needed.
    g_region_size = page_size * 4;
    g_region = VirtualAlloc(nullptr, g_region_size, MEM_RESERVE, PAGE_NOACCESS);
    all_ok &= check(g_region != nullptr, "VirtualAlloc reserved address space");
    if (!g_region) {
        RemoveVectoredExceptionHandler(handle);
        return 1;
    }

    // A reserved page must be committed before it can be protected or written,
    // which is what the fault handler would normally do. Commit here so the
    // protection transitions below are the thing under test.
    all_ok &= check(VirtualAlloc(g_region, g_region_size, MEM_COMMIT, PAGE_NOACCESS) != nullptr,
                    "VirtualAlloc committed the pages as PAGE_NOACCESS");

    auto* bytes = static_cast<volatile uint8_t*>(g_region);

    // 1. A read of an inaccessible page must trap and resume.
    const uint8_t observed = bytes[0];
    all_ok &= check(g_read_faults == 1, "a read of a PAGE_NOACCESS page reached the handler");
    all_ok &= check(observed == 0xAB, "the faulting read resumed and saw what the handler wrote");

    // 2. A write to that now read-only page must trap again. This is the part
    //    that makes dirty tracking possible; if it does not happen, writes are
    //    invisible and evicting a page would silently discard them.
    bytes[1] = 0x5A;
    all_ok &= check(g_write_faults == 1, "a write to a PAGE_READONLY page reached the handler");
    all_ok &= check(bytes[1] == 0x5A, "the faulting write resumed and stuck");

    // 3. A second write to the same page must NOT trap: it is writable now.
    const LONG writes_before = g_write_faults;
    bytes[2] = 0x77;
    all_ok &= check(g_write_faults == writes_before,
                    "a second write to the same page did not fault again");

    // 4. Decommitting must hand the pages back and re-arm the trap. This is
    //    what makes the process's memory figure actually fall.
    all_ok &= check(VirtualFree(g_region, page_size, MEM_DECOMMIT) != 0,
                    "VirtualFree(MEM_DECOMMIT) released the page");
    all_ok &= check(VirtualAlloc(g_region, page_size, MEM_COMMIT, PAGE_NOACCESS) != nullptr,
                    "the decommitted page could be committed again");
    const LONG reads_before = g_read_faults;
    const uint8_t after_evict = bytes[0];
    all_ok &= check(g_read_faults == reads_before + 1,
                    "touching the re-armed page faulted again");
    all_ok &= check(after_evict == 0xAB, "the refilled page read back correctly");

    VirtualFree(g_region, 0, MEM_RELEASE);
    RemoveVectoredExceptionHandler(handle);

    std::printf("\nread faults: %ld, write faults: %ld\n", g_read_faults, g_write_faults);
    if (all_ok) {
        std::printf("\nOK: vectored exception handling, protection changes and decommit\n"
                    "all behave as the Windows backend needs. Phase 0 step 2 passes.\n");
        return 0;
    }

    std::printf("\nFAILED: this machine does not support the technique the Windows backend\n"
                "relies on. Do not proceed to Phase 2 here -- find out what is intercepting\n"
                "exception dispatch (security software is the usual cause) or use another\n"
                "machine.\n");
    return 1;
}
