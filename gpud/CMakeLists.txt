project(gpud)

# Find CUDA Toolkit
find_package(CUDAToolkit QUIET)

set(CUDA_EXECUTOR_SRC src/cuda_executor_stub.cpp)
if(CUDAToolkit_FOUND)
    message(STATUS "CUDA Toolkit found, compiling real executor.")
    list(APPEND CUDA_EXECUTOR_SRC src/cuda_executor_real.cpp)
    add_compile_definitions(MEMINFO_ENABLE_CUDA)
else()
    message(STATUS "CUDA Toolkit NOT found, compiling stub executor only.")
    # create_real_cuda_executor will return nullptr in stub file by fallback compilation.
    list(APPEND CUDA_EXECUTOR_SRC src/cuda_executor_real.cpp)
endif()

# Library containing core logic
add_library(gpud_lib STATIC
    src/gpu_session.cpp
    src/gpu_daemon.cpp
    ${CUDA_EXECUTOR_SRC}
)

target_include_directories(gpud_lib PUBLIC include)
target_link_libraries(gpud_lib PUBLIC meminfo_common)

if(CUDAToolkit_FOUND)
    target_link_libraries(gpud_lib PUBLIC CUDA::cudart)
endif()

meminfo_set_strict_warnings(gpud_lib)

# Daemon executable
add_executable(gpud src/main.cpp)
target_link_libraries(gpud PRIVATE gpud_lib CLI11::CLI11)
meminfo_set_strict_warnings(gpud)

# GPU Client library
add_library(gpu_client_lib STATIC src/gpu_client.cpp)
target_include_directories(gpu_client_lib PUBLIC include)
target_link_libraries(gpu_client_lib PUBLIC meminfo_common)
meminfo_set_strict_warnings(gpu_client_lib)

# Tests
if(MEMINFO_BUILD_TESTS)
    add_executable(test_gpud tests/test_gpu_protocol.cpp)
    target_link_libraries(test_gpud PRIVATE gpud_lib gpu_client_lib GTest::gtest_main GTest::gtest GTest::gmock)
    meminfo_set_strict_warnings(test_gpud)
    gtest_discover_tests(test_gpud)
endif()
