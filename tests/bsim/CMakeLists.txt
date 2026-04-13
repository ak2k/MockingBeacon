# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(everytag_bsim_test)

# BabbleSim test framework
add_subdirectory(${ZEPHYR_BASE}/tests/bsim/babblekit babblekit)
target_link_libraries(app PRIVATE babblekit)

zephyr_include_directories(
    ${BSIM_COMPONENTS_PATH}/libUtilv1/src/
    ${BSIM_COMPONENTS_PATH}/libPhyComv1/src/
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
)

target_sources(app PRIVATE
    src/main.c
    src/advertiser.c
    src/scanner.c
    src/beacon_logic_c_api.cpp
    ../../src/beacon_logic.cpp
)
