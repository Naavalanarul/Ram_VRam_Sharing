cmake_minimum_required(VERSION 3.20)
project(WindowsLANSharing VERSION 1.0.0 LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
    add_compile_definitions(NOMINMAX _WIN32_WINNT=0x0A00)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4 /WX /EHsc")
    set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} /W4 /WX")
else()
    message(FATAL_ERROR "This project is Windows-only")
endif()

find_package(CUDA REQUIRED)

add_library(common INTERFACE)
target_include_directories(common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/common)

add_library(paging_wire OBJECT common/PagingWireProtocol.hpp)
add_library(gpu_wire OBJECT common/GpuWireProtocol.hpp)

add_executable(paging_server server/paging_server.cpp)
target_link_libraries(paging_server PRIVATE paging_wire ws2_32)
target_include_directories(paging_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_compile_definitions(paging_server PRIVATE NOMINMAX)

add_executable(gpu_server server/gpu_server.cu)
target_link_libraries(gpu_server PRIVATE gpu_wire ${CUDA_LIBRARIES} ws2_32 cudart_static)
target_include_directories(gpu_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_compile_definitions(gpu_server PRIVATE NOMINMAX)
set_target_properties(gpu_server PROPERTIES CUDA_SEPARABLE_COMPILATION ON)

add_library(remote_heap_client
    client/IPageFaultBackend.hpp
    client/TcpPageFaultBackend.hpp
    client/TcpPageFaultBackend.cpp
    client/RemoteHeap.hpp
    client/RemoteHeap.cpp
    client/DevicePointerTracker.hpp
    client/PagingWireProtocol.hpp
)
target_include_directories(remote_heap_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/client ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_link_libraries(remote_heap_client PRIVATE paging_wire ws2_32)
target_compile_definitions(remote_heap_client PRIVATE NOMINMAX)

add_library(cudart_proxy SHARED
    client/cuda_proxy.cpp
    client/cudart64_12.def
    client/GpuWireProtocol.hpp
    client/DevicePointerTracker.hpp
)
target_include_directories(cudart_proxy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/client ${CMAKE_CURRENT_SOURCE_DIR}/common)
target_link_libraries(cudart_proxy PRIVATE gpu_wire ws2_32)
target_compile_definitions(cudart_proxy PRIVATE NOMINMAX DLL_EXPORT)
set_target_properties(cudart_proxy PROPERTIES
    OUTPUT_NAME "cudart64_12"
    PREFIX ""
    SUFFIX ".dll"
    LINK_FLAGS "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/client/cudart64_12.def"
)

add_executable(win_integration_test tests/win_integration_test.cpp)
target_link_libraries(win_integration_test PRIVATE remote_heap_client cudart_proxy ws2_32 ${CUDA_LIBRARIES} cudart_static)
target_compile_definitions(win_integration_test PRIVATE NOMINMAX)