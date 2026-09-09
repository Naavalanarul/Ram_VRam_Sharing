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
)

meminfo_set_strict_warnings(meminfo_common)

# SSE 4.2 for CRC32C hardware acceleration (x86_64 only)
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-msse4.2" COMPILER_HAS_SSE42)
if(COMPILER_HAS_SSE42)
    set_source_files_properties(src/crc32c.cpp PROPERTIES COMPILE_FLAGS "-msse4.2")
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
