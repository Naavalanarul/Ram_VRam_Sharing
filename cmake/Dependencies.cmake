# cmake/Dependencies.cmake
# All third-party dependencies fetched via FetchContent.
# No system-wide install assumptions.

include(FetchContent)

# ──────────────────────────────────────────────
# libuv — event loop and networking
# ──────────────────────────────────────────────
set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_BENCH OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    libuv
    GIT_REPOSITORY https://github.com/libuv/libuv.git
    GIT_TAG        v1.50.0
    GIT_SHALLOW    TRUE
)

# ──────────────────────────────────────────────
# FlatBuffers — serialization
# ──────────────────────────────────────────────
set(FLATBUFFERS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_FLATC ON CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_FLATHASH OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    flatbuffers
    GIT_REPOSITORY https://github.com/google/flatbuffers.git
    GIT_TAG        v24.3.25
    GIT_SHALLOW    TRUE
)

# ──────────────────────────────────────────────
# spdlog — structured logging
# ──────────────────────────────────────────────
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.15.1
    GIT_SHALLOW    TRUE
)

# ──────────────────────────────────────────────
# CLI11 — command-line parsing (header-only)
# ──────────────────────────────────────────────
set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_DOCS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG        v2.4.2
    GIT_SHALLOW    TRUE
)

# ──────────────────────────────────────────────
# toml++ — TOML config parsing (header-only)
# ──────────────────────────────────────────────
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
    GIT_SHALLOW    TRUE
)

# ──────────────────────────────────────────────
# GoogleTest — testing framework
# ──────────────────────────────────────────────
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK ON CACHE BOOL "" FORCE)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE
)

# ──────────────────────────────────────────────
# Fetch all dependencies
# ──────────────────────────────────────────────
FetchContent_MakeAvailable(
    libuv
    flatbuffers
    spdlog
    CLI11
    tomlplusplus
    googletest
)
