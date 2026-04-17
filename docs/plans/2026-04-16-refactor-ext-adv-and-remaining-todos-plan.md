---
title: Extended-adv migration + remaining TODOs
type: refactor
status: abandoned
date: 2026-04-16
---

# Extended-adv migration + remaining TODOs

## Overview

Five TODO items remain after the NCS 3.2.4 migration. The ext-adv migration
(#1) is the architectural centerpiece — it replaces the legacy single-global-
advertiser model with per-set `bt_le_ext_adv` instances on boards with
sufficient RAM, while nrf52810 (24 KB SRAM) stays on the legacy API.

## Problem Statement

**Current:** one global advertiser, stop-start-stop alternation between 4
modes, a dedicated workqueue thread (1024 B stack) to bridge the async
`.recycled` callback into the synchronous state machine. Four boolean flags
(`broadcasting_anything_`, `_airtag_`, `_fmdn_`, `_settings_`) track which
single mode is active.

**With ext-adv (nrf52832+, nrf54l15):** one `bt_le_ext_adv*` handle per mode.
Non-connectable sets (AirTag + FMDN) can coexist — no alternation needed.
Connectable set (Settings) still needs `.recycled` + workqueue restart (the
`.recycled` callback is on `bt_conn_cb`, not per-adv-set, even with ext-adv).
Key rotation stops only the AirTag set, not the entire BT stack.

**nrf52810 stays on legacy:** confirmed overflow — ext-adv pulls in SDC
multirole variant, RAM overflows by 2004 bytes. Not recoverable with
Kconfig levers. Legacy API + workqueue bridge remains for this board.

## Architecture: IHardware absorbs the split

The `IHardware` abstraction layer hides the ext-adv vs legacy difference.
The state machine says *what* it wants; the hardware impl decides *how*.

```
IHardware (interface)           — mode-specific methods, no #ifdef
  ├─ ZephyrHardware (ext-adv)   — bt_le_ext_adv per set, simultaneous non-conn
  ├─ ZephyrHardware (legacy)    — current code, single global advertiser
  └─ MockHardware (tests)       — trivial stubs, single implementation

beacon_state.cpp                — calls mode-specific IHardware methods
                                  no #ifdef, no awareness of ext-adv vs legacy
```

`#ifdef CONFIG_BT_EXT_ADV` lives only inside `zephyr_hardware.cpp` method
bodies. State machine, tests, and mock are unchanged. No divergent test
matrices.

### IHardware interface changes

Replace the generic `adv_start(bool connectable, int intvl_min, int intvl_max,
bool use_fmdn)` with mode-specific methods:

```cpp
// New — replaces adv_start()
virtual int adv_start_airtag(int interval_min, int interval_max) = 0;
virtual int adv_start_fmdn(int interval_min, int interval_max) = 0;
// start_settings_adv() already exists
// broadcast_ibeacon() already exists

// New — per-mode stop (ext-adv can stop one set, legacy stops the global)
virtual void adv_stop_broadcast() = 0;  // stops AirTag/FMDN/iBeacon
// stop_settings_adv() already exists
// adv_stop() remains as "stop everything"

// adv_update_airtag() / adv_update_fmdn() unchanged
```

### What the workqueue looks like after

The `.recycled` workqueue stays on all boards — it's the bridge for the one
async event Zephyr forces on us. What changes:

- **ext-adv boards:** handler calls `bt_le_ext_adv_start(settings_set, ...)`
  to restart the specific connectable set.
- **nrf52810:** handler calls `::start_settings_adv()` (unchanged from today).

Both paths are behind `#ifdef CONFIG_BT_EXT_ADV` in the handler. The
workqueue infrastructure (stack, init, cancel_sync, set_allowed) is shared.

### Alternation elimination

**ext-adv boards:** `handle_airtag_fmdn_alternation()` becomes "start both
sets." `broadcast()` starts AirTag AND FMDN simultaneously. No stop-start
cycle.

**nrf52810:** `adv_start_fmdn()` internally stops the AirTag set first (only
one global advertiser). The alternation logic moves INTO `ZephyrHardware`'s
legacy impl, out of the state machine. State machine always says "start
both" — legacy impl serializes.

### Key rotation simplification

**ext-adv boards:** stop only the AirTag set → update payload → restart.
FMDN and Settings (if active) keep running. No `bt_disable()`/`bt_enable()`
cycle. MAC change: AirTag set uses `BT_LE_ADV_OPT_USE_IDENTITY`; MAC is
changed globally via `bt_ctlr_set_public_addr()` which still requires
`bt_disable()`. So the full teardown remains unless we switch AirTag to a
per-set random address. **Decision: keep the teardown for now** — per-set
random addresses require changes to the AirTag Offline Finding MAC-to-key
derivation protocol. Defer to a future iteration.

**nrf52810:** unchanged from today.

## Pre-work: fix pauseUpload bug

Spec-flow analysis found a real bug: `chrc_write_auth` (gatt_glue.c:128)
sets `pauseUpload = 1` on every successful auth. If a user authenticates
to just read the time, then disconnects without writing any setting,
`pauseUpload` remains true. On the next `handle_settings_mode_exit()`
(beacon_state.cpp:204), the firmware enters FirmwareUpload state and waits
60s before rebooting. Fix: only set `pauseUpload` when a settings write
actually occurs (not on auth alone). Fix before ext-adv migration to avoid
debugging this during the refactor.

## Implementation Phases

### Phase 0: Pre-work (half day)

- [x] Investigate `pauseUpload` "bug" — intentional DFU trigger, not a bug.
  Auth sets it; every write/read clears it. Auth+disconnect = DFU request.
  Added explanatory comment. No code change needed.

### Phase 1: ext-adv migration (2-3 days)

**1a. IHardware interface** — add mode-specific methods as above. Update
`MockHardware` in `tests/host/src/test_main.cpp`. Host tests must pass.

**1b. ZephyrHardware ext-adv impl** (`#ifdef CONFIG_BT_EXT_ADV`) —
- Create ext-adv sets at `bt_enable()`: one connectable (settings), one
  non-connectable (broadcast — shared by AirTag/FMDN/iBeacon, switching
  payload via `bt_le_ext_adv_set_data()`).
- `adv_start_airtag()` / `adv_start_fmdn()` → set data on broadcast set,
  start it.
- `start_settings_adv()` → start connectable set.
- `.recycled` handler → `bt_le_ext_adv_start(settings_set, ...)`.
- Kconfig: `CONFIG_BT_EXT_ADV=y`, `CONFIG_BT_EXT_ADV_MAX_ADV_SET=2`,
  `CONFIG_BT_BUF_EVT_DISCARDABLE_SIZE=58` (ext-adv events are larger).
  Applied via board overlay for nrf52832+/nrf54l15, NOT in base prj.conf
  (keeps nrf52810 on legacy).

**1c. ZephyrHardware legacy impl** (`#else`) — adapt existing code to the
new IHardware interface. `adv_start_airtag()` calls the existing
`bt_le_adv_start()` wrapper. `adv_start_fmdn()` stops AirTag first if
running (since only one global advertiser). This is the alternation logic
moved from `beacon_state.cpp` into the hardware layer.

**1d. beacon_state.cpp** — replace `adv_start(false, ...)` calls with
`adv_start_airtag()` / `adv_start_fmdn()`. Replace alternation booleans
with a simpler "which modes are requested" model. The state machine always
says "start both AirTag + FMDN if both enabled" — the hardware impl handles
whether that's simultaneous (ext-adv) or alternating (legacy).

**1e. gatt_glue.c** — `start_settings_adv()` implementation unchanged for
legacy; ext-adv variant creates/starts the connectable set. The `.recycled`
callback stays (calls `beacon_adv_recycled_cb()` on both paths).

**1f. ble_glue.c** — adv data arrays stay. `bt_le_ext_adv_set_data()` uses
the same arrays.

**1g. Gate** — all 11 firmware + 5 DFU build, 290 host tests, lint, Bumble
22 tests. nrf52810 verifies legacy path; nrf52832+ verifies ext-adv path.

### Phase 2: bsim lifecycle tests (1 day)

**2a. Connect/disconnect lifecycle** — new bsim test with central role
(scaffold from `tests/bsim_dfu/src/smp_client.c`). 20 connect/disconnect
cycles. Use a test Kconfig overlay to shorten settings interval
(`kIntervalSettings=5s, kSettingsWait=30s`) so 20 cycles don't need 20
minutes of simulated time. Central verifies adv resumes by scanning between
cycles. Auth-state isolation: attempt unauthorized write after reconnect,
assert rejection.

**2b. Key-rotation race** — extend 2a's central to connect during a key
rotation. Use BabbleSim simulated time control. Assert no stale-mode adv
data after rotation. Note: with legacy API, `bt_disable()` kills the
connection (race is trivially resolved). More interesting with ext-adv
(only one set restarts). Test both paths if feasible.

**2c. Simultaneous adv test** (ext-adv boards only) — verify AirTag +
Settings connectable running simultaneously. Central connects to Settings
while scanner observes AirTag adv data on a second device. This is the
headline ext-adv benefit and needs test coverage.

**2d. flake.nix** — add cmake + run blocks for new bsim test binary.

### Phase 3: GATT handler fuzzing (1 day, parallel with Phase 2)

Independent of adv architecture.

**3a. Fuzz targets** — create `tests/fuzz/` with libFuzzer targets for all
12 write handlers + 1 read handler via `beacon_glue_handle_*` bridge
functions. Two tiers:
- Tier 1: pure validators (`validate_field`, `validate_auth_code`) — fast,
  bounded input, no side effects.
- Tier 2: bridge functions with a real `SettingsManager` + mock
  `INvsStorage` — catches stateful bugs like `keysReceived` counter
  overflow at `zephyr_hardware.cpp:596`.

**3b. Build** — CMakeLists using clang + `-fsanitize=fuzzer,address,undefined`.
Flake app `nix run .#fuzz`.

**3c. Seed corpus** — one valid payload per handler from Bumble tests.

### Phase 4: conn_beacon.py refactor (half day, parallel with Phase 2)

**4a. BeaconClient class** — extract from module-level code. UUID constants
shared with `test_conn_beacon.py`.

**4b. Type hints + click CLI** — replace flat argparse.

**4c. Bumble migration** — replace `simplepyble` with async `bumble`.
CLI wraps async in `asyncio.run()`. Add scan timeout (currently loops
forever if device not found).

**4d. Error handling** — `write_value` calls handle ATT errors.

**4e. Tests** — import `BeaconClient` directly instead of `exec()`. All 22
Bumble tests pass.

## Acceptance Criteria

- [ ] nrf52810 builds + runs on legacy API (no ext-adv, no regression)
- [ ] nrf52832+/nrf54l15 build + run with ext-adv (simultaneous AirTag+FMDN)
- [ ] `.recycled` workqueue restarts connectable adv on both paths
- [ ] No `#ifdef` in beacon_state.cpp or tests — IHardware absorbs the split
- [ ] pauseUpload bug fixed (auth alone doesn't trigger DFU entry)
- [ ] 11 CI firmware + 5 DFU build green
- [ ] 290 host tests + 22 Bumble tests pass
- [ ] bsim: 20-cycle connect/disconnect with adv resumption
- [ ] bsim: key-rotation race passes
- [ ] bsim: simultaneous connectable + non-connectable (ext-adv boards)
- [ ] fuzz: all 13 GATT handlers have libFuzzer targets, 60s clean run
- [ ] conn_beacon.py: BeaconClient class, bumble backend, 22 tests pass

## Risk Analysis

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| nrf52810 RAM overflow with ext-adv | CONFIRMED | nrf52810 stays legacy | IHardware absorbs; `#ifdef` in ZephyrHardware only |
| Key rotation still requires bt_disable | HIGH | Full teardown on all boards | Defer per-set random address to future iteration |
| bsim settings-window timing | MEDIUM | Slow tests | Test overlay shortens intervals |
| pauseUpload bug masks DFU entry issues | CONFIRMED | Unexpected 60s reboot | Phase 0 fix before refactor |

## Sources

- `TODO.md` — item specs
- NCS 3.2.4 migration plan §Future Considerations
- `zephyr/samples/bluetooth/extended_adv/` — ext-adv pattern
- `tests/bsim_dfu/src/smp_client.c` — central-role bsim reference
- `src/zephyr_hardware.cpp` — current adv implementation
- `src/beacon_state.cpp` — state machine transitions
- `src/gatt_glue.c` — GATT handlers + .recycled
- Spec-flow analysis: 24 gaps identified, 8 critical questions resolved
- Feasibility test: `nix build .#firmware-nrf52810` with `CONFIG_BT_EXT_ADV=y`
  → RAM overflow by 2004 B (SDC multirole variant too large)
