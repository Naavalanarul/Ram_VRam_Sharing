project(memclient)

# Header-only cache and client lib
add_library(memclient_lib STATIC
    src/lru_cache.cpp
    src/client_api.cpp
)

target_include_directories(memclient_lib PUBLIC include)
target_link_libraries(memclient_lib PUBLIC meminfo_common meminfo_platform)
meminfo_set_strict_warnings(memclient_lib)

# CLI
add_executable(memclient_cli src/memclient_cli.cpp)
target_link_libraries(memclient_cli PRIVATE memclient_lib CLI11::CLI11)
meminfo_set_strict_warnings(memclient_cli)

# Tests
if(MEMINFO_BUILD_TESTS)
    add_executable(test_memclient
        tests/test_lru_cache.cpp
        tests/test_client_api.cpp
    )
    target_link_libraries(test_memclient PRIVATE memclient_lib memoryd_lib GTest::gtest_main GTest::gtest GTest::gmock)
    meminfo_set_strict_warnings(test_memclient)

    # Registers one ctest entry per GoogleTest case, as every other test target
    # here does, so a failure names the case instead of just the binary.
    gtest_discover_tests(test_memclient)
endif()
