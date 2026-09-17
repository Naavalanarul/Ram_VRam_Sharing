# hook/CMakeLists.txt
# Malloc interposer and its launcher. Windows only -- Detours is a Windows
# library and the whole mechanism is Win32-specific.

if(NOT WIN32)
    return()
endif()

# ──────────────────────────────────────────────
# Microsoft Detours
#
# Detours ships an nmake makefile rather than a CMake build, so the library is
# declared here from its sources. The five listed below are what an in-process
# hook needs, matching src/Makefile minus its disol* objects.
#
# Two files are deliberately absent:
#   uimports.cpp   is #included by creatwth.cpp, so compiling it separately
#                  would define its symbols twice.
#   disol*.cpp     are one-line wrappers that re-include disasm.cpp under a
#                  DETOURS_*_OFFLINE_LIBRARY define, producing disassemblers for
#                  architectures other than the host. They exist for tools that
#                  inspect foreign binaries; DetourAttach only ever uses the
#                  native disasm.cpp, and nothing here references the offline
#                  entry points.
# ──────────────────────────────────────────────
include(FetchContent)

FetchContent_Declare(
    detours
    GIT_REPOSITORY https://github.com/microsoft/Detours.git
    GIT_TAG        e4bfd6b03e50de46b47abfbd1e46b384f0c5f833 # v4.0.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(detours)

add_library(detours STATIC
    ${detours_SOURCE_DIR}/src/detours.cpp
    ${detours_SOURCE_DIR}/src/modules.cpp
    ${detours_SOURCE_DIR}/src/disasm.cpp
    ${detours_SOURCE_DIR}/src/image.cpp
    ${detours_SOURCE_DIR}/src/creatwth.cpp
)

target_include_directories(detours SYSTEM PUBLIC ${detours_SOURCE_DIR}/src)

# Detours is third-party: its own warnings are not ours to fix, and /WX here
# would fail the build on them. It also needs the CRT's "unsafe" string
# functions, which it uses deliberately.
if(MSVC)
    target_compile_options(detours PRIVATE /W0)
    target_compile_definitions(detours PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ──────────────────────────────────────────────
# The hook DLL, injected into the target process
# ──────────────────────────────────────────────
add_library(meminfo_hook SHARED meminfo_hook.cpp)

target_link_libraries(meminfo_hook
    PRIVATE
        memclient_lib
        detours
)

# Named exactly meminfo_hook.dll: the launcher looks for it beside itself.
set_target_properties(meminfo_hook PROPERTIES OUTPUT_NAME "meminfo_hook" PREFIX "")

meminfo_set_strict_warnings(meminfo_hook)

# The DLL writes its log with fopen/vfprintf, which MSVC reports as deprecated
# (C4996) and which /WX would then turn into a build failure. The secure
# variants buy nothing here: the format strings are all literals in this file
# and the path comes from the operator's own environment.
if(MSVC)
    target_compile_definitions(meminfo_hook PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ──────────────────────────────────────────────
# The launcher
# ──────────────────────────────────────────────
add_executable(meminfo_launch meminfo_launch.cpp)

target_link_libraries(meminfo_launch PRIVATE detours)

meminfo_set_strict_warnings(meminfo_launch)

# The launcher is useless without the DLL, and it looks for it in its own
# directory, so keep them together.
add_dependencies(meminfo_launch meminfo_hook)
set_target_properties(meminfo_hook meminfo_launch PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/hook"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/hook"
)
