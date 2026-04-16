# TODO

## Upgrade to nRF Connect SDK v3.x

Currently on NCS v2.9.2 (Zephyr 3.7.99). NCS 3.x is a major release
with breaking changes. Latest is v3.2.4 at time of writing.

Scope:
- Bump `west.yml` sdk-nrf revision → `v3.2.4`
- `nix run .#update-lockfile` to regenerate west2nix.toml
- Fix Kconfig deprecations (3.x has moved several options, renamed
  some, removed others)
- Fix Zephyr API churn (3.x drops some legacy Zephyr APIs)
- MCUboot partition layout may change — existing devices with
  2.9-built MCUboot can't OTA to a 3.x-built signed image without
  SWD recovery. Plan for fleet coordination if that matters.
- Verify nRF54L15 hw accel PSA Crypto (introduced in 2.9, possibly
  improved in 3.x)
- Re-run full test suite + BabbleSim + release matrix

Budget: 2–4 hours, depending on API churn.


## Fuzz the GATT settings handlers

The 15 `conn_beacon.py` UUIDs (in `src/gatt_glue.c`) each accept binary
writes from any BLE client in range — a real attack surface. Extract
the parser/validator logic to a host-testable form and wire libFuzzer
targets (or AFL++). Good candidates:

- key upload (`8c5debde-...`) — 14-byte keys, bounds checks
- FMDN key (`8c5debe2-...`) — hex bytearray parsing
- status byte (`8c5debe5-...`) — 32-bit packed flags
- accel threshold (`8c5debe6-...`) — 32-bit little-endian
- time sync (`8c5debe3-...`) — 8-byte epoch

Related: most of `src/beacon_config.cpp` validators are already in a
host-testable shape (they live behind `SettingsManager` + `GATT validators`).
The piece not yet host-testable is the raw BLE write decode in
`gatt_glue.c`.

## Refactor `conn_beacon.py`

Python 2-era style inherited from upstream vasimv fork: bare `except`,
mutable globals in argparse's `args` dict, no type hints, no subcommands.
Ruff found ~20 issues which we auto-fixed, but the module structure
remains procedural.

Minimal improvement path:
- Extract the BLE write helpers into a `BeaconClient` class
- Add `click` or argparse subcommands (`load-keys`, `set-time`, ...)
- Type hints + `bumble` integration (deprecate `simplepyble` dependency)

Tradeoff: any refactor creates drift vs. upstream, which matters if we
pull upstream patches. Current minimal-touch approach is fine while
upstream remains active.
