# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(EveryTag)

# C sources (original + glue)
target_sources(app PRIVATE
    src/main.c
    src/settings.c
    src/bq2512x.c
    src/watchdog.c
    src/lis2dw12.c
)

# C++ sources (Phase 1-3 stubs, populated incrementally)
target_sources(app PRIVATE
    src/beacon_logic.cpp
    src/accel_data.cpp
    src/beacon_config.cpp
)

# Relax warnings on existing C code (pre-existing issues, will be replaced)
target_compile_options(app PRIVATE
    $<$<COMPILE_LANGUAGE:C>:
        -Wno-incompatible-pointer-types
    >
)

# C++ compiler flags
target_compile_options(app PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:
        -Wall -Wextra -Werror -Wpedantic
        -Wconversion -Wsign-conversion -Wcast-align
        -fno-threadsafe-statics -fno-use-cxa-atexit
    >
)
