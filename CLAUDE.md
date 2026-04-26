# Build & Test Commands

## Prerequisites
Run `nix develop` to enter the dev shell with all dependencies.
Run `cd .. && west init -l MockingBeacon && west update --narrow -o=--depth=1` to fetch Zephyr + NCS modules.
# Migrating from a 2.9.2 workspace: rm -rf ../zephyr ../nrf ../modules ../bootloader ../tools && west update

## Flake apps (no nix develop needed)
nix build .#firmware    # cross-compile all boards in parallel (nrf52810 + nrf54l15)
nix build .#bsim-test  # BabbleSim BLE + ZMS persistence tests (Linux, Cachix-cached)
nix run .#test          # host-native tests (290 assertions, ASan/UBSan)
nix run .#lint          # clang-format check (dry-run, fails on diff)
nix run .#format        # auto-format C++ sources

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

## BabbleSim tests (nix build .#bsim-test)
# tests/bsim/        — BLE adv + MAC verification (nrf52_bsim + nrf54l15bsim)
# tests/bsim_dfu/    — MCUmgr SMP echo test (nrf52_bsim)
# tests/zms_persist/ — ZMS write/remount/read persistence (native_sim, 25 assertions)
# All three run in a single Nix derivation via gccMultiStdenv.
# BabbleSim components are pinned fetchGit derivations, cached via Cachix (ak2k store).
# west2nix uses PR#2 (wrvsrx/west2nix#reduce-repeating-clone) for fast builds.

## Bumble BLE client tests
# tests/ble_client/test_conn_beacon.py — GATT unit tests (8 tests: auth enforcement,
#   key upload, time read/write, all settings)
# tests/ble_client/test_conn_beacon_integration.py — runs actual conn_beacon.py against
#   Bumble virtual BLE (12 tests: all 15 CLI options covered)
# Run: uv run --with bumble --with pytest --with pytest-asyncio pytest tests/ble_client/ -v

## Binary size (nrf52810)
# C baseline (NCS 2.9.2): text=151968, data=2872, bss=19930
# C++ migration: text=152340 (+372 bytes, +0.2%)
# C++ overhaul:  text=152920 (+952 bytes, +0.6%)
# + ZMS, MAC logging, nrf54l15 guards: text=154544 (+2576, +1.7%)
# NCS 3.2.4 migration (+ .recycled wq, stack pins): text=154576, data=2912, bss=21376 (+1.7% text vs C baseline)

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
#   cd .. && west update && cd MockingBeacon
#   uv run scripts/west2nix.py -o west2nix.toml


<!-- BEGIN BEADS INTEGRATION v:1 profile:minimal hash:ca08a54f -->
## Beads Issue Tracker

This project uses **bd (beads)** for tracked work items. Discover available
work, claim, and close in the standard bd loop. `bd prime` for the full
workflow context. Beads complement (not replace) the project's existing
`TODO.md` (design roadmap) and `docs/plans/` (phased design docs).

### Quick Reference

```bash
bd ready              # Find available work
bd show <id>          # View issue details
bd update <id> --claim  # Claim work
bd close <id>         # Complete work
```
<!-- END BEADS INTEGRATION -->
