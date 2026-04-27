# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(MockingBeacon)

# C sources (hardware drivers + GATT/BLE glue)
target_sources(app PRIVATE
    src/bq2512x.c
    src/watchdog.c
    src/lis2dw12.c
    src/gatt_glue.c
    src/ble_glue.c
)

# C++ sources
target_sources(app PRIVATE
    src/main.cpp
    src/beacon_logic.cpp
    src/accel_data.cpp
    src/beacon_config.cpp
    src/beacon_state.cpp
    src/zephyr_nvs.cpp
    src/zephyr_hardware.cpp
)

# Relax warnings on existing C code (pre-existing issues, will be replaced)
target_compile_options(app PRIVATE
    $<$<COMPILE_LANGUAGE:C>:
        -Wno-incompatible-pointer-types
    >
)

# C++ compiler flags
# Note: -Wpedantic omitted because Zephyr headers use C99 compound literals
# and named variadic macros which are invalid in strict C++.
# -Wcast-align, -Wconversion, -Wsign-conversion relaxed for Zephyr internal macros.
# -Wno-unused-parameter: Zephyr's flash.h:113 (flash_params_get_erase_cap)
# has a static inline whose body conditionally uses parameter `p` — when
# only FLASH_HAS_NO_EXPLICIT_ERASE is set (RRAM-only boards like the
# Holyiot 25055/nRF54L05, which have no external NOR flash), the fallback
# branch doesn't reference `p`. Treating that as a project-level error
# would block all RRAM-only board ports.
target_compile_options(app PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:
        -Wall -Wextra -Werror
        -Wno-missing-field-initializers
        -Wno-unused-parameter
        -fno-threadsafe-statics -fno-use-cxa-atexit
    >
)
