# TODO

## Preserve vendor-neutral portability option

The core beacon logic is already chip-agnostic by design (pure C++ via
the `IHardware` abstraction). Keeping it that way gives us the option
to retarget silicon if product direction changes, without a ground-up
rewrite.

### What "vendor-neutral" actually means here

Not "zero vendor blobs." Most BLE-capable SoCs ship a proprietary
controller because BLE timing requires deep radio-peripheral access —
escaping binary blobs entirely restricts chip choice severely.

What we care about: **the application, library, and IHardware layers
stay portable**. Each target picks its own BLE controller via Kconfig;
the source tree doesn't fork.

### Controller options by target

| Target | BLE controller | License |
|---|---|---|
| Nordic nRF5x (default) | SDC (`CONFIG_BT_LL_SOFTDEVICE`) | Nordic 5-clause, Nordic HW only |
| Nordic nRF5x (fallback) | Zephyr SW LL (`CONFIG_BT_LL_SW_SPLIT`) | Apache-2.0, open |
| ESP32 | Espressif's ROM stack via HCI | ESP32 HW only, proprietary |
| TI CC13xx/CC26xx | Varies | Mix |
| Silicon Labs EFR32 | Vendor controller | EFR32 HW only |

The Zephyr BT **host** is the same across all of these. Application
code (our `src/`) compiles unchanged.

### Current Nordic-specific surface

All legitimate dependencies on Nordic components, used as intended:

- **SoftDevice Controller (SDC)** — Nordic's BLE controller library
  (`nrfxlib/softdevice_controller/libsoftdevice_controller_*.a`).
  Default for NCS builds.
- **MPSL** — Multi-Protocol Service Layer, default companion to SDC.
- **nrf_memconf RAM retention** — `src/zephyr_hardware.cpp` in the
  `CONFIG_SOC_NRF54L15_CPUAPP` guard (already isolated behind `#ifdef`).
- **Board definitions** — all current `boards/arm/*` entries are Nordic
  SoCs (nRF52805/nRF52810/nRF52832/nRF52833). Not a deliberate lock-in —
  these are what KKM/Honeycomm/WB shipped.

### What keeps us portable

- `beacon_logic`, `accel_data`, `beacon_config`, `beacon_state`,
  `ihardware.hpp` — pure C++, no Zephyr or vendor headers.
- Zephyr's open-source BLE controller (`CONFIG_BT_LL_SW_SPLIT`,
  Apache-2.0) works on Nordic as a drop-in alternative to SDC. Already
  used for our bsim tests, so we know it builds.
- Zephyr BT host API (`bt_le_adv_start`, etc.) is controller-agnostic.

### To keep the option alive (low-effort hygiene)

- Continue isolating vendor-specific code behind `#ifdef` in
  `zephyr_hardware.cpp` — no vendor APIs creep into library headers.
- When modularizing (TODO below), enforce the rule at the module
  boundary — module headers must not include `nrfx/*` or Nordic
  softdevice headers.
- Periodically build with `CONFIG_BT_LL_SW_SPLIT=y` on Nordic to
  confirm it still works (takes ~5 min).

### To actually retarget (per-target effort, do when needed)

**Same-vendor (Nordic → Nordic, e.g. nRF52 → nRF54):** trivially done;
we already do this today across 6 Nordic SoCs.

**Different vendor (Nordic → ESP32 / EFR32 / TI):**
- Add `boards/arm/<vendor-chip>/` entry (DTS, Kconfig.defconfig,
  pinctrl).
- Select that vendor's BLE controller in the board's Kconfig.defconfig.
- Audit for any direct `nrfx`/`nrf_*` calls in `zephyr_hardware.cpp`;
  swap for the new vendor's peripheral drivers (ADC, GPIO, I2C, etc.).
- Update `flake.nix` board list.
- Validate power/timing — vendor-tuned BLE may outperform the generic
  controller on battery life.

Not urgent. File so the context is recoverable if/when it matters.

## Package beacon logic as a Zephyr module

References:
- https://github.com/zephyrproject-rtos/example-application — canonical
  Zephyr pattern for "out-of-tree application that is also a module."
  Shows `zephyr/module.yml` + `app/` + `lib/` + `include/app/` layout
  plus Twister integration.
- https://github.com/koenvervloesem/openhaystack-zephyr — smaller
  example of the module-only pattern with callback-based API.

Gap analysis vs example-application:
- We have: `.github/workflows`, `boards/`, `west.yml`, `CMakeLists.txt`
- We're missing: `zephyr/module.yml` (the one file that makes us
  module-consumable), `app/` vs `lib/` split, `include/app/` for
  public headers, Sphinx/Doxygen docs, Twister tests

### Current state

The code is already architecturally split but not packaged:

- **Library-shaped** (host-testable, no Zephyr includes):
  `src/beacon_logic.{cpp,hpp}`, `src/accel_data.{cpp,hpp}`,
  `src/beacon_config.{cpp,hpp}`, `src/beacon_state.{cpp,hpp}`,
  `src/ihardware.hpp`, `src/beacon_nvs_ids.hpp`
- **App-shaped** (Zephyr-specific, product-specific):
  `src/zephyr_hardware.cpp`, `src/gatt_glue.c`, `src/ble_glue.c`,
  `src/main.cpp`, `boards/`, `prj.conf`, `dfu.conf`

### Benefits worth the cost

- **API discipline**: module boundary forces "public vs internal" to
  be explicit. Right now any code can reach into any header — easy
  for implicit coupling to creep in.
- **Documentation pressure**: consumers can't grep source, so public
  API needs docs. Without a module, docs rot.
- **Independent versioning**: library can stabilize (v1.x) while the
  product iterates (v0.x).
- **SBOM clarity downstream**: `everytag-beacon` appears as a distinct
  supply-chain dep in consumer projects' buildtime SBOMs.
- **Portability surface**: IHardware already abstracts Zephyr; module
  packaging makes that contract explicit if ever porting to another
  RTOS or to bare-metal.

### Minor benefits (relevant only if external consumers)

- License separation (module permissive, app whatever).
- CI surface reduction (module tests independent).
- Contribution boundary clarity.

### Rough plan

1. Create `modules/everytag-beacon/` (or separate repo).
2. Move library sources: `beacon_logic`, `accel_data`, `beacon_config`,
   `beacon_state`, `ihardware.hpp`, `beacon_nvs_ids.hpp`.
3. Define public API headers (in `include/everytag/beacon/`?) —
   explicit `#include <everytag/beacon/state_machine.hpp>` etc.
4. Add `zephyr/module.yml` at module root.
5. Add module-level `Kconfig` with `CONFIG_EVERYTAG_BEACON=y`.
6. Module `CMakeLists.txt` compiles library sources, exposes include
   dir, hides internals.
7. App `west.yml` imports the module.
8. App's `CMakeLists.txt` no longer compiles library sources directly
   — they come from the module.
9. Move host tests into module repo (module owns its tests).
10. Document the public API (`docs/` in module repo).

### When to do this

Defer unless:
- A second product wants to reuse the beacon logic (different GATT,
  different settings layer, same AirTag/FMDN/iBeacon core), OR
- Maintenance complexity grows to where the API discipline payoff
  exceeds the packaging overhead, OR
- External contributors start submitting PRs (module gives them a
  clear boundary).

Estimated effort: 1-2 days for initial split, then ongoing discipline.

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

## ~~Migrate advertising to extended-adv API (bt_le_ext_adv_*)~~ — DEFERRED INDEFINITELY

Prototyped and reverted (2026-04-16, branch `feat/ext-adv-and-todos`).

**Why not:** nrf52810 (24 KB RAM) overflows by 2004 B with the SDC multirole
variant required for `CONFIG_BT_EXT_ADV=y`. The `#ifdef` split to maintain
both code paths costs more in complexity than the benefit (simultaneous adv
sets) delivers — especially since the primary production target gets nothing.

**What was done instead:** cleaned up `IHardware` interface to use
mode-specific `adv_start_airtag()` / `adv_start_fmdn()` instead of the
boolean-encoded `adv_start(bool, int, int, bool)` (PR #12). Legacy
`bt_le_adv_start` on all boards. `.recycled` workqueue bridge stays.

**Revisit if:** simultaneous connectable + non-connectable advertising
becomes a hard requirement, OR nrf52810 is deprecated from the board list.

## Implement real Find Hub Network (FHN) participation

Today's "FMDN" code is a static-blob Eddystone-shaped frame that
Android Find Hub does not resolve. Plan to replace with a real
FHN-compliant path, OpenHaystack-style (no Fast Pair, no Google
Model ID registration).

**Design summary** — full plan in
`docs/plans/2026-04-17-fhn-openhaystack-plan.md`:

- **Design A (universal)**: backend pregenerates N-day EID batches
  from a per-tag EIK, uploads them to Google's servers (via
  reverse-engineered endpoint), and flashes the same batch to the
  tag over our existing GATT settings window. Works on every board
  (nRF52805–nRF54L15). 30-day default batch.
- **Design C (nRF54L15-only, preferred for 54L SKU)**: tag stores
  just the 32-byte EIK and derives EIDs on-device via Cracen
  (secp256r1 + AES-ECB-256 + HKDF-SHA256) at 1024s cadence. No
  batch, no re-flash, tag is autonomous. Backend still uploads EID
  batches to Google (server-side resolution window requirement).
- Decoupled rotation: AirTag keeps our existing cadence; FHN rotates
  per spec (1024s + random 1–204s). `CONFIG_EVERYTAG_FHN_STATIC_MAC`
  Kconfig as fallback if Android unwanted-tracker alerts prove
  problematic in field testing.
- Companion app is a thin BLE relay; backend holds the EIK and
  drives all Google API interaction. Per-user dedicated Google
  account.
- Unprovisioning: time-since-seen auto-expire + explicit app trigger.
  Per-device anti-spoof keypair stored in KMU (54L15) / ZMS (52xxx).

**Accepted tradeoffs:**

- Backend cron to top up Google-side EID registrations every ≤3 days
  (non-negotiable; inherent to FHN architecture)
- Undocumented/RE'd Google endpoint — may break; nRF52810 has no
  OTA path (reflash-only recovery)
- DULT skipped for now — enthusiast/personal use; revisit if
  unwanted-tracker alerts fire in the field
- No Fast Pair certification — legitimate upgrade path architected
  in (locator_tag vendoring) but deferred

**Supersedes** the FMDN stub; README updates once implemented.

## Fold "FMDN" README wording once real FHN ships

Low-priority follow-up. Current README/project description calls it
"FMDN" but what's on the wire is a static-blob Eddystone-shaped
frame, not FHN-resolvable (see plan above). Once the real FHN
implementation ships, rewrite the README section to describe the
actual capability; until then leave as-is (code symbols also stay
as-is to preserve git history grep).

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
