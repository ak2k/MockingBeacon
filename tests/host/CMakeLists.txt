cmake_minimum_required(VERSION 3.20)
project(everytag_host_tests CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Sanitizers
add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
add_link_options(-fsanitize=address,undefined)

# Strict warnings for C++
add_compile_options(
    -Wall -Wextra -Werror -Wpedantic
    -Wconversion -Wsign-conversion -Wcast-align
)

# Include the firmware source directory
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../src)

# Source files under test (Phase 1-3 C++ modules)
set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../src)

add_executable(host_tests
    src/test_main.cpp
    ${SRC_DIR}/beacon_logic.cpp
    ${SRC_DIR}/accel_data.cpp
    ${SRC_DIR}/beacon_config.cpp
)

target_compile_definitions(host_tests PRIVATE HOST_TEST=1)
