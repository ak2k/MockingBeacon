# TODO

## Deferred NCS 3.2.4 migration fold-ins

Migration completed 2026-04-16 via 6 sequential PRs (#6–#10 + phase-4).
See `docs/plans/2026-04-16-refactor-ncs-3.2.4-upgrade-plan.md` for full
plan + appendix. These items were deferred from the migration:

- `bt_hci_err_to_str(reason)` in disconnect log (`src/gatt_glue.c`
  `disconnected()`) — improves debug output. Low effort.
- `BUILD_ASSERT(IS_ENABLED(CONFIG_BT_HAS_HCI_VS))` at top of
  `src/zephyr_hardware.cpp` HCI command block — compile-time guard
  against using VS commands without the VS subsystem.
- Minor log cleanup: remove redundant printk prefixes where Zephyr's
  LOG macros already add module names.

## Bumble-vs-firmware-in-bsim end-to-end test infrastructure

Phase 1β added `tests/ble_client/test_recycled_lifecycle.py` which exercises
rapid reconnect + auth-state isolation via the Python `BeaconGattServer` mock.
This validates the test harness side, not the firmware-side `.recycled` wiring.

Plan §1h originally called for "Bumble virtual BLE against `nrf52_bsim`
firmware" — a Bumble Python client connecting to firmware running in
BabbleSim's virtual BT controller. Existing `tests/bsim/` runs firmware
adv+scanner in two BabbleSim processes but has no GATT-client peer.

To close: build a bsim test that:
- Compiles firmware as `nrf52_bsim` (already supported by `tests/bsim`).
- Spawns a Python Bumble client + an HCI bridge to the BabbleSim phy
  (e.g., via Zephyr's `tests/bsim/bumble` integration if it lands, or
  custom HCI socket bridge).
- Iterates 20 connect/disconnect cycles, asserts adv resumes between.
- Asserts auth-state isolation across clients.

Estimated effort: 1-2 days of bsim infrastructure work. Defer until
either hardware arrives (preferred) or this test infra is needed for
another reason. Tracked here so it doesn't drop off the radar.

## Connect/disconnect-during-key-rotation race test

Plan §1h gate item 2: schedule a connection right after
`beacon_state.cpp:378` key-rotation transition, ensure no double-start
or stale-mode advertising. Requires firmware-side trigger to perform
key rotation while a connection is in progress, which the current
test harness doesn't expose. Same blocker as the bsim Bumble item
above. Defer.

## Hardware DFU swap validation post-NCS-3.2.4-migration

The NCS 2.9.2 → 3.2.4 migration was validated entirely in simulation (bsim_dfu
MCUmgr transport + imgtool verify + sysbuild log grep + hardcoded-ID grep in
src/boards). The runtime MCUboot slot-selection bug class (DevZone 127216 —
`flash_img_init` picks wrong slot under auto-assigned image IDs in NCS 3.2) is
NOT fully simulation-testable.

When nrf52832dk and/or nrf54l15dk hardware arrives:
- Flash latest main firmware (nrf52832-dfu, nrf54l15-dfu)
- Perform full DFU swap cycle via mcumgr: upload signed image → mark-pending
  → reboot → verify swap succeeded → assert new image is running
- If swap fails: the grep audit missed a hardcoded slot ID somewhere, OR the
  image-ID Kconfig assignment doesn't match runtime slot selection
- On success: close this item; mark migration fully validated

See docs/plans/2026-04-16-refactor-ncs-3.2.4-upgrade-plan.md Phase 3 "Known
residual risk" note for context.

## Set a production MCUboot signing key

Current state (2026-04-16): `sysbuild/mcuboot.conf` enables
`BOOT_SIGNATURE_TYPE_ECDSA_P256` but does NOT set `CONFIG_BOOT_SIGNATURE_KEY_FILE`.
MCUboot falls back to the upstream dev key (`bootloader/mcuboot/root-ec-p256.pem`),
which is public. Anyone with network proximity can sign a DFU image and upload it.

This is acceptable for pre-production dev builds (README.md:138 documents) but
**must be resolved before any field deployment**. Deferred during the NCS
2.9.2 → 3.2.4 migration to minimize scope.

To resolve:
- Generate a per-environment (or per-board) ECDSA-P256 signing key, store in
  secrets management (not in repo)
- Set `CONFIG_BOOT_SIGNATURE_KEY_FILE="${APPLICATION_CONFIG_DIR}/sysbuild/mcuboot/boards/keys/${BOARD}.pem"`
  in `sysbuild/mcuboot.conf` (or per-board overlay)
- CI: inject key from encrypted secret at build time; do NOT commit
- Re-enable signature-isolation test in DFU integration suite (currently skipped
  per plan docs/plans/2026-04-16-refactor-ncs-3.2.4-upgrade-plan.md Phase 0 decision)

## Migrate advertising to extended-adv API (bt_le_ext_adv_*)

Deferred from the NCS 2.9 → 3.2 migration. We currently use legacy
`bt_le_adv_start` / `bt_le_adv_stop` / `bt_le_adv_update_data` and
stop+restart to switch payload between AirTag / FMDN / iBeacon / settings
modes. All current Zephyr 4.x / NCS 3.x samples (`multiple_adv_sets`,
peripheral samples) use `bt_le_ext_adv_create` + `bt_le_ext_adv_set_data`
+ `bt_le_ext_adv_start/stop` with one persistent `bt_le_ext_adv *` per
payload and a per-set `bt_le_ext_adv_cb`.

Benefits:
- Remove state-machine flag churn around adv mode switching
  (`beacon_state.cpp` ~L52-80)
- Unlock simultaneous non-connectable AirTag/FMDN + connectable settings
  (impossible with legacy API)
- Align with long-term Zephyr direction

Effort: medium — cross-cutting in `zephyr_hardware.cpp`, `beacon_state.cpp`,
possibly `gatt_glue.c`. Legacy API still works in 3.2.x, so this is not
blocking. Do after migration lands and is stable.

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
