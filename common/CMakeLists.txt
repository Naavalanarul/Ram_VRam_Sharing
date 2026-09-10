# common/CMakeLists.txt
# Shared utilities library for MemInfo

# ──────────────────────────────────────────────
# FlatBuffers schema compilation
# ──────────────────────────────────────────────
flatbuffers_generate_headers(
    TARGET meminfo_schemas
    SCHEMAS
        ${CMAKE_CURRENT_SOURCE_DIR}/schemas/discovery.fbs
        ${CMAKE_CURRENT_SOURCE_DIR}/schemas/memory.fbs
        ${CMAKE_CURRENT_SOURCE_DIR}/schemas/gpu.fbs
        ${CMAKE_CURRENT_SOURCE_DIR}/schemas/control.fbs
    OUTPUT_DIR
        ${CMAKE_CURRENT_BINARY_DIR}/generated
)

# ──────────────────────────────────────────────
# Common library
# ──────────────────────────────────────────────
add_library(meminfo_common STATIC
    src/uuid.cpp
    src/crc32c.cpp
    src/config.cpp
    src/logging.cpp
    src/signal_handler.cpp
    src/http_status.cpp
)

target_include_directories(meminfo_common
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(meminfo_common
    PUBLIC
        meminfo_schemas
        uv_a
        spdlog::spdlog
        tomlplusplus::tomlplusplus
        flatbuffers
        meminfo_platform
)

meminfo_set_strict_warnings(meminfo_common)

# SSE 4.2 for CRC32C hardware acceleration (x86 only).
#
# -msse4.2 is a GCC/Clang flag and is meaningless off x86. Clang on arm64 only
# warns that the argument is unused, which the plain check below accepted --
# and then -Werror turned that same warning into an error in the real build.
# So restrict the flag to x86 targets and make the probe itself strict.
# MSVC has no such flag (SSE4.2 needs none on x64) and never defines
# __SSE4_2__, so crc32c.cpp uses its software path there.
if(NOT MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|i[3-6]86|x86)$")
    include(CheckCXXCompilerFlag)
    set(_meminfo_saved_required_flags "${CMAKE_REQUIRED_FLAGS}")
    # Without -Werror an "unused argument" warning still counts as success.
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -Werror")
    check_cxx_compiler_flag("-msse4.2" COMPILER_HAS_SSE42)
    set(CMAKE_REQUIRED_FLAGS "${_meminfo_saved_required_flags}")

    if(COMPILER_HAS_SSE42)
        set_source_files_properties(src/crc32c.cpp PROPERTIES COMPILE_FLAGS "-msse4.2")
    endif()
endif()

# ──────────────────────────────────────────────
# Tests
# ──────────────────────────────────────────────
if(MEMINFO_BUILD_TESTS)
    add_executable(test_common
        tests/test_crc32c.cpp
        tests/test_uuid.cpp
        tests/test_config.cpp
        tests/test_protocol_version.cpp
    )

    target_link_libraries(test_common
        PRIVATE
            meminfo_common
            GTest::gtest
            GTest::gtest_main
    )

    meminfo_set_strict_warnings(test_common)
    gtest_discover_tests(test_common)
endif()
