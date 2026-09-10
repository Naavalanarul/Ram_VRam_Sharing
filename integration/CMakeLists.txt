project(integration)

add_executable(test_cluster test_cluster.cpp)
target_link_libraries(test_cluster PRIVATE GTest::gtest_main GTest::gtest)

# Absolute paths so the test works regardless of ctest's working directory.
target_compile_definitions(test_cluster PRIVATE
    MEMINFO_CLUSTER_SCRIPT="${CMAKE_SOURCE_DIR}/scripts/run_cluster_test.sh"
    MEMINFO_BUILD_DIR="${CMAKE_BINARY_DIR}"
)

meminfo_set_strict_warnings(test_cluster)

add_test(NAME test_cluster COMMAND test_cluster)
