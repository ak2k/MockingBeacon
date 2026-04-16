---
title: NCS 2.9.2 → 3.2.4 firmware upgrade (staged)
type: refactor
status: active
date: 2026-04-16
deepened: 2026-04-16
companion: docs/plans/2026-04-16-ncs-3.2.4-research-appendix.md
---

# NCS 2.9.2 → 3.2.4 firmware upgrade (staged)

**Companion doc**: `docs/plans/2026-04-16-ncs-3.2.4-research-appendix.md` — research summaries, decision-reasoning table, symptom→cause map, expanded citations. Consult on-demand when hitting unexpected issues or understanding why a decision was made. THIS plan is the spec; the appendix is reference material.

## Enhancement Summary

**Deepened on:** 2026-04-16 via 10 parallel research+review agents (performance-oracle, security-sentinel, architecture-strategist, code-simplicity-reviewer, data-integrity-guardian, deployment-verification-agent, julik-races-reviewer, spec-flow-analyzer, framework-docs-researcher, best-practices-researcher).

**Key changes vs. v1 of this plan:**
1. **Phase 1 split into 1α (compile-green) + 1β (behavior-green)** for bisectability.
2. **Per-phase merges to `main`**, not single long-lived branch — keeps `main` releasable between phases.
3. **New: `IHardware` contract decision** — `start_*_adv()` remains synchronous via internal semaphore, hiding the workqueue from callers. Phase 1c pseudo-code revised.
4. **New: multi-mode `adv_work_handler`** — dispatches by current adv mode; plan's original hardcoded `BT_LE_ADV_CONN_FAST_1` was wrong for AirTag/FMDN paths.
5. **New: `authorizedGatt` must be cleared in `.recycled`** (not just `.disconnected`) to prevent inherited auth across rapid reconnects.
6. **Pre-existing security finding flagged and deferred to TODO.md** — MCUboot signing uses upstream dev key (`CONFIG_BOOT_SIGNATURE_KEY_FILE` unset). Decided 2026-04-16: keep dev key for prebuilds; track as post-migration issue.
7. **Phase 1g RAM audit restructured** — measurement-first (`nm --size-sort` diff + `.config` diff + `size -A`) BEFORE applying Kconfig levers. Dropped misapplied levers (`MBEDTLS_PSA_KEY_SLOT_COUNT`, `BT_CTLR_DATA_LENGTH_MAX`, `BT_BUF_ACL_TX_SIZE`); added higher-impact ones (`LOG_BUFFER_SIZE`, `CBPRINTF_REDUCED_INTEGRAL`, `BT_HCI_ACL_FLOW_CONTROL=n`, `BT_LONG_WQ=n`).
8. **Phase 1c stack budget**: dedicated workqueue (K_THREAD_STACK 768 B) chosen; `SYSTEM_WORKQUEUE_STACK_SIZE=1280` is fallback if the 768 B allocation pushes nrf52810 bss over budget.
9. **Extract NVS ID enum to shared header** so `tests/zms_persist/` iterates production IDs, not a stale hand-picked subset.
10. **Stale-memory correction**: `nix build .#firmware-nrf52832-dfu` works on `main` (fixed by commit 42793bf). Plan no longer treats this as a blocker.
11. **DFU matrix expanded** to match `release.yml` (5 variants: 52832, 52833, 52840, 54l15, thingy52).
12. **Deployment checklist added**: pre-merge gates, post-merge verification commands, rollback SOP, monitoring plan, developer communication draft.
13. **Per-phase `.config` snapshot & critical-module hash-diff** — catches silent Kconfig default flips and west2nix supply-chain drift on crypto modules.
14. **Per-commit bisect granularity required** — promoted from "reviewer preference" to requirement.

**Cut for simplicity (per code-simplicity-reviewer):** 3 of 6 opportunistic fold-ins (`bt_hci_err_to_str`, `BUILD_ASSERT(HCI_VS)`, comment cleanup — moved to TODO.md); 3 risk-table padding rows; 5 soft acceptance criteria; Phase 2 rollback ceremony.

---

## Overview

Upgrade this Zephyr/NCS firmware from NCS **2.9.2** (Zephyr 3.7.99) to NCS **3.2.4** (Zephyr 4.2.99) — three NCS majors and three upstream Zephyr majors (3.7 → 4.0 → 4.1 → 4.2).

A WIP straight-jump attempt existed on `upgrade-ncs-3.2.4` (commit `c46c3d7`) that stalled on a Kconfig build-assert. This plan **discards that branch** in favor of a staged 2.9 → 3.0 → 3.1 → 3.2 migration. Five lines of real semantic work from the WIP will be reproduced in the appropriate stages.

This is a **greenfield migration — no deployed devices to preserve data for**. That removes one major risk class (ZMS settings-backend legacy compatibility). Remaining migration risks: advertising lifecycle regression, RAM overflow on nRF52810, MCUboot runtime slot-selection under auto-assigned IDs, and workqueue-race-induced auth inheritance. (Pre-existing MCUboot dev-key signing is tracked separately in TODO.md and is NOT in migration scope.)

## Problem Statement

**Why now:** Weekly `ncs-upgrade` workflow flags 3.2.4 availability. NCS 2.9.x enters community maintenance when 3.4 ships. Waiting compounds breaking-change debt.

**Why this is hard:** Three Zephyr major bumps carry several unrelated breaking changes that compound during debugging:

- **Zephyr 4.0** — `BT_LE_ADV_OPT_CONNECTABLE` renamed to `BT_LE_ADV_OPT_CONN` with a behavioral change: **advertising no longer auto-restarts after disconnect**. Firmware that connects once goes silent unless it implements the new `.recycled` callback + workqueue pattern (Zephyr `include/zephyr/bluetooth/conn.h:2123-2136`).
- **Zephyr 4.1** — `CONFIG_BT_BUF_ACL_RX_COUNT` deprecated in favor of auto-computation (`BT_MAX_CONN + 1`). Hand-tuned prj.conf values become wrong.
- **Zephyr 4.2** — `bt_hci_cmd_create()` replaced by `bt_hci_cmd_alloc()` (opcode moved to send). GATT CCC struct rename (`_bt_gatt_ccc` → `bt_gatt_ccc_managed_user_data`) — transparent for `BT_GATT_CCC` macro consumers (our case).
- **NCS 3.0** — ZMS settings backend is new (no migration issue for us — greenfield). Child-image multi-image builds replaced by sysbuild (we already use sysbuild).
- **NCS 3.2** — **MCUboot image IDs now auto-assigned** via `CONFIG_MCUBOOT_APPLICATION_IMAGE_NUMBER`; hardcoded slot IDs in application code crash at swap (DevZone 127216 class). Partition Manager static file format changed.

**Why straight-jump failed:** The WIP hit `CONFIG_BT_BUF_EVT_RX_COUNT` build-assert without addressing any cross-cutting concern (ZMS backend, image IDs, RAM audit, advertising resumption). Fixing the assert alone would have uncovered the next gotcha, then the next.

## Proposed Solution

**Discard the WIP branch.** Start fresh from `main` with per-phase branches and per-phase merges.

**Why incremental, not straight-jump:** Three independent sources (Nordic DevZone, Zephyr migration docs, Argenox) recommend sequential per-release migration. Each stage has unrelated breaking changes; bundling triples debug time. Stage boundaries are natural bisect points.

**Why per-phase merges (revised):** `main` has an active release cadence (weekly `ncs-upgrade` workflow, release matrix with 22 variants). A 5-day frozen `main` blocks releases. Each phase produces a valid intermediate state (NCS 3.0.2, then 3.1.x, then 3.2.4) — not "broken-by-design" intermediates. Merged history gives real bisect points post-hoc; a squashed 5-day branch destroys that.

**Fold small improvements INTO migration where coupled, defer otherwise.** Required (coupled to Zephyr 4.0): `.recycled` + workqueue pattern, `BT_LE_ADV_CONN_FAST_1` macro. Fold: dead-Kconfig cleanup (file already being touched). Move to TODO.md: `bt_hci_err_to_str` log improvements, `BUILD_ASSERT(IS_ENABLED(CONFIG_BT_HAS_HCI_VS))`, comment cleanups. Also deferred in TODO.md: `bt_le_ext_adv_*` API for simultaneous adv sets.

## IHardware contract decision (PRE-PHASE-1 CALL)

**Decision:** `IHardware::start_settings_adv()`, `start_airtag_adv()`, `start_fmdn_adv()`, `start_ibeacon_adv()` remain **synchronous** from the caller's perspective. The workqueue-based restart machinery is internal to `zephyr_hardware.cpp`.

**Why:** `StateMachine` (`src/beacon_state.cpp:52-83, 213-240, 301-330, 356-388, 392-417`) calls adv-start methods synchronously and assumes the advertiser is running when the call returns. Making these methods async would change the contract and require auditing every caller. A semaphore-based barrier inside `zephyr_hardware.cpp` preserves the current contract at the cost of a few hundred bytes and a context switch per call.

**Implementation sketch** (reference only, actual code in Phase 1c):
```cpp
// zephyr_hardware.cpp
static struct k_sem adv_started_sem;
static AdvMode pending_mode;

void ZephyrHardware::start_settings_adv() {
  pending_mode = AdvMode::Settings;
  k_work_submit(&adv_work);
  k_sem_take(&adv_started_sem, K_MSEC(500));
}
```

The `.recycled` callback also submits `adv_work` but does NOT take the semaphore (the state machine doesn't wait for recycled-triggered restarts). This separates "intentional start" from "recovery restart" while reusing the handler.

## Technical Approach

### Architecture

No architectural change to `IHardware`/`StateMachine`/`BeaconConfig` layers. Changes are localized to Zephyr-facing implementation files plus Kconfig and build glue.

### Implementation Phases

#### Phase 0: Pre-flight (2-3 hours)

**Setup:** Create worktree `/tmp/claude-ncs-upgrade/wt-phase0` off `main` on branch `ncs-upgrade/phase-0-baselines`. (WIP archive tagging + deletion handled per Execution Model section.)

**Tasks:**
- **Baseline captures** (all committed to `docs/baselines/` on the phase-0 branch for later diffs):
  - Text/data/bss per board: `nix build .#firmware 2>&1 | tee docs/baselines/sizes-2.9.2.log`
  - Resolved `.config` per board: for nrf52810, nrf52832dk, nrf54l15dk — `cp build-*/zephyr/.config docs/baselines/config-<board>-2.9.2`
  - Top-60 symbol sizes: `arm-none-eabi-nm --print-size --size-sort --radix=d build-810/zephyr/zephyr.elf | tail -60 > docs/baselines/symbols-nrf52810-2.9.2.txt`
  - Test suite: `nix build .#bsim-test && nix run .#test && uv run --with bumble --with pytest --with pytest-asyncio pytest tests/ble_client/ -v` — commit result logs.
  - **Verify DFU builds green on main**: `nix build .#firmware-nrf52832-dfu .#firmware-nrf52833-dfu .#firmware-nrf52840-dfu .#firmware-nrf54l15-dfu .#firmware-thingy52-dfu`. All 5 must succeed (commit 42793bf on 2026-04-15 fixed the prior `zephyr_module.py TypeError`).
- **Signing key fingerprint**: capture `sha256sum sysbuild/mcuboot/boards/keys/*.pem 2>/dev/null || echo "NO KEY FILE"` — expected: no key file. See "Security sub-task" below.
- **`nrfutil device` in dev shell** (DEFERRED — hardware-only): `nrfutil` is a hardware flashing/communication tool. Migration is simulation-only (per §Dependencies); nrfutil isn't required for builds, simulation tests, or PR merges. Tracked in TODO.md "Hardware DFU swap validation" entry — to be addressed when hardware arrives. Decision 2026-04-16: do not block Phase 1 on nrfutil packaging (segger-jlink unfree license + Linux-only; non-trivial flake.nix design decision).

**Security note — pre-existing dev-key signing (decided: keep):**
`sysbuild/mcuboot.conf` enables `BOOT_SIGNATURE_TYPE_ECDSA_P256` but does NOT set `CONFIG_BOOT_SIGNATURE_KEY_FILE`. MCUboot falls back to the upstream dev key (`bootloader/mcuboot/root-ec-p256.pem`), which is public. Anyone can sign a DFU image. This is a pre-existing dev-build limitation (README.md:138 documents). **Decision (2026-04-16): keep dev key for prebuilts; track in TODO.md as a separate post-migration issue.** The migration will not change signing config. Phase 3's signature-isolation test is therefore SKIPPED; DFU is not a production trust boundary in this build.

---

#### Phase 1: NCS 2.9.2 → 3.0.2 (3-4 days)

Split into 1α (compile-green) and 1β (behavior-green) for bisect granularity.

**Sub-task numbering:** 1α contains sub-tasks `1a`, `1b`, `1d`, `1e`, `1f` (build system); 1β contains `1c` (advertising behavior) and `1g` (RAM audit). `1h` is 1β's gate. Sub-task `1c` is intentionally in 1β — it's behavioral, not build-system.

##### Phase 1α — build-system migration

**1a. Lockfile bump** — `west.yml` → `v3.0.2`; regenerate `west2nix.toml`, `bsim-west2nix.toml`. One commit.

**1b. Widen `flake.nix` chmod hook** — preserve existing `git init` block at `flake.nix:107-111` (it's what makes DFU builds work on main — do not remove). Widen chmod scope:
```nix
find zephyr/scripts bootloader/mcuboot/scripts modules -type f -name '*.py' -exec chmod +x {} +
```
One commit. **Verification**: `nix build .#firmware-nrf52832-dfu` still succeeds (proves we didn't regress 42793bf).

**1d. Kconfig updates** (moved out of 1c — belongs in build-green phase):
- `prj.conf:59` — `CONFIG_BT_BUF_EVT_RX_COUNT=2` → `=4` (defined constraint: `BUILD_ASSERT` at `zephyr/include/zephyr/bluetooth/buf.h:168-172`; strictly `>`, so `ACL_TX=3` → `EVT_RX ≥ 4`).
- `prj.conf:42` — audit `CONFIG_BT_DEBUG_NONE`; if Kconfig warns, replace with `CONFIG_BT_LOG_LEVEL_OFF=y`.
- `prj.conf:49` — delete dead `CONFIG_BT_SETTINGS_CCC_LAZY_LOADING=n`.
- New RAM-savers (framework-docs + perf-oracle): `CONFIG_LOG_BUFFER_SIZE=512` (saves up to 768 B bss vs. 4.x default 1024), `CONFIG_CBPRINTF_REDUCED_INTEGRAL=y` (saves 400-800 B text), `CONFIG_BT_HCI_ACL_FLOW_CONTROL=n` (~150 B for single-conn), `CONFIG_BT_LONG_WQ=n` if nothing auto-selects it (saves up to 1 KB).
One commit.

**1e. ZMS explicit enables + shared ID header**:
- `prj.conf`: explicit `CONFIG_ZMS=y`, `CONFIG_NVS=n`. Add `CONFIG_ZMS_LOOKUP_CACHE=y` + `CONFIG_ZMS_LOOKUP_CACHE_FOR_SETTINGS=y` + `CONFIG_ZMS_LOOKUP_CACHE_SIZE=16` (128 B RAM; jump to 64 = 512 B on nrf54l15 only).
- Extract NVS ID enum from `src/beacon_config.hpp` into `src/beacon_nvs_ids.hpp` and include from both `beacon_config.hpp` and `tests/zms_persist/src/main.c`. Make the bsim test iterate **every** production ID, not a hand-picked subset. (Data-integrity finding #3: the test currently uses `ID_TIME=0x0B` but production uses `ID_status_NVS=0x0B` — test drift.)
- Clarify (comment in `prj.conf`): `CONFIG_SETTINGS_ZMS_MAX_COLLISIONS_BITS` is N/A — we use direct `zms_read`/`zms_write` with hand-assigned uint16_t IDs, not `BT_SETTINGS`.
One or two commits.

**1f. Sysbuild sanity** — confirm no `child_image/` overlays (`ls child_image/ 2>/dev/null` empty). `dfu.conf:50` `CONFIG_MCUMGR_TRANSPORT_BT_DYNAMIC_SVC_REGISTRATION=y` — verify Kconfig still has symbol. One commit only if changes needed.

**1α gate (must pass to proceed to 1β):**
- `nix build .#firmware` — all 11 CI attrs cross-compile
- `nix build .#firmware-nrf52832-dfu .#firmware-nrf52833-dfu .#firmware-nrf52840-dfu .#firmware-nrf54l15-dfu .#firmware-thingy52-dfu` — all 5 DFU variants
- `nix run .#test` — host 290 assertions green
- `nix run .#lint` — no drift
- **Snapshot `.config` for nrf52810, nrf52832dk, nrf54l15dk** — `cp build-*/zephyr/.config docs/baselines/config-<board>-3.0.2`. Diff against Phase 0 baseline; any unexpected change in `CONFIG_BT_*`, `CONFIG_BOOT_*`, `CONFIG_MBEDTLS_*`, `CONFIG_PSA_*` must be explained in commit message.
- **Critical-module hash-diff** (security finding): diff `west2nix.toml` lines matching `hal_nordic`, `nrfxlib`, `mbedtls`, `trusted-firmware-m`, `mcuboot` — one-line "why changed" note per change in commit.

**Merge 1α to `main` via PR.** Release cadence preserved.

##### Phase 1β — behavior migration (the `.recycled` work)

This is the highest-complexity phase. Per julik-races review, the naive implementation (hardcoded `BT_LE_ADV_CONN_FAST_1` in handler) is wrong for this firmware's multi-mode reality.

**1c. Advertising lifecycle** — files touched: `src/ihardware.hpp`, `src/zephyr_hardware.cpp`, `src/gatt_glue.c`, `src/beacon_state.cpp`.

**Step 1** — `src/ihardware.hpp`: preserve current synchronous method signatures (no interface change; see "IHardware contract decision" above).

**Step 2** — `src/zephyr_hardware.cpp`: add mode-state + workqueue + semaphore. Pseudo-code:
```cpp
enum class AdvMode { None, Settings, AirTag, FMDN, iBeacon };
static AdvMode current_adv_mode_ = AdvMode::None;
static AdvMode pending_adv_mode_ = AdvMode::None;

// Dedicated workqueue (performance-oracle #4)
K_THREAD_STACK_DEFINE(adv_wq_stack, 768);
static struct k_work_q adv_wq;
static struct k_work adv_work;
static struct k_sem adv_started_sem;

static bool adv_allowed() {  // julik-races #3
  // Read state from StateMachine; deny in ShuttingDown / FirmwareUpload.
}

static void adv_work_handler(struct k_work *work) {
  if (!adv_allowed()) return;

  const struct bt_le_adv_param *params = nullptr;
  struct bt_data *ad = nullptr;
  size_t ad_len = 0;
  switch (pending_adv_mode_) {
    case AdvMode::Settings: params = BT_LE_ADV_CONN_FAST_1; /* ... */ break;
    case AdvMode::AirTag:   /* non-connectable, AirTag-specific params */ break;
    case AdvMode::FMDN:     /* non-connectable, FMDN params */ break;
    case AdvMode::iBeacon:  /* non-connectable, iBeacon params */ break;
    default: return;
  }

  // Bounded retry (julik-races #4)
  for (int attempt = 0; attempt < 5; ++attempt) {
    int err = bt_le_adv_start(params, ad, ad_len, NULL, 0);
    if (err == 0) { current_adv_mode_ = pending_adv_mode_; k_sem_give(&adv_started_sem); return; }
    if (err != -EAGAIN && err != -ENOMEM) { /* log + bail */ return; }
    k_sleep(K_MSEC(100));
  }
  /* log escalation */
}

static void recycled_cb(void) {
  // Re-submit whatever mode should be current post-disconnect.
  k_work_submit_to_queue(&adv_wq, &adv_work);
}
```

**Step 3** — `src/gatt_glue.c:99-102`: register `.recycled = recycled_cb` on `bt_conn_cb`. Also add **security finding fix**: clear `authorizedGatt=0` in `.recycled` (defense-in-depth) — not just in `.disconnected`. Without this, a workqueue race where `recycled` fires before `disconnected` cleanup completes could let a new client inherit prior auth.

**Step 4** — `src/beacon_state.cpp`: every adv-mode-changing transition (key rotation @L378, settings entry/exit @L213, UVLO shutdown @L260-284, firmware upload entry @L301-330, alternation @L356-388) must call `k_work_cancel_sync(&adv_work)` BEFORE calling `IHardware::stop_*_adv()` or `bt_disable()` (julik-races #2). Prevents stale queued work from running in the wrong mode post-transition.

**Fold-in (coupled):** Replace magic adv-interval numbers at `gatt_glue.c:375` with `BT_LE_ADV_CONN_FAST_1` macro (intervals 30-60 ms, verified at `zephyr/include/zephyr/bluetooth/bluetooth.h` via framework-docs research; appropriate for connectable settings-mode beacon wanting fast rediscovery).

**1g. nRF52810 RAM audit** (restructured per perf-oracle).

**Preemptive stack pinning** (Step 1) — pin these in `prj.conf` BEFORE first 3.0.2 build to prevent Zephyr 4.x defaults from flipping silently:
```
CONFIG_MAIN_STACK_SIZE=1024       # keep
CONFIG_IDLE_STACK_SIZE=256        # up from 128; 4.x init path tighter with canaries
CONFIG_BT_RX_STACK_SIZE=1024      # keep; 4.0 bumped default but we don't use crypto
CONFIG_BT_HCI_TX_STACK_SIZE=768   # up from 640; 4.2 bt_hci_cmd_alloc path tighter
CONFIG_ISR_STACK_SIZE=1024        # keep
CONFIG_BT_LONG_WQ=n               # 4.1 added this workqueue; disable if nothing selects
CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=1024   # keep; adv_work uses dedicated queue
```

**Measurement-first audit** (Step 2):
1. Build 3.0.2 nrf52810 with stack pinning above.
2. Diff resolved `.config` vs. Phase 0 baseline; categorize every change (expected from bump / new default / regression).
3. `arm-none-eabi-nm --print-size --size-sort --radix=d build-810/zephyr/zephyr.elf | tail -60 | diff - docs/baselines/symbols-810-2.9.2.txt` — symbols with >100 B growth are the real targets.
4. `arm-none-eabi-size -A build-810/zephyr/zephyr.elf` — attribute delta per subsystem (bt_host, bt_ll, logging, settings, net_buf_pool).
5. ONLY THEN apply Kconfig levers.

**Kconfig levers (corrected list)**:
- Pre-audit Kconfig additions already in 1d (LOG_BUFFER_SIZE, CBPRINTF_REDUCED_INTEGRAL, BT_HCI_ACL_FLOW_CONTROL, BT_LONG_WQ).
- `CONFIG_BT_CTLR_DUP_FILTER_LEN=0` if `CONFIG_BT_OBSERVER=n` (scanner-less peripheral) — saves 200-400 B bss.
- `CONFIG_BT_CONN_TX_MAX=2` (explicit, match BT_L2CAP_TX_BUF_COUNT).
- **NOT applicable** (previous plan had these; removed per perf-oracle): `MBEDTLS_PSA_KEY_SLOT_COUNT` (no PSA linked without SMP on non-DFU path — verify via `.config` diff; applies only if PSA surfaces in DFU variant), `BT_CTLR_DATA_LENGTH_MAX` (we have `BT_DATA_LEN_UPDATE=n`), `BT_BUF_ACL_TX_SIZE` (already at 27, not 251).

**Runtime verification** (Step 3):
- **bsim THREAD_ANALYZER**: new overlay `tests/bsim/overlay-thread-analyzer.conf` with `CONFIG_THREAD_ANALYZER=y + CONFIG_THREAD_ANALYZER_AUTO=y + CONFIG_THREAD_ANALYZER_RUN_UNLOCKED=y`. 60-second adv + 3× connect/disconnect cycle. Fail if any thread reports <20% stack headroom.
- **Link-time**: `arm-none-eabi-size build-810/zephyr/zephyr.elf`; bss ≤ 23 KB hard / 22 KB target; text ≤ baseline × 1.03 target / × 1.05 hard.

**1h. 1β gate — behavioral tests (simulation-only; hardware unavailable — see TODO.md)**:
- All 1α gate items
- **Rapid connect/disconnect** — Bumble virtual BLE against `nrf52_bsim` firmware: 20-iteration connect/disconnect with 200ms dwell; advertising MUST resume between iterations. Catches `.recycled` wiring bugs.
- **Auth-inheritance check** — Bumble rapid-reconnect asserts writes WITHOUT prior auth are rejected on every fresh connection (no state leaks across recycle).
- **Connect/disconnect DURING key rotation** (julik-races #2): schedule connection right after `beacon_state.cpp:378` key-rotation transition; ensure no double-start or stale-mode advertising.
- `nix build .#bsim-test` green including `zms_persist` overlay that iterates ALL production NVS IDs.
- `nix build .#bsim-test` green including new `overlay-thread-analyzer.conf`: all threads ≥ 20% stack headroom.
- `uv run ... pytest tests/ble_client/ -v` green with all new tests (rapid-reconnect, auth-inheritance, key-rotation-during-churn).

**Merge 1β to `main` via PR.** Firmware is now on NCS 3.0.2 with working advertising lifecycle.

---

#### Phase 2: NCS 3.0.2 → 3.1.x (1 day)

Lightweight gate (no full Bumble re-run — NCS 3.0→3.1 is housekeeping per official migration guide).

**Files:**
- `west.yml` → `v3.1.x`; regenerate lockfiles.
- `prj.conf` — **remove** any `CONFIG_BT_BUF_ACL_RX_COUNT` (deprecated 4.1; auto-computed as `BT_MAX_CONN + 1` = 2 for us); rename `BT_DIS_MODEL/MANUF` to `_STR` variants if set (grep first; likely no-op).
- `sysbuild.conf` — `grep SB_CONFIG_MCUBOOT_MODE_SWAP_WITHOUT_SCRATCH sysbuild.conf` first; if present, rename to `SB_CONFIG_MCUBOOT_MODE_SWAP_USING_MOVE`; if absent, no-op.
- **Custom board DTS drift check** (community finding #3): diff each `boards/arm/<board>/*.dts` against upstream `nrf52*_common.dtsi` at the 3.1 checkpoint. Any silent base-partition size change must be explained.
- **Re-run RAM audit subset**: rebuild nrf52810; compare `size -A` vs. 3.0.2 baseline. Auto-computed ACL_RX_COUNT should reduce RAM by ~4 buffers × 260 B = ~1 KB. If RAM grew, investigate before proceeding.

**Gate**: `nix build .#firmware` + `nix run .#test` + `nix run .#lint`. (Bumble suite not re-run; no behavioral changes in 3.1 for our code paths.)

**Merge to `main`.**

---

#### Phase 3: NCS 3.1.x → 3.2.4 (2-3 days)

**Files:**
- `west.yml` → `v3.2.4`; regenerate lockfiles.
- `src/zephyr_hardware.cpp:170-197` — `bt_hci_cmd_create(opcode, len)` → `bt_hci_cmd_alloc(K_FOREVER)`; opcode moves to `bt_hci_cmd_send_sync(opcode, buf, rsp)`. Pattern matches `samples/bluetooth/hci_pwr_ctrl/src/main.c:82-111`.

**MCUboot image ID audit (CRITICAL — per community + framework-docs):**

```bash
grep -rn "flash_img_init\|fixed_partition_id\|FIXED_PARTITION_ID\|FIXED_PARTITION_OFFSET\|flash_area_open\|MCUBOOT_IMAGE_NUMBER\|PM_S[01]_ID\|PM_MCUBOOT_PAD_\|MCUBOOT_SCRATCH" src/ boards/
```

Expected: no hits in our `src/`. If any surface, refactor to use `CONFIG_MCUBOOT_APPLICATION_IMAGE_NUMBER` (framework-docs #3 — ID auto-assigned by sysbuild, readable via generated Kconfig). Runtime inspection of installed images uses `img_mgmt_read_info()` from `zephyr/include/zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt.h:247`.

Grep sysbuild build log for the assignment line: `nix build .#firmware-nrf52832-dfu 2>&1 | grep "Sysbuild assigned MCUboot image IDs"`. Record in commit message.

**Partition Manager check**: `find . -name 'pm_static.yml' -o -name 'pm.yml'` — expected empty (custom boards use DTS `fixed-partitions`). If any appear (especially auto-generated under `build*/`), halt and audit — NCS 3.2 sysbuild-PM integration can silently synthesize `pm.yml` that overrides DTS offsets.

**nRF54L15-specific DFU check** (community finding #8): confirm `sysbuild.conf` or board-specific overlay sets `CONFIG_BOOT_MAX_IMG_SECTORS=256` for nrf54l15-dfu variant. Default won't fit larger flash page structure.

**Signature isolation test** — SKIPPED per Phase 0 decision to keep dev-key signing. Re-enable if/when `CONFIG_BOOT_SIGNATURE_KEY_FILE` is set in the follow-up issue.

**GATT authorization callback audit** (security criterion #3): read `struct bt_gatt_authorization_cb` layout from NCS 3.2 `zephyr/include/zephyr/bluetooth/gatt.h`. If any new field was appended, the designated-initializer in `src/gatt_glue.c:300-318` zero-initializes it — review what the zero default means per field.

**Kconfig check (mechanical grep, expected clean):**
- `grep -rn "_bt_gatt_ccc\|BT_GATT_CCC_INITIALIZER" src/` → expect no hits (we use `BT_GATT_CCC(...)` macro; transparent rename per framework-docs #8).
- `grep -rn "CONFIG_MP_NUM_CPUS\|K_THREAD_STACK_MEMBER\|ceiling_fraction\|random/rand32.h\|net/buf.h" src/` → expect clean.

**Post-DFU BT enable retest** (community finding #6): integration test must exercise MCUmgr-triggered reset, not just swap. After swap, verify `bt_enable()` returns 0 on first attempt. If `-EAGAIN`, drop `BT_BUF_ACL_TX_SIZE` and `BT_CTLR_DATA_LENGTH_MAX` together (never asymmetric).

**Gate (simulation-only; hardware DFU validation deferred to TODO.md):**
- 1β gate items, all boards
- `nix build .#firmware` + all 5 DFU variants green
- **bsim_dfu**: existing MCUmgr SMP echo test green on 3.2.4 (catches transport regressions)
- **Sysbuild log check**: `nix build .#firmware-nrf52832-dfu 2>&1 | grep "Sysbuild assigned MCUboot image IDs"` — record assigned IDs in commit message. No hardcoded hits in `src/`/`boards/` per grep audit.
- **imgtool verify** on each signed image: `imgtool verify build/*/zephyr/zephyr.signed.bin` — catches signature format regression at build time.
- **Image-fits-slot check**: parse sysbuild partitions YAML; assert `image_size + header + trailer ≤ slot_size − 0x200` for all DFU variants.
- `uv run ... pytest tests/ble_client/ -v` green (add `test_post_dfu_bt_enable` — post-bsim-dfu-swap, re-init BT, assert returns 0 first attempt).

**Known residual risk (accepted with TODO-tracked hardware validation):** runtime MCUboot slot selection under auto-assigned image IDs (DevZone 127216 class) is NOT fully simulation-testable. Merged with theoretical-unless-hardware-validated flag. See TODO.md for hardware follow-up.

**Merge to `main`.**

---

#### Phase 4: Polish + CI (½ day)

- `grep -rn "2\.9\.\|v2\.9\|ncs-2" .github/ flake.nix west.yml docs/ README.md CLAUDE.md` → zero hits (except baseline logs).
- `release.yml:171` `awk` parse still works for new `west.yml` format.
- **DFU slot sizes in `release.yml:186-189`** — re-measure from Phase 3 sysbuild output (`grep -A2 'app_image' build/dfu_*/zephyr/partitions*.yml`) and update.
- Update CLAUDE.md NCS version + add "Migrating from 2.9.2 workspace" one-liner (`rm -rf ../zephyr ../nrf ../modules ../bootloader ../tools && west update`).
- Update TODO.md:
  - Remove the completed "Upgrade to nRF Connect SDK v3.x" entry
  - Retain: extended-adv API refactor, hardware DFU swap validation, production MCUboot signing key, fuzz GATT handlers, refactor conn_beacon.py
  - Add new deferred fold-ins: `bt_hci_err_to_str(reason)` in disconnect log, `BUILD_ASSERT(IS_ENABLED(CONFIG_BT_HAS_HCI_VS))` at top of zephyr_hardware.cpp HCI command block, minor log cleanup.

---

### Branch / PR strategy

See "Execution Model — Agent Team" section below for the authoritative branch, worktree, and PR model.

### Rollback criteria

**Per-stage abort (plus bisect runbook):**
- 1β Bumble rapid-connect fails → revert 1β commits; keep 1α merged. Investigate `.recycled` wiring on dedicated investigate-branch.
- 1β nrf52810 bss > 23 KB → halt; `nm` diff must identify culprit before relaxing budget.
- Phase 3 bsim_dfu or imgtool verify fails → **do not merge Phase 3**; audit for hardcoded slot IDs in `src/`/`boards/` or any partition API consumer.
- Any phase triggers unexplained `.config` drift in crypto/security Kconfig → halt; explain or revert.

**Negative acceptance criteria** (architecture-strategist #4): at each phase gate, text-size delta on unchanged boards (nrf52840dk) must be <1%. Silent default flips propagate otherwise.

### Deployment (merge of final phase)

See "Deployment Checklist" section below.

## Alternative Approaches Considered

### A. Straight-jump to 3.2.4 (the WIP approach)
**Rejected.** WIP demonstrated the failure mode: one Kconfig assert surfaces, the rest of cross-cutting issues stay hidden. Per-stage wall-clock cost is small versus debuggability gain.

### B. Continue the WIP branch in place
**Rejected.** WIP's real diff is 5 lines. Liabilities (narrow chmod, 267-line reflow in gatt_glue.c, missing `.recycled`) outweigh salvage value.

### C. Single long-lived branch with final-PR-only merge
**Rejected (revision from v1 of plan).** Main has active release cadence; 5-day freeze blocks releases. Intermediate states (NCS 3.0.2, 3.1.x) are valid, not broken-by-design. Merged history gives real bisect points.

### D. Pause, wait for NCS 3.3
**Rejected.** 3.3 carries 2.9→3.2 changes PLUS Zephyr 4.4 ZMS→kvss move. Waiting compounds.

## System-Wide Impact

### Interaction Graph (revised — multi-mode adv)

```
BLE client disconnects
  → Zephyr BT host fires .disconnected callback
     → gatt_glue.c clears authorizedGatt (existing)
  → conn object released by host
  → .recycled callback fires
     → gatt_glue.c clears authorizedGatt again (NEW — defense in depth)
     → zephyr_hardware.cpp recycled_cb submits adv_work to dedicated adv_wq
  → adv_work_handler runs on dedicated workqueue thread
     → adv_allowed() checks StateMachine state
     → dispatches based on current_adv_mode_ (Settings/AirTag/FMDN/iBeacon)
     → bt_le_adv_start() with correct per-mode params, bounded retry on -EAGAIN/-ENOMEM
     → on success: give adv_started_sem (for sync callers)
```

State machine transitions (key rotation, mode switch, UVLO, DFU entry):
```
StateMachine::transition() {
  k_work_cancel_sync(&adv_work);   // NEW — prevent stale queued work
  hw_.stop_*_adv();
  bt_disable(); bt_enable();  // or similar
  hw_.start_*_adv();           // synchronous via internal semaphore
}
```

### Error & Failure Propagation
- `bt_le_adv_start` in workqueue context: bounded-retry, escalate to log on exhaustion. No infinite retry.
- Sync semaphore timeout 500ms: if workqueue hangs, caller returns without blocking forever. State machine continues.
- ZMS `-ENOENT`/`-EDEADLK` flow through `SettingsManager::load` → defaults (existing, unchanged).
- MCUboot image ID bug: runtime crash on DFU swap. No compile signal. **bsim_dfu + imgtool verify + sysbuild log grep + hardcoded-ID grep in src/boards cover build-time risk; runtime slot-selection risk accepted pending hardware DFU validation (TODO.md).**

### State Lifecycle Risks
- Adv state machine flag churn: `k_work_cancel_sync` before every mode transition prevents stale work. Bumble test covers.
- Auth-inheritance: clearing `authorizedGatt` in BOTH `.disconnected` AND `.recycled` prevents the race-window variant.
- ZMS fresh-flash behavior: greenfield assumption — `west flash --erase` required on dev boards that had pre-3.x firmware, to avoid stale NVS bytes being misread as ZMS ATEs (data-integrity #4).

### API Surface Parity
Agent-accessible surface (`conn_beacon.py` → GATT) unchanged. All 13+ characteristics retain UUIDs and payload formats.

### Integration Test Scenarios

Must pass before merging respective phase. Each catches a distinct failure class:

All simulation-only (hardware unavailable; hardware validation tracked in TODO.md):

1. **Rapid connect/disconnect cycles** — Bumble virtual BLE against `nrf52_bsim` firmware, 20 iterations with 200ms dwell. **Phase 1β gate.** Catches `.recycled` wiring.
2. **Connect/disconnect DURING key rotation** — new test extending `tests/ble_client/test_conn_beacon_integration.py`; races state-machine transition against conn-object lifecycle. **Phase 1β gate.**
3. **Auth state cleared across recycle** — Bumble: client A authorizes, disconnects; client B connects, must NOT be authorized. **Phase 1β gate.**
4. **bsim_dfu MCUmgr swap path** — existing bsim test re-run on 3.2.4, verifies SMP transport + image upload. **Phase 3 gate.** (Does NOT fully exercise MCUboot boot-path slot selection — see residual risk note under Phase 3.)
5. **Post-swap `bt_enable()`** — extend bsim_dfu to re-init BT after the swap simulation, assert returns 0 first attempt (catches community finding #6). **Phase 3 gate.**
6. **ZMS persistence across reboot** — bsim `zms_persist` now iterating ALL production NVS IDs. **Every-phase gate.** Catches ZMS API drift AND test-vs-production ID drift.
7. **Full GATT settings matrix** via `conn_beacon.py` against Bumble-virtual — all 15 CLI options via existing `tests/ble_client/test_conn_beacon*.py`. **Phase 1β + Phase 3 gate.**
8. **THREAD_ANALYZER stack HWM** — new bsim overlay; 60s adv + 3× connect/disconnect. Fail if any thread < 20% headroom. **Phase 1β gate.**
9. **Rapid-GATT-write-during-notification** — new Bumble test; catches Zephyr 4.1 auto-computed ACL_RX_COUNT=2 queue-depth regression. **Phase 2 gate.**
10. **imgtool verify + image-fits-slot** — build-time checks on all 5 DFU variants. **Phase 3 gate.**
11. **Signature isolation** — SKIPPED (dev-key signing retained per Phase 0 decision; re-enable in follow-up issue when `CONFIG_BOOT_SIGNATURE_KEY_FILE` is set).

## Acceptance Criteria (trimmed per simplicity-reviewer)

### Functional Requirements
- [ ] All 11 CI board variants cross-compile under NCS 3.2.4
- [ ] All 5 DFU variants build (nrf52832, nrf52833, nrf52840, nrf54l15, thingy52)
- [ ] Advertising resumes automatically after BLE disconnect in Bumble-virtual + `nrf52_bsim` 20-cycle test
- [ ] Advertising correct mode after key rotation (no stale-mode advertising)
- [ ] New connections never inherit prior `authorizedGatt` state
- [ ] All 15 `conn_beacon.py` CLI options work against Bumble-virtual post-migration
- [ ] bsim_dfu MCUmgr swap path green on 3.2.4 (transport + upload verified; hardware boot-path swap in TODO.md)
- [ ] Post-swap `bt_enable()` returns 0 first attempt (bsim_dfu extended test)
- [ ] ZMS persistence across reboots (bsim `zms_persist` green for ALL production NVS IDs)

### Non-Functional Requirements
- [ ] nrf52810 bss ≤ 23 KB hard / 22 KB target
- [ ] nrf52810 flash text delta ≤ +3% target / +5% hard vs. 2.9.2 baseline
- [ ] bsim THREAD_ANALYZER: all threads ≥ 20% stack headroom

### Quality Gates
- [ ] Host-native tests (290 assertions) green
- [ ] All bsim tests green (including THREAD_ANALYZER overlay)
- [ ] Bumble tests green (including new rapid-reconnect, auth-inheritance, key-rotation, GATT-write-during-notification)
- [ ] `.config` diffs per phase: no unexplained change in crypto/security Kconfig
- [ ] west2nix critical-module hash-diff: changes documented in commit messages

## Success Metrics
- Migration duration: ≤ 8 working days across 5 phases (Phase 0: 2-3h; Phase 1α: 1-1.5d; Phase 1β: 2-2.5d; Phase 2: 1d; Phase 3: 2-3d; Phase 4: 0.5d)
- Zero critical defects post-merge (critical = any Risk-table rank-1-to-3 realized)
- CI green-rate ≥ 95% (1 flaky bsim timeout tolerance)
- Post-merge developer re-init time < 20 minutes (with warm Cachix)

## Dependencies & Prerequisites
- NCS v3.2.4, 3.1.x, 3.0.2 tags reachable on github.com/nrfconnect/sdk-nrf (confirmed)
- `nrfutil device` ≥ 2.8.8 in dev shell (Phase 0 check)
- Hardware: NOT required for migration merge. Simulation-only validation (Bumble virtual + nrf52_bsim + bsim_dfu + build-time audits). Hardware DFU swap validation tracked in TODO.md for whenever nrf52832dk / nrf54l15dk arrives.
- No external coordination (greenfield)

## Risk Analysis (trimmed)

| Rank | Risk | Likelihood | Impact | Mitigation |
|------|------|-----------|--------|------------|
| 1 | Silent-broken advertising (missing/wrong `.recycled` wiring) | HIGH if naïve impl | Critical | Multi-mode handler + `k_work_cancel_sync` discipline + Phase 1β Bumble rapid-connect gate |
| 2 | `.recycled` race → auth inheritance across reconnect | MEDIUM | Critical | Clear `authorizedGatt` in `.recycled` too; Bumble auth-inheritance test |
| 3 | MCUboot runtime slot-selection bug under auto-assigned IDs | MEDIUM | Critical (unrecoverable) | Phase 3: grep + bsim_dfu + imgtool verify + sysbuild log — catches build-time; runtime accepted pending hardware validation (TODO.md) |
| 4 | nRF52810 RAM overflow post-4.x | MEDIUM | High | Measurement-first audit; preemptive stack pinning; explicit budget |
| 5 | Kconfig default silently enables unwanted subsystem | MEDIUM | Medium | `.config` diff per phase; explain every crypto/security change |
| 6 | west2nix crypto-module hash swap | LOW | High | Per-phase hash-diff on 5 critical pins; commit note per change |

**Not in table (separately tracked):** pre-existing MCUboot dev-key signing — TODO.md.

## Deployment Checklist

### Pre-Merge (per-phase PR and final phase)
- [ ] Branch CI green end-to-end on HEAD (not just latest stage commit)
- [ ] Integration test scenarios for this phase passed
- [ ] Baseline artifacts under `docs/baselines/` committed
- [ ] `.config` diffs explained in commit messages
- [ ] west2nix critical-module hash-diff explained (Phase 1α, 3 only)
- [ ] `ncs-upgrade.yml` workflow grep for hardcoded `2.9`/`v2.9`/`ncs-2` cleaned up (Phase 4)
- [ ] Cachix push confirmed for `bsim-test` derivation before merge

### Post-Merge Verification (within 30 minutes, fresh clone)
```bash
git clone <repo> /tmp/postmerge && cd /tmp/postmerge
cd .. && west init -l postmerge && west update --narrow -o=--depth=1
cd postmerge
nix build .#firmware -L
nix build .#bsim-test
nix run .#test
nix run .#lint
uv run --with bumble --with pytest --with pytest-asyncio pytest tests/ble_client/ -v
```

### Rollback SOP
```bash
git revert -m 1 <merge-sha>
git push origin main
# Developers:
cd .. && rm -rf .west nrf zephyr modules bootloader tools
west init -l Everytag && west update --narrow -o=--depth=1
```
Nix cache is content-addressed; revert is safe, 3.2.4 entries remain valid but unreferenced.

### Monitoring (7 days post-final-merge)
| Signal | Healthy | Investigate if |
|--------|---------|----------------|
| CI wall-clock per PR | ≤ 2× baseline | > 3× sustained |
| Cachix hit ratio | > 70% | < 40% after day 2 |
| Kconfig deprecation warnings | 0 new | any new |
| `ncs-upgrade.yml` next run | advises 3.3+ | still flags 3.2.x |
| nrf52810 bss drift on subsequent PRs | < 22 KB | > 23 KB |

### Communication (post final merge)
> NCS 3.2.4 migration complete — final PR merged. (Sequence was 6 sequential PRs, one per phase, landed over ~1 week.) Action required:
> 1. `git pull origin main`
> 2. `cd .. && rm -rf .west && west init -l Everytag && west update --narrow -o=--depth=1` (lockfile changed)
> 3. Next `nix build .#firmware` slow (~Xm, full rebuild); subsequent cached
> 4. In-flight branches based on `main`: rebase; take `main`'s `west.yml`/`west2nix.toml`; re-run `west update`
> 5. Report advertising or DFU issues in-thread. Rollback SOP in `docs/plans/2026-04-16-refactor-ncs-3.2.4-upgrade-plan.md`.

## Future Considerations
- **Extended advertising API** (`bt_le_ext_adv_*`) — tracked in TODO.md, unlocks simultaneous adv sets, medium-effort, post-migration.
- **NCS 3.3+ readiness** — Zephyr 4.4 ZMS→kvss header move. Minor; plan when 3.3 stable.
- **MCUboot signing key** — tracked in TODO.md; set `CONFIG_BOOT_SIGNATURE_KEY_FILE` when productionizing.
- **Hardware DFU swap validation** — tracked in TODO.md; run on nrf52832dk + nrf54l15dk when hardware arrives to close residual risk on runtime MCUboot slot selection.
- **Per-attribute `BT_GATT_PERM_*`** — if we ever add SMP pairing, revisit. Current single-callback auth is deliberate.

## Execution Model — Agent Team

This plan is executed by a coordinator + per-phase implementor/reviewer agent pairs. Decisions locked 2026-04-16:

### Orchestration
- **Phases are sequential** (Phase 0 → 1α → 1β → 2 → 3 → 4). Each phase gates the next.
- **Sub-tasks within a phase run in parallel** where independent (e.g., Phase 0 baseline captures per board; Phase 1α lockfile/chmod/Kconfig/ZMS; Phase 3 HCI-swap/MCUboot-audit/DTS-checks).
- **Coordinator**: a fresh Claude Code session (not this conversation). Start with `/ce:work docs/plans/2026-04-16-refactor-ncs-3.2.4-upgrade-plan.md`.
- **Per phase**: one implementor agent + one adversarial reviewer agent. Reviewer verifies AFTER implementor reports done.

### Worktrees and branches
- **One git worktree per phase.** Sub-task agents within a phase share the worktree with file-ownership boundaries noted in per-agent prompts.
- Worktree paths under `/tmp/claude-ncs-upgrade/` (not committed).
- Each phase opens its own PR to `main`. No long-lived tracking branch — each phase branches from current `main` AFTER the prior phase has merged.

| Phase | Worktree path | Branch | Branches off |
|-------|--------------|--------|--------------|
| 0 | `/tmp/claude-ncs-upgrade/wt-phase0` | `ncs-upgrade/phase-0-baselines` | `main` |
| 1α | `/tmp/claude-ncs-upgrade/wt-phase1a` | `ncs-upgrade/phase-1a-build-system` | `main` (after phase-0 merges) |
| 1β | `/tmp/claude-ncs-upgrade/wt-phase1b` | `ncs-upgrade/phase-1b-advertising` | `main` (after phase-1a merges) |
| 2 | `/tmp/claude-ncs-upgrade/wt-phase2` | `ncs-upgrade/phase-2-ncs-3.1` | `main` (after phase-1b merges) |
| 3 | `/tmp/claude-ncs-upgrade/wt-phase3` | `ncs-upgrade/phase-3-ncs-3.2.4` | `main` (after phase-2 merges) |
| 4 | `/tmp/claude-ncs-upgrade/wt-phase4` | `ncs-upgrade/phase-4-polish` | `main` (after phase-3 merges) |

Main stays releasable after each phase merge.

### Reviewer veto authority
- **Block phase merge** on: (a) any deviation from this plan's phase spec, (b) any acceptance criterion failing, (c) any drive-by scope not in plan.
- **Advisory only** for: compiler/lint warnings, minor log message cosmetics, style issues that pass `nix run .#lint`.
- Reviewer has AUTHORITY TO REJECT with reasoning; implementor iterates until reviewer signs off.

### Merge authority
- **Implementor merges** their phase PR to `main` after reviewer signs off. No human intervention required between phases.
- `main` advances phase by phase; release cadence preserved.

### Per-commit bisect granularity
- Each of 1a, 1b, 1d, 1e, 1f, 1c-step1, 1c-step2, 1c-step3, 1c-step4, 1g-stack-pinning, 1g-measurement, 1g-levers is a **separate commit**.
- PRs do NOT squash-merge; preserve stage bisectability on `main`.

### Agent timeouts and escalation
- If implementor or reviewer makes no forward progress for **90 minutes** (same error, no new commits, no new diagnostic output), agent reports back to coordinator and halts.
- Coordinator decides: continue (with new instructions), pause (wait for human), or abort (revert phase branch).
- No silent unbounded retries.

### WIP branch handling (one-time, before Phase 0 starts)
From the primary checkout on `main`:
```bash
git tag archive/wip-c46c3d7 upgrade-ncs-3.2.4   # preserve SHA for archaeology
git branch -D upgrade-ncs-3.2.4                 # remove the branch itself
git push origin archive/wip-c46c3d7             # push the tag
git push origin --delete upgrade-ncs-3.2.4      # remove remote branch if present
```

### MCUboot audit scope
- `src/` and `boards/` only. Do NOT audit sdk-nrf modules (out of scope — we don't modify them).

### Test authorship
- **Phase 1β implementor** writes the 4 new Bumble tests (rapid-reconnect, auth-inheritance, key-rotation-during-churn, GATT-write-during-notification).
- **Phase 1β reviewer** validates each via test-injection: revert the fix being tested (e.g., remove `.recycled` callback, remove `authorizedGatt=0` in recycle) and confirm the test FAILS. Then restore fix; confirm test passes. Documents test-injection results in review notes.

### Agent prompt inputs
Each implementor / reviewer agent receives:
- Full path to this plan doc
- The specific phase section (e.g., "Phase 1α" or "Phase 3 MCUboot audit")
- The acceptance criteria slice for that phase (exact checkbox list)
- Worktree path + branch name
- Explicit deliverable statement (commits, PR title, gate items to satisfy)

Agents are fresh-context by default (no conversation history). Plan doc is the source of truth.

### Phase 4 CI workflow grep — dedicated reviewer task
A dedicated Phase 4 reviewer (not the implementor) runs:
```bash
grep -rn "2\.9\.\|v2\.9\|ncs-2" .github/ flake.nix west.yml docs/ README.md CLAUDE.md
```
Expected: zero hits (baseline logs in `docs/baselines/` excluded). Reviewer reports hits; implementor clears them.

## Sources & References

### Internal References
- `CLAUDE.md` — build commands, board list, RAM budgets
- `TODO.md` — deferred extended-adv work + fuzzing entry + new post-Phase-1 fold-ins
- `src/zephyr_hardware.cpp:120-147,170-197` — adv + HCI targets
- `src/gatt_glue.c:82,90,99-102,322-348,375` — disconnect log / conn_cb / service-define / adv params
- `src/beacon_state.cpp:52-83,213-240,260-284,301-330,356-388` — state machine transition points needing `k_work_cancel_sync`
- `src/beacon_config.hpp` → new `src/beacon_nvs_ids.hpp` (Phase 1e)
- `tests/zms_persist/src/main.c` — test to re-wire to shared ID header
- `prj.conf:11,42,44,49,50,59,60` — Kconfig audit targets
- `sysbuild/mcuboot.conf:12-17` — pre-existing dev-key issue
- `flake.nix:95-120` — `westConfigurePhase` (preserve git-init fix, widen chmod scope)
- `.github/workflows/ci.yml:65-92` — actual CI matrix (11 attrs)
- `.github/workflows/release.yml:56-85` — 22-variant release matrix
- `.github/workflows/release.yml:171,186-189` — hardcoded parsing + DFU slot sizes

### External References — Migration Guides
- Nordic NCS 3.0/3.1/3.2 migration guides (docs.nordicsemi.com)
- Nordic sysbuild image IDs breaking change: https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/app_dev/bootloaders_dfu/sysbuild_image_ids.html
- Zephyr 4.0/4.1/4.2 migration guides (docs.zephyrproject.org)

### External References — Community (2025-2026)
- DevZone 125507: `BT_LE_ADV_OPT_CONN` returns -5
- DevZone 127216: MCUboot image ID NCS 3.2.3 `flash_img_init`
- DevZone 127720: nRF54L15 NCS 3.2.3 B0/NSIB validation
- DevZone 126561: NCS 3.0→3.2 device-tree/USB drift
- DevZone 126140: nRF54L15 MCUboot partition sizing (BOOT_MAX_IMG_SECTORS)
- DevZone 105423: `bt_enable` -EAGAIN after MCUmgr reset on 3.2.0
- DevZone 121792: nrf52810 BLE compile-fail under 3.0.0
- DevZone 96536: nrf52810 OTA 24 KB RAM ceiling
- Zephyr issue #94954: spurious `"No valid legacy adv to resume"` log (4.2, cosmetic)
- Zephyr issue #84541: MCUboot + BT_LL_SW_SPLIT regression
- GitHub project-chip/connectedhomeip #40300: `nrfutil-device` missing under 3.0+

### Reference Samples (NCS 3.2.4 checkout at /Users/akirby/Downloads/zephyr)
- `zephyr/samples/bluetooth/peripheral_lbs/src/main.c:53-67` — `.recycled` + workqueue pattern
- `zephyr/samples/bluetooth/hci_pwr_ctrl/src/main.c:82-111` — `bt_hci_cmd_alloc` pattern
- `zephyr/include/zephyr/bluetooth/conn.h:2123-2136` — `.recycled` callback spec
- `zephyr/include/zephyr/bluetooth/buf.h:168-172` — `BT_BUF_EVT_RX_COUNT` BUILD_ASSERT
- `zephyr/include/zephyr/bluetooth/gatt.h:1057-1200` — CCC struct rename (transparent for macro users)
- `nrf/sysbuild/CMakeLists.txt:167-210` — MCUboot image ID assignment

### Research Agents (this session)
- Branch review agent (WIP c46c3d7 analysis)
- NCS 2.9→3.2 migration research
- Beacon samples divergence review
- performance-oracle (RAM/flash analysis)
- security-sentinel (trust anchors, auth races)
- architecture-strategist (staging, branch strategy, IHardware contract)
- code-simplicity-reviewer (YAGNI audit)
- data-integrity-guardian (partitions, ZMS, test drift)
- deployment-verification-agent (checklist, rollback, monitoring)
- julik-races-reviewer (workqueue/recycled races, multi-mode dispatch)
- spec-flow-analyzer (flow gaps, CI workflow audit)
- framework-docs-researcher (Zephyr 4.2 API specifics via Context7/source)
- best-practices-researcher (2025-2026 DevZone community reports)

## Appendix: WIP reference (c46c3d7)

For archaeology after branch deletion. WIP did: `west.yml` bump, lockfile regens, `flake.nix` chmod (too narrow), `BT_LE_ADV_OPT_CONN` rename, `bt_hci_cmd_alloc` swap, same rename in gatt_glue.c, 267-line drive-by reflow in gatt_glue.c (**discarded as noise** in this plan). WIP did NOT: widen chmod, add `.recycled`, bump `BT_BUF_EVT_RX_COUNT`, audit RAM, audit image IDs, update deprecated Kconfig, make ZMS explicit.
