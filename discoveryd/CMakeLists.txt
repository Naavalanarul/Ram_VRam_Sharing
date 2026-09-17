# discoveryd/CMakeLists.txt
# Discovery + Control Plane daemon

add_executable(discoveryd src/main.cpp)

target_link_libraries(discoveryd
    PRIVATE
        discoveryd_lib
)

meminfo_set_strict_warnings(discoveryd)

# Also build as a library for testing
add_library(discoveryd_lib STATIC
    src/discovery_daemon.cpp
    src/peer_table.cpp
    src/announcer.cpp
    src/listener.cpp
    src/control_socket.cpp
    src/pool_probe.cpp
)

target_include_directories(discoveryd_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(discoveryd_lib
    PUBLIC
        meminfo_common
        meminfo_platform
        CLI11::CLI11
)

meminfo_set_strict_warnings(discoveryd_lib)

if(MEMINFO_BUILD_TESTS)
    add_executable(test_discoveryd
        tests/test_peer_table.cpp
        tests/test_announcer_listener.cpp
        tests/test_control_socket.cpp
        tests/test_multi_node.cpp
    )

    target_link_libraries(test_discoveryd
        PRIVATE
            discoveryd_lib
            GTest::gtest
            GTest::gtest_main
            GTest::gmock
    )

    meminfo_set_strict_warnings(test_discoveryd)
    gtest_discover_tests(test_discoveryd)
endif()
