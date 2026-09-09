project(meminfo_platform)

set(PLATFORM_SRCS)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND PLATFORM_SRCS
        src/linux/memory_monitor_linux.cpp
        src/linux/page_fault_linux.cpp
        src/linux/local_ipc_linux.cpp
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    list(APPEND PLATFORM_SRCS
        src/windows/memory_monitor_win.cpp
        src/windows/page_fault_win.cpp
        src/windows/local_ipc_win.cpp
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    list(APPEND PLATFORM_SRCS
        src/macos/memory_monitor_mac.cpp
        src/macos/page_fault_mac.cpp
        src/macos/local_ipc_mac.cpp
    )
else()
    message(FATAL_ERROR "Unsupported platform for libplatform: ${CMAKE_SYSTEM_NAME}")
endif()

add_library(meminfo_platform STATIC ${PLATFORM_SRCS})
target_include_directories(meminfo_platform PUBLIC include)

# Dependencies
target_link_libraries(meminfo_platform PUBLIC spdlog::spdlog)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(meminfo_platform PRIVATE pthread)
endif()

meminfo_set_strict_warnings(meminfo_platform)

if(MEMINFO_BUILD_TESTS)
    add_executable(test_platform tests/test_platform.cpp)
    target_link_libraries(test_platform PRIVATE meminfo_platform GTest::gtest_main GTest::gtest)
    meminfo_set_strict_warnings(test_platform)
    gtest_discover_tests(test_platform)
endif()
