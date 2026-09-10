project(integration)

add_executable(test_cluster test_cluster.cpp)

# libuv gives the harness portable process spawn/kill; meminfo_common carries it.
target_link_libraries(test_cluster PRIVATE meminfo_common GTest::gtest_main GTest::gtest)

# $<TARGET_FILE:...> resolves to each binary's real path, so the per-config
# subdirectory of a multi-config generator and the .exe suffix are handled by
# CMake rather than assumed by the test.
target_compile_definitions(test_cluster PRIVATE
    MEMINFO_DISCOVERYD_EXE="$<TARGET_FILE:discoveryd>"
    MEMINFO_MEMORYD_EXE="$<TARGET_FILE:memoryd>"
    MEMINFO_GPUD_EXE="$<TARGET_FILE:gpud>"
    MEMINFO_MEMCLIENT_CLI_EXE="$<TARGET_FILE:memclient_cli>"
)

# The daemons are launched by the test, so they must exist before it runs.
add_dependencies(test_cluster discoveryd memoryd gpud memclient_cli)

meminfo_set_strict_warnings(test_cluster)

add_test(NAME test_cluster COMMAND test_cluster)
