# Build & Test Commands

## Prerequisites
Run `nix develop` to enter the dev shell with all dependencies.
Run `cd .. && west init -l Everytag && west update --narrow -o=--depth=1` to fetch Zephyr + NCS modules.

## Flake apps (no nix develop needed)
nix build .#firmware  # pure cross-compile for nrf52810 (no west setup)
nix run .#test        # host-native tests (266 assertions, ASan/UBSan)
nix run .#lint        # clang-format check (dry-run, fails on diff)
nix run .#format      # auto-format C++ sources

## Cross-compile check (verify firmware compiles for real boards)
west build --board kkm_p1_nrf52810 -d build-810 --pristine --no-sysbuild -- -DBOARD_ROOT=$(pwd)

## Host-native tests (macOS/Linux, ASan/UBSan, no Zephyr dependency)
cd tests/host && cmake -B build && cmake --build build && ./build/host_tests

## C++ quality checks
clang-format --dry-run --Werror src/*.cpp src/*.hpp
clang-tidy src/*.cpp -p tests/host/build

## Binary size (nrf52810)
# C baseline: text=151968, data=2872, bss=19930
# C++ migration: text=152340 (+372 bytes, +0.2%)
# C++ overhaul:  text=152920 (+952 bytes, +0.6%)

## Supported boards
# Custom boards (in boards/arm/, need -DBOARD_ROOT=$(pwd)):
#   kkm_p1_nrf52810, kkm_p11_nrf52810, kkm_k4p, wb_20241125, hcbb22e, nrf52805_evm
# Zephyr built-in boards (no BOARD_ROOT needed):
#   nrf52dk/nrf52832, nrf52840dk/nrf52840, nrf52833dk/nrf52833, thingy52/nrf52832
# Not yet working: nrf54l15dk/nrf54l15/cpuapp (watchdog DTS issue)

## Architecture (C++ migration)
# src/beacon_logic.hpp/cpp   — pure functions (MAC, adv template, status byte via StatusFlags)
# src/accel_data.hpp/cpp     — MovementTracker class (circular buffer)
# src/beacon_config.hpp/cpp  — BeaconConfig (narrowed types), SettingsManager (validated setters), GATT validators
# src/beacon_state.hpp/cpp   — StateMachine with IHardware interface, WhatInStatus enum
# src/ihardware.hpp          — hardware abstraction (virtual interface, 29 methods)
# src/zephyr_hardware.cpp    — IHardware impl wrapping Zephyr APIs
# src/zephyr_nvs.cpp         — INvsStorage impl wrapping NVS
# src/gatt_glue.c            — BT_GATT_SERVICE_DEFINE (C99, can't be C++)
# src/ble_glue.c             — BT_DATA_BYTES arrays (C99, can't be C++)
# src/main.cpp               — static object graph, event loop

## west2nix lockfile
# west2nix.toml pins 51 NCS/Zephyr projects with Nix hashes.
# To regenerate after changing west.yml:
#   cd .. && west update && cd Everytag
#   uv run scripts/west2nix.py -o west2nix.toml
