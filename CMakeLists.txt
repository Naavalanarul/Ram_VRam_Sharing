cmake_minimum_required(VERSION 3.16)
project(MemInfo VERSION 0.1.0 LANGUAGES C CXX)

# ──────────────────────────────────────────────
# C++ Standard
# ──────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ──────────────────────────────────────────────
# Build Options
# ──────────────────────────────────────────────
option(MEMINFO_BUILD_TESTS "Build unit tests" ON)
option(MEMINFO_HAVE_CUDA "Build with CUDA support (requires CUDA toolkit)" OFF)

# ──────────────────────────────────────────────
# Out-of-source build enforcement
# ──────────────────────────────────────────────
if("${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_BINARY_DIR}")
    message(FATAL_ERROR "In-source builds are not allowed. Use an out-of-source build directory.")
endif()

# ──────────────────────────────────────────────
# Dependencies (via FetchContent)
# NOTE: Fetched BEFORE we set strict warnings so
#       third-party code is not affected by -Werror
# ──────────────────────────────────────────────
include(cmake/Dependencies.cmake)

# ──────────────────────────────────────────────
# Windows: suppress the min/max macros
#
# libuv pulls in <windows.h>, which defines min() and max() as macros unless
# NOMINMAX is set. Those macros break every std::numeric_limits<T>::max() call
# in the headers we include -- flatbuffers' FLATBUFFERS_MAX_BUFFER_SIZE among
# them -- with a confusing "illegal token on right side of '::'".
#
# Declared after the dependency fetch above so it applies to our own targets
# and leaves the third-party ones exactly as they were.
# ──────────────────────────────────────────────
if(WIN32)
    add_compile_definitions(NOMINMAX)
endif()

# ──────────────────────────────────────────────
# FlatBuffers schema compilation helper
# ──────────────────────────────────────────────
include(cmake/FlatBuffersCompile.cmake)

# ──────────────────────────────────────────────
# Strict compiler warnings — applied only to our
# own targets via this helper function.
# ──────────────────────────────────────────────
# -Wextra/-Wpedantic/-Werror are GCC/Clang spellings; MSVC only warns that it is
# ignoring them (D9002), so it would silently build without strict warnings.
function(meminfo_set_strict_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Werror
        )
    endif()
endfunction()

# ──────────────────────────────────────────────
# Testing
# ──────────────────────────────────────────────
if(MEMINFO_BUILD_TESTS)
    enable_testing()
    include(GoogleTest)
endif()

# ──────────────────────────────────────────────
# Subdirectories
# ──────────────────────────────────────────────
add_subdirectory(common)
add_subdirectory(platform)
add_subdirectory(discoveryd)
add_subdirectory(memoryd)
add_subdirectory(memclient)
add_subdirectory(gpud)
add_subdirectory(tools)

# Windows-only malloc interposer; the subdirectory is a no-op elsewhere.
add_subdirectory(hook)

if(MEMINFO_BUILD_TESTS)
    add_subdirectory(integration)
endif()
