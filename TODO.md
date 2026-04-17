# TODO

## bsim connect/disconnect lifecycle test

Add a bsim test with two nrf52_bsim executables: one running the
firmware (peripheral), one acting as a GATT central (standard Zephyr
bsim pattern — no Python/Bumble needed). The central:
- Connects, discovers services, disconnects — 20 iterations with 200ms
  dwell. Asserts the peripheral re-advertises between iterations (proves
  `.recycled` wiring works at the firmware level).
- Asserts auth-state isolation: after one client authorizes and
  disconnects, a fresh connection must NOT inherit prior auth.

Effort: ~half day. Extend existing `tests/bsim/` with a central role.

## bsim key-rotation-during-connection race test

Schedule the central's connection to coincide with the firmware's
key-rotation timer (BabbleSim gives simulated-time control). Assert no
double-start or stale-mode advertising after the rotation completes
during an active connection.

Effort: ~half day. Depends on the connect/disconnect test above for
the central scaffolding.

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
