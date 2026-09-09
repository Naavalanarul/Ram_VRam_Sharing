project(integration)

add_executable(test_cluster test_cluster.cpp)
target_link_libraries(test_cluster PRIVATE GTest::gtest_main GTest::gtest)

add_test(NAME test_cluster COMMAND test_cluster)
