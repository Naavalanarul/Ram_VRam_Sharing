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
# FlatBuffers schema compilation helper
# ──────────────────────────────────────────────
include(cmake/FlatBuffersCompile.cmake)

# ──────────────────────────────────────────────
# Strict compiler warnings — applied only to our
# own targets via this helper function.
# ──────────────────────────────────────────────
function(meminfo_set_strict_warnings target)
    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
    )
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

if(MEMINFO_BUILD_TESTS)
    add_subdirectory(integration)
endif()
