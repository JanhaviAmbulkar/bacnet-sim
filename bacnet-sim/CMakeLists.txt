cmake_minimum_required(VERSION 3.16)
project(bacnet_sim LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra)
endif()

find_package(Threads REQUIRED)

# Plain-C helper library
add_library(bytebuf STATIC c/bytebuf.c)
target_include_directories(bytebuf PUBLIC c)

# C++ core: codec, sockets, device model, IPC
add_library(bacnet_core STATIC src/bacnet.cpp src/net.cpp src/device.cpp src/ipc.cpp)
target_include_directories(bacnet_core PUBLIC include)
target_link_libraries(bacnet_core PUBLIC bytebuf Threads::Threads)
if(WIN32)
  target_link_libraries(bacnet_core PUBLIC ws2_32)
endif()

add_executable(bacnet_sim src/simulator.cpp)
add_executable(bacnet_client src/client.cpp)
add_executable(bacnet_logger src/logger.cpp)
target_link_libraries(bacnet_sim PRIVATE bacnet_core)
target_link_libraries(bacnet_client PRIVATE bacnet_core)
target_link_libraries(bacnet_logger PRIVATE bacnet_core)

enable_testing()
add_executable(test_codec tests/test_codec.cpp)
target_link_libraries(test_codec PRIVATE bacnet_core)
add_test(NAME codec_unit_tests COMMAND test_codec)
