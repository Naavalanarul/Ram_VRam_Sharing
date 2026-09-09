# memoryd/CMakeLists.txt
# Remote Memory Server daemon

add_executable(memoryd
    src/main.cpp
    src/memory_daemon.cpp
    src/slab_allocator.cpp
    src/page_tracker.cpp
    src/memory_protocol.cpp
    src/client_session.cpp
)

target_include_directories(memoryd
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(memoryd
    PRIVATE
        meminfo_common
        CLI11::CLI11
)

meminfo_set_strict_warnings(memoryd)

# Library for testing
add_library(memoryd_lib STATIC
    src/memory_daemon.cpp
    src/slab_allocator.cpp
    src/page_tracker.cpp
    src/memory_protocol.cpp
    src/client_session.cpp
)

target_include_directories(memoryd_lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(memoryd_lib
    PUBLIC
        meminfo_common
        CLI11::CLI11
)

meminfo_set_strict_warnings(memoryd_lib)

if(MEMINFO_BUILD_TESTS)
    add_executable(test_memoryd
        tests/test_slab_allocator.cpp
        tests/test_page_tracker.cpp
        tests/test_concurrent_clients.cpp
        tests/test_peer_disconnect.cpp
    )

    target_link_libraries(test_memoryd
        PRIVATE
            memoryd_lib
            GTest::gtest
            GTest::gtest_main
            GTest::gmock
    )

    meminfo_set_strict_warnings(test_memoryd)
    gtest_discover_tests(test_memoryd)
endif()
