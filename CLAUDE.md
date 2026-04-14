# Build & Test Commands

## Prerequisites
Run `nix develop` to enter the dev shell with all dependencies.
Run `cd .. && west init -l Everytag && west update --narrow -o=--depth=1` to fetch Zephyr + NCS modules.

## Flake apps (no nix develop needed)
nix build .#firmware  # cross-compile all boards in parallel (nrf52810 + nrf54l15)
nix run .#test        # host-native tests (290 assertions, ASan/UBSan)
nix run .#lint        # clang-format check (dry-run, fails on diff)
nix run .#format      # auto-format C++ sources

## Cross-compile check (verify firmware compiles for real boards)
west build --board kkm_p1_nrf52810 -d build-810 --pristine --no-sysbuild -- -DBOARD_ROOT=$(pwd)
west build --board nrf54l15dk/nrf54l15/cpuapp -d build-54l --pristine --no-sysbuild

## Host-native tests (macOS/Linux, ASan/UBSan, no Zephyr dependency)
cd tests/host && cmake -B build && cmake --build build && ./build/host_tests

## C++ quality checks
clang-format --dry-run --Werror src/*.cpp src/*.hpp
clang-tidy src/*.cpp -p tests/host/build

## DFU builds (boards with >= 512KB flash)
nix build .#firmware-nrf52832-dfu   # sysbuild: MCUboot + app
nix build .#firmware-nrf52833-dfu
nix build .#firmware-nrf54l15-dfu
# west: west build --board nrf52dk/nrf52832 -d build-dfu --pristine -- -DEXTRA_CONF_FILE=dfu.conf

## BabbleSim tests
# tests/bsim/   — BLE advertisement test: verifies AirTag key rotation
#   and MAC derivation produce correct payloads over simulated radio.
# tests/bsim_dfu/ — MCUmgr SMP echo test: verifies the DFU BLE transport
#   works end-to-end. Covers:
#   1. SMP GATT service registers and is discoverable by UUID
#   2. BLE central can connect, discover, subscribe to notifications
#   3. SMP echo request round-trips through the BLE transport
#   4. Response has valid SMP header (op=3, group=OS, id=echo)
#   5. Echoed payload contains the original test string
#   Does NOT test: MCUboot image swap, actual firmware upload, flash
#   partitions (nrf52_bsim lacks them). Those require real hardware.

## Binary size (nrf52810)
# C baseline: text=151968, data=2872, bss=19930
# C++ migration: text=152340 (+372 bytes, +0.2%)
# C++ overhaul:  text=152920 (+952 bytes, +0.6%)

## Supported boards
# Custom boards (in boards/arm/, need -DBOARD_ROOT=$(pwd)):
#   kkm_p1_nrf52810, kkm_p11_nrf52810, kkm_k4p, wb_20241125, hcbb22e, nrf52805_evm
# Zephyr built-in boards (no BOARD_ROOT needed):
#   nrf52dk/nrf52832, nrf52840dk/nrf52840, nrf52833dk/nrf52833, thingy52/nrf52832
# nrf54l15dk/nrf54l15/cpuapp (build-verified, BabbleSim-tested)

## Architecture (C++ migration)
# src/beacon_logic.hpp/cpp   — pure functions (MAC, adv template, status byte via StatusFlags)
# src/accel_data.hpp/cpp     — MovementTracker class (circular buffer)
# src/beacon_config.hpp/cpp  — BeaconConfig (narrowed types), SettingsManager (validated setters), GATT validators
# src/beacon_state.hpp/cpp   — StateMachine with IHardware interface, WhatInStatus enum
# src/ihardware.hpp          — hardware abstraction (virtual interface, 29 methods)
# src/zephyr_hardware.cpp    — IHardware impl wrapping Zephyr APIs
# src/zephyr_nvs.cpp         — INvsStorage impl wrapping ZMS
# src/gatt_glue.c            — BT_GATT_SERVICE_DEFINE (C99, can't be C++)
# src/ble_glue.c             — BT_DATA_BYTES arrays (C99, can't be C++)
# src/main.cpp               — static object graph, event loop

## west2nix lockfile
# west2nix.toml pins 51 NCS/Zephyr projects with Nix hashes.
# To regenerate after changing west.yml:
#   cd .. && west update && cd Everytag
#   uv run scripts/west2nix.py -o west2nix.toml
