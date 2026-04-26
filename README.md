# MockingBeacon

> **Lineage:** MockingBeacon (formerly Everytag) is a C++ restructuring of the firmware with comprehensive testing.
> Upstream: [OpenHaystack](https://github.com/seemoo-lab/openhaystack) → [macless-haystack](https://github.com/dchristl/macless-haystack) → [vasimv/Everytag](https://github.com/vasimv/Everytag) → MockingBeacon.
>
> **What changed:**
> - NCS 3.2.4 / Zephyr 4.2 (staged migration from 2.9.2 preserved `.recycled` advertising lifecycle and RAM budgets on nRF52810)
> - C++ modules with explicit state machine and `IHardware` abstraction — pure computation is fully testable off-target
> - 317 host-native test assertions (ASan/UBSan) covering pure computation
> - BLE advertisement payloads verified end-to-end via [BabbleSim](https://babblesim.github.io/) simulation (3-key rotation over simulated radio, MCUmgr SMP echo for DFU transport)
> - BLE client integration tests against a virtual GATT server ([Bumble](https://google.github.io/bumble/)) — all 15 `conn_beacon.py` CLI options exercised with auth enforcement
> - ZMS persistence test: write → remount → read roundtrip on native_sim flash
> - Nix flake for reproducible cross-compilation (`nix build .#firmware`) — no west setup needed
> - nrf54l15dk board support (build-verified, BabbleSim-tested)
> - NVS to ZMS storage migration (future-proof for nRF54 series)
> - Binary size: see [flash usage table](#flash-usage) — 69% release / 80% dev on the tightest target (nRF52810), ample headroom elsewhere
>
> **Quick start:**
> ```
> nix build .#firmware          # cross-compile for all boards
> nix run .#test                # host-native tests (317 assertions)
> nix run .#lint                # clang-format check
> ```

The firmware advertises as an **Apple AirTag** using the Offline Finding protocol (up to 40 public keys rotating at default 10 minute intervals for tracker anonymity), alongside an **Eddystone-EID-format secondary beacon** with a single static 20-byte identifier. If no AirTag keys are loaded, it falls back to a plain **iBeacon** carrying battery voltage. Full **Google Find My Device Network (FMDN)** integration — ephemeral-ID rotation, Google account provisioning — is planned, not shipped; the Eddystone-EID beacon is the advertising foundation for that future work. Runs on nRF52/54 chips using Zephyr, optimized for microampere-range power consumption; the MCU's watchdog ensures it keeps running until the battery dies.

All settings (keys, TX power, broadcast interval, etc.) can be reconfigured over BLE without reflashing — `conn_beacon.py` (Python 3) drives the configuration protocol. To minimize power consumption, the firmware accepts BLE connections only for 2 seconds every minute, gated by an 8-byte auth code. Signed firmware updates (MCUmgr SMP over BLE) are supported on boards with ≥512 KB flash.

Additional features:
- **Clock tracking** — if the board has a 32.768 kHz crystal, the firmware counts time and periodically saves it to flash so the clock survives reboots with minimal drift
- **Accelerometer support** — on boards with a LIS2DW12 (e.g., KKM K4P), movement is tracked and encoded as a 7-bit summary byte in the advertisement status field
- **Status byte telemetry** — the advertisement status byte can be configured to report battery voltage, accelerometer movement, temperature, or cycle between all three every minute

## Supported hardware

Custom boards (defined in `boards/arm/`, need `-DBOARD_ROOT=$(pwd)`):
- KKM P1 / P11 (nRF52810), KKM K4P with accelerometer (nRF52833)
- WB 20241125, Fanstel NRF52805EVM (nRF52805), Minew HCB22E (nRF52832)

Zephyr built-in boards (no `BOARD_ROOT` needed):
- nRF52DK (nRF52832), nRF52840DK, nRF52833DK, Thingy:52

nRF54 series:
- nRF54L15DK (build-verified, BabbleSim-tested)

For NRF52DK, KKM C2, and KKM K4P you need to use the button to turn on after first flash. On other boards it starts right after flashing (1 short + 2 long LED blink).

Button (first one on NRF52DK): long press until 1 short + 2 long flashes to start. Same long press to shut down (two short flashes, then `sys_poweroff()` at < 1 uA). Power state is stored in flash and survives battery replacement.

## Building

### With Nix (recommended)

No west setup needed:

```
nix build .#firmware              # all board targets (dev config)
nix build .#firmware-nrf52810     # single board (dev: logging + RTT)
nix build .#firmware-nrf52810-release  # production: no logging
nix build .#firmware-nrf52832     # dev
nix build .#firmware-nrf52832-release  # production
nix build .#firmware-nrf52832-dfu # MCUboot + OTA (always release config)
nix build .#firmware-nrf52833-dfu
nix build .#firmware-nrf54l15-dfu
nix run .#test                    # host-native tests (ASan/UBSan)
nix run .#lint                    # clang-format check
nix run .#format                  # auto-format C++ sources
```

DFU targets build with sysbuild (MCUboot + application) and produce `app_update.bin` for OTA uploads. Boards that lack MCUboot partition layouts (nRF52805, nRF52810) simply don't have DFU targets.

### Flash usage

Dev builds use `prj.conf` (logging, RTT console, GPIO enabled). Release builds use `prj-lowpower.conf` (all disabled — production configuration). DFU builds always use the release config.

| SoC | Build | Used | Available | Usage | Note |
|-----|-------|-----:|----------:|------:|------|
| nRF52810 | dev | 154 KB | 192 KB | **80%** | |
| nRF52810 | release | 133 KB | 192 KB | 69% | |
| nRF52832 | dev | 148 KB | 512 KB | 29% | |
| nRF52832 | release | 127 KB | 512 KB | 25% | |
| nRF52832 | DFU app slot | 145 KB | 220 KB | 66% | +33 KB MCUboot |
| nRF52833 | dev | 148 KB | 512 KB | 29% | |
| nRF52833 | release | 127 KB | 512 KB | 25% | |
| nRF52833 | DFU app slot | 145 KB | 220 KB | 66% | +33 KB MCUboot |
| nRF54L15 | dev | 177 KB | 1524 KB | 12% | |
| nRF54L15 | release | 155 KB | 1524 KB | 10% | |
| nRF54L15 | DFU app slot | 172 KB | 674 KB | 26% | +36 KB MCUboot |

Logging and RTT add ~21 KB. The nRF52810 is the tightest target — 69% in release, 80% in dev — with 38–59 KB of headroom. All other platforms have ample room.

### With west

Requires nRF Connect SDK 2.8.0:

```
# Small-flash boards (no DFU)
west build --board kkm_p1_nrf52810 -d build-810 --pristine --no-sysbuild -- -DBOARD_ROOT=$(pwd)

# DFU-capable boards (omit --no-sysbuild to enable MCUboot)
west build --board nrf52dk/nrf52832 -d build-dfu --pristine -- -DEXTRA_CONF_FILE=dfu.conf
west build --board nrf54l15dk/nrf54l15/cpuapp -d build-dfu --pristine -- -DEXTRA_CONF_FILE=dfu.conf
```

### Host-native tests

No Zephyr dependency needed:

```
cd tests/host && cmake -B build && cmake --build build && ./build/host_tests
```

## Flashing

Erase all flash before first run to reset storage:

```
nrfjprog --eraseall
```

After flashing via SWD or using the RTT debug console, disconnect power (remove battery) for 10-20 seconds and reconnect. nRF chips often enter high power consumption (milliamps) after using J-Link RTT, and software reset doesn't help.

## Firmware updates

**SWD (all boards):** Flash via J-Link / debugger.

**OTA via BLE (DFU targets):** Build a `-dfu` target, flash initially via SWD, then update over the air:

```
# Initial flash
west flash -d build-dfu

# OTA update (authenticates via BLE, uploads signed image via mcumgr)
./flash_beacon.sh <MAC_ADDR> <AUTH_KEY> build-dfu/zephyr/app_update.bin
```

Settings stored in ZMS are preserved across OTA updates. Use full chip erase via SWD to reset them.

DFU builds ship with MCUboot's default dev signing key. This means anyone can build and OTA their own firmware. To lock DFU to your own key, generate one with `imgtool keygen -k my-key.pem -t ecdsa-p256` and set `CONFIG_BOOT_SIGNATURE_KEY_FILE` in `sysbuild/mcuboot.conf`.

## Changing settings via BLE

Every minute the beacon switches into settings mode for 2 seconds (configurable). Settings can be changed using `conn_beacon.py` (Python 3).

All operations require password authentication (default: `abcdefgh`, changeable via settings mode).

Note: after loading an AirTag key, the MAC address in settings mode changes. It is recommended to set a specific settings MAC address (`-c` flag) when loading keys. Otherwise, you'll need to discover the new MAC via a BLE scanner (e.g., nRF Connect app).

### conn_beacon.py reference

| Flag | Description | Example |
|------|-------------|---------|
| `-i` | BLE MAC address (required) | `-i e7:93:3a:cc:f6:61` |
| `-a` | Authentication code (required) | `-a abcdefgh` |
| `-n` | Set new authentication code | `-n h45edc78` |
| `-c` | Set new MAC for settings mode | `-c e7:93:3a:cc:f6:61` |
| `-k` | Load AirTag keys from binary file | `-k ~/airtags/016WX1_keyfile` |
| `-f` | Set Google FMDN key (hex) | `-f f1e101...` |
| `-t` | Enable/disable AirTag broadcast | `-t 1` or `-t 0` |
| `-g` | Enable/disable FMDN broadcast | `-g 1` or `-g 0` |
| `-p` | TX power: 0 = -8 dBm, 1 = 0 dBm, 2 = +4/+8 dBm | `-p 2` |
| `-d` | Broadcast interval multiplier: 1, 2, 4, or 8 seconds | `-d 2` |
| `-l` | Key rotation interval in seconds (default 6000) | `-l 600` |
| `-s` | Status byte config (hex, see below) | `-s 458000` |
| `-m` | Accelerometer threshold in mg (0 = disable) | `-m 800` |
| `-w` | Sync host clock to beacon | `-w 1` |
| `-r` | Read beacon clock | `-r 1` |

### Status byte encoding

The `-s` flag configures what the AirTag/FMDN status bytes report. It's a packed 32-bit hex value:

```
bits  0..7  — base AirTag status byte (used in mode 1)
bits  8..15 — base FMDN status byte (used in mode 1)
bits 16..19 — AirTag mode:
  0 = off         3 = battery voltage    5 = telemetry (cycle all)
  1 = fixed byte  4 = battery level
  2 = counter
bits 20..23 — FMDN mode (same as AirTag mode)
```

Default `0x458000` = AirTag telemetry (cycles voltage/accel/temperature each minute), FMDN battery level, AirTag base `0x00`, FMDN base `0x80`.

### Examples

Load keys and configure:
```
python3 conn_beacon.py -i e7:93:3a:cc:f6:61 -c e7:93:3a:cc:f6:61 -a abcdefgh \
  -n h45edc78 \
  -f f1e101731f1e1b312272f812e521293a3f484414 \
  -k ~/airtags/016WX1_keyfile \
  -p 2 -g 1 -t 1 -w 1 -d 2
```

This connects to the beacon, changes the password to `h45edc78`, loads an AirTag key and FMDN key, sets max TX power, and enables both AirTag and FMDN broadcasting alternating every 2 seconds.

Sync clock:
```
python3 conn_beacon.py -i e7:93:3a:cc:f6:61 -a h45edc78 -w 1
```

Read time:
```
python3 conn_beacon.py -i e7:93:3a:cc:f6:61 -a h45edc78 -r 1
```

## Default behavior

Out of the box, the firmware broadcasts dummy iBeacon packets at 7-second intervals at -8 dBm, with the TX power field reporting battery voltage (e.g., 31 = 3.1V). This broadcast stops once you load AirTag and/or FMDN keys.

The beacon uses its factory MAC address for iBeacon and settings broadcasts.

## Android app

A clean-sheet Android companion app is planned but not yet implemented — see [`docs/plans/2026-04-25-android-app-rewrite-plan.md`](docs/plans/2026-04-25-android-app-rewrite-plan.md). The earlier inherited `tagcheck/` reference implementation has been removed (Activity-era patterns, no CI, never released as an APK); use `conn_beacon.py` and `flash_beacon.sh` for now.

## Credits and project history

This project (formerly **Everytag**, renamed to **MockingBeacon** in 2026-04) restructures the firmware as C++ with an `IHardware` abstraction and comprehensive off-target testing.

It originated as a fork of the [macless-haystack](https://github.com/dchristl/macless-haystack) project, which itself builds on the [OpenHaystack](https://github.com/seemoo-lab/openhaystack) research project by the Secure Mobile Networking Lab at TU Darmstadt.

The original C firmware **and Android app** were written by [vasimv](https://github.com/vasimv/Everytag), adding multi-key rotation, Google FMDN support, BLE settings reconfiguration, accelerometer tracking, OTA updates, and power optimization for nRF52 chips.

### Useful tools

- [FindMy scripts](https://github.com/biemster/FindMy) — query Apple Find My locations
- [GoogleFindMyTools](https://github.com/leonboe1/GoogleFindMyTools) — create FMDN keys and query Google Find My Device locations
