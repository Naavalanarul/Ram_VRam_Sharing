# tools/CMakeLists.txt
# Hand-run diagnostics, not part of the automated suite.

add_executable(remote_heap_probe remote_heap_probe.cpp)

target_link_libraries(remote_heap_probe
    PRIVATE
        memclient_lib
        CLI11::CLI11
)

if(WIN32)
    # GetProcessMemoryInfo, for the working-set figure the probe prints.
    target_link_libraries(remote_heap_probe PRIVATE psapi)
endif()

meminfo_set_strict_warnings(remote_heap_probe)

# ──────────────────────────────────────────────
# Phase 0 validation probe (Windows only)
#
# Standalone by design -- it links nothing from this project, so it still
# answers "does the technique work on this machine?" even if everything else
# fails to build.
# ──────────────────────────────────────────────
if(WIN32)
    add_executable(win_veh_probe win_veh_probe.cpp)
    meminfo_set_strict_warnings(win_veh_probe)
endif()
