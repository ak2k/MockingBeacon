cmake_minimum_required(VERSION 3.20)
project(everytag_fuzz CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_COMPILER clang++)

# libFuzzer + sanitizers
add_compile_options(-fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
add_link_options(-fsanitize=fuzzer,address,undefined)

add_compile_options(-Wall -Wextra -Werror)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../src)

set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../src)

# Each target fuzzes one family of GATT validators.
# These are pure functions with no Zephyr dependency.

add_executable(fuzz_validate_field
    fuzz_validate_field.cpp
    ${SRC_DIR}/beacon_config.cpp
    ${SRC_DIR}/beacon_logic.cpp
    ${SRC_DIR}/accel_data.cpp
)
target_compile_definitions(fuzz_validate_field PRIVATE HOST_TEST=1)

add_executable(fuzz_auth
    fuzz_auth.cpp
    ${SRC_DIR}/beacon_config.cpp
    ${SRC_DIR}/beacon_logic.cpp
    ${SRC_DIR}/accel_data.cpp
)
target_compile_definitions(fuzz_auth PRIVATE HOST_TEST=1)

add_executable(fuzz_key_chunk
    fuzz_key_chunk.cpp
    ${SRC_DIR}/beacon_config.cpp
    ${SRC_DIR}/beacon_logic.cpp
    ${SRC_DIR}/accel_data.cpp
)
target_compile_definitions(fuzz_key_chunk PRIVATE HOST_TEST=1)
