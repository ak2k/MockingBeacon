---
title: SOTA Android companion app — clean-sheet rewrite of tagcheck
type: feature
status: planned
date: 2026-04-25
---

# SOTA Android companion app — clean-sheet rewrite of tagcheck

## Overview

Replace the inherited `tagcheck/` Android Studio project (vasimv, Feb 2026,
~2,100 lines of XML/Activity-era Kotlin in `com.example.tagcheck`) with a
clean-sheet companion app built on current Android architecture. The
existing tagcheck is a working *protocol reference* but not a maintainable
foundation; the cost to refactor it into something we want to keep
exceeds the cost of a fresh build.

**Scope of v1:** functional parity with `conn_beacon.py` + `flash_beacon.sh`
plus mobile-native ergonomics (scan view, file picker for keyfiles, on-device
DFU). Personal / hobbyist / small-fleet (1–30 tags) use cases. **Not** in v1:
multi-user accounts, cloud sync, location reports, Play Store presence.

**Out of scope entirely:** anything that requires a backend service. The
FHN OpenHaystack work (`docs/plans/2026-04-17-fhn-openhaystack-plan.md`)
implies a separate companion app talking to our backend — that's a
different product surface and gets its own plan when it ships.

## Why a new app, not "modernize tagcheck"

| Dimension | tagcheck today | SOTA target | Salvage cost |
|---|---|---|---|
| UI framework | XML layouts + ViewBinding | Jetpack Compose | rewrite all screens |
| Navigation | 4 separate Activities | Single Activity + Navigation Compose | rewrite all routing |
| State | Activity fields, no ViewModel | MVVM with StateFlow | rewrite all state mgmt |
| Async | `Handler(Looper.getMainLooper())` + `postDelayed` | Coroutines + Flow | rewrite all timing |
| BLE | Raw `BluetoothGattCallback` (1,200 lines manual lifecycle) | Nordic `nordic-ble` lib (coroutine wrapper) | reduces BLE surface ~80% |
| Permissions | Manual `requestPermissions` callbacks | Activity Result API + Compose helpers | rewrite all permission flows |
| Persistence | `SharedPreferences` via `Prefs.kt` | DataStore (typed, Flow-based) | rewrite persistence |
| DI | None (everything in Activity) | Hilt | introduce DI |
| Tests | `ExampleUnitTest.kt` (template default, empty) | JVM unit tests for protocol module + Android instrumentation tests | start from zero |
| Package name | `com.example.tagcheck` (template default) | `com.everytag.companion` or similar | rename everything |
| CI | None | GitHub Actions: build + lint + test + signed APK | introduce |
| Distribution | "Use the included APK" but no APK in any release | Signed APK in GitHub Releases, F-Droid candidate | introduce |

What remains constant between tagcheck and a rewrite:

- The 15 GATT characteristic UUIDs and their wire format
- The 8-byte ASCII password authentication flow
- The keyfile chunking pattern (omit first byte, 14-byte chunks, terminate
  with two 14-byte zero blocks)
- The `mcumgr-ble:2.7.4` library choice (current Nordic lib; right call)
- The 6-second scan timeout with rescan-before-reconnect retry pattern
  (genuinely useful — Android BLE stack flakiness is real, GATT-133 is real)
- The DFU watchdog idea (25 s no-callback timeout → restart whole flow)

Net: tagcheck is *protocol documentation in Kotlin*. The rewrite extracts
that into a reusable `:protocol` module and rebuilds the rest from current
patterns.

## Repo & distribution

**New repo: `ak2k/everytag-android`** (separate from firmware repo).

Reasoning:
- Independent CI cadence (firmware and app evolve at different rates)
- Independent release versioning (firmware semver vs app semver)
- Cleaner contributor surface (Android developers don't need to clone
  the Zephyr workspace)
- `Everytag` firmware repo's `.github/workflows` and `flake.nix` stay
  focused on firmware concerns
- This plan stays in the firmware repo (`docs/plans/`) as the canonical
  design record; implementation lives in the app repo

**Distribution:**
- **Primary:** GitHub Releases — signed APK + signed bundle. Same release
  cadence as firmware? No — app releases are independent.
- **Secondary (eventual):** F-Droid. Open-source friendly, no account fee,
  but requires reproducible builds and a manifest in their meta repo.
  Defer until v1 stabilizes.
- **Not pursuing:** Play Store. $25 one-time + Play Console review
  overhead doesn't fit hobbyist scope. Reconsider if user demand emerges.

## Architecture

### Tech stack with rationale

| Layer | Choice | Why |
|---|---|---|
| Language | Kotlin 2.x | Standard Android |
| UI | Jetpack Compose + Material 3 | Eliminates XML, enables previews, smaller code |
| Navigation | Navigation Compose | Type-safe routes, single-Activity model |
| Architecture | MVVM via ViewModel + StateFlow | Standard Android post-2022 |
| DI | Hilt | Compose-aware, less boilerplate than Dagger |
| BLE scanning + GATT | [Nordic Android BLE Library v2](https://github.com/NordicSemiconductor/Android-BLE-Library) (`no.nordicsemi.android:ble-ktx`) | Coroutine-based wrapper, by Nordic, handles GATT-133 retries, MTU negotiation, bonding correctly |
| DFU | `no.nordicsemi.android:mcumgr-ble:2.7.4+` (already in tagcheck — keep) | The right library, no alternatives |
| Persistence | DataStore Preferences (or Proto DataStore for typed config) | Flow-based, supersedes SharedPreferences |
| Async | Kotlin Coroutines + Flow throughout | Standard |
| Testing | JUnit 5 + MockK + Turbine + Compose UI testing | Standard |
| Build | Gradle 8.x with Version Catalog (libs.versions.toml) | Already in tagcheck — keep |
| Min SDK | 26 (Android 8.0) | Same as tagcheck; covers BLE 5 features |
| Target SDK | 36 (Android 16) | Current; align with Play requirements |

### Module layout

```
everytag-android/
├── settings.gradle.kts
├── build.gradle.kts                # root, version catalog
├── gradle/libs.versions.toml
├── app/                            # entry point, Hilt setup, Compose nav graph
├── feature/
│   ├── scan/                       # discovery screen
│   ├── connect/                    # connect + auth flow
│   ├── settings/                   # 15-UUID settings UI
│   ├── keyfile/                    # AirTag keyfile import + management
│   └── dfu/                        # OTA flow
├── core/
│   ├── protocol/                   # PURE KOTLIN. UUIDs, encoding, validation.
│   │                               # No Android deps, JVM-testable.
│   ├── ble/                        # Nordic BLE wrapper, suspend functions
│   ├── dfu-mcumgr/                 # mcumgr-ble wrapper + watchdog
│   ├── persistence/                # DataStore repos
│   └── ui/                         # shared Compose components
└── .github/workflows/
    ├── ci.yml                      # build + lint + unit tests + instrumentation
    └── release.yml                 # signed APK on tag push
```

The `:core:protocol` module is the most important boundary. It contains:
- All 15 GATT UUIDs as named constants
- `encodeStatusByte(): ByteArray`, `decodeStatusByte(): StatusConfig` etc.
- `chunkKeyfile(bytes: ByteArray): List<ByteArray>` (with the omit-first-byte
  + 14-byte chunks + 2× zero blocks logic)
- `validatePassword(s: String): Result<ByteArray>` (8-ASCII check)
- `parseTxPower(level: Int): TxPower` (sealed class)
- Wire format constants (interval multipliers, status byte field layout)

This module:
- Has zero Android dependencies (pure Kotlin / JVM)
- Runs in JVM unit tests with no emulator
- Could in theory be reused by other clients (a Compose-multiplatform
  desktop config app, a server-side validator, etc.)
- Mirrors what `src/beacon_config.cpp` does on the firmware side, in Kotlin

### v1 feature scope

| Feature | Mirror in CLI | Mobile-native value-add |
|---|---|---|
| Scan view: discover Everytags by service UUID, sort by RSSI, name | `simplepyble` scan loop | RSSI gauge, friendly names, persistent device naming |
| Connect view: enter 8-char auth, see connection status | `conn_beacon.py -a` | live RSSI during connection, retry visibility |
| Settings view: TX power, broadcast intervals, status byte config, key rotation interval, accel threshold, MAC reconfig | `conn_beacon.py -p/-d/-s/-l/-m/-c/-n` | typed sliders/dropdowns instead of CLI flags, validation feedback |
| Keyfile import: pick `.bin` from Files, validate, upload | `conn_beacon.py -k` | no laptop required, file picker integrates with cloud storage |
| FMDN key configuration | `conn_beacon.py -f` | hex input with validation |
| Clock sync | `conn_beacon.py -w` | one-tap sync to phone clock |
| Read clock | `conn_beacon.py -r` | display drift since last sync |
| OTA DFU: pick `.signed.hex`, flash, monitor progress | `flash_beacon.sh` | progress bar with bytes/sec, watchdog cancellation |

**Explicitly deferred to v2 or later:**
- Multi-tag fleet view (group, batch operations)
- Per-tag profiles (saved settings templates)
- Settings backup/restore (export/import JSON)
- Tag naming / nicknames persisted across reboots
- Low-battery alerts via local notifications when scan sees a low-voltage
  status byte
- Map view of last-seen locations (would need backend or Apple Find My
  data scrape — significant scope)
- Telemetry dashboard (battery / accelerometer history graphs)
- Locale / i18n beyond English

## Carrying forward from tagcheck

These are the "non-obvious things tagcheck got right" that should be
captured in the rewrite even though no code carries over:

1. **6 s scan timeout, rescan-before-reconnect.** Android's `BluetoothDevice`
   cache can hold stale references that produce GATT-133 on reconnect. Always
   start a fresh scan to obtain a current `BluetoothDevice` instance, then
   connect. Code: `tagcheck/.../ConnectActivity.kt:120-180`. Implementation
   in rewrite: encapsulate inside `:core:ble` so feature modules don't
   reimplement.

2. **400 ms delay between `stopScan()` and `connectGatt()`.** Empirically
   reduces stack instability. Same source. Same encapsulation.

3. **DFU no-callback watchdog (25 s).** mcumgr-ble can hang silently if
   SMP responses stop arriving (peer reboot, MTU stall, Bluetooth
   connection drop). Watch for `dfuAnyCallbackSeen` flag updated on every
   callback; if no callbacks in 25 s, restart the whole upgrade flow.
   Code: `tagcheck/.../FirmwareActivity.kt:62-75`. Implementation in
   rewrite: feature of `:core:dfu-mcumgr`.

4. **Write queue serialization.** Android BLE doesn't allow multiple
   in-flight characteristic writes; tagcheck queues them with a `Queue<Pair<UUID, ByteArray>>`
   and processes one-at-a-time on `onCharacteristicWrite` callback.
   Nordic's BLE library handles this for you (its suspend functions
   serialize internally), so this concern goes away — but the queue
   *order* (password first, then config writes, then keyfile chunks,
   then keyfile terminator) is the contract the firmware expects. Encode
   that order as a `SettingsApplyJob` data class in `:core:protocol`.

5. **Keyfile chunking gotcha.** Skip the first byte of the file, send
   in 14-byte chunks, terminate with two 14-byte zero blocks. Code:
   `tagcheck/.../ConnectActivity.kt:283-310`. Also documented in
   `conn_beacon.py:140-160` — but tagcheck's implementation is the
   most readable. Mirror in `:core:protocol::chunkKeyfile()`.

6. **Permanent location-permission warning** banner. Android requires
   `ACCESS_FINE_LOCATION` for BLE scanning even when location services
   are off. tagcheck shows a permanent reminder banner (`MainActivity.kt:55`).
   Worth keeping — users get confused otherwise.

These notes belong in the new repo's `docs/protocol-notes.md` so they
survive the rewrite.

## Testing strategy

**`:core:protocol` (highest test ROI):**
- JVM unit tests via JUnit 5
- 100 % branch coverage target — pure-function module, no excuse
- Property-based tests for round-trip encoding (encode → decode == input)
- Run on every PR, fast (< 5 s)

**`:core:ble`:**
- Mock BLE infrastructure via Robolectric where possible
- Real-device instrumentation tests gated to specific PRs (BLE hardware
  required)

**`:core:dfu-mcumgr`:**
- Watchdog logic JVM-testable in isolation (inject a fake mcumgr controller)
- Real DFU flow tested manually + via the firmware's `tests/bsim_dfu/`
  sim test (which doesn't need Android, but proves the wire protocol)

**Feature modules:**
- ViewModel unit tests with `kotlinx-coroutines-test` + Turbine for Flow
  assertions
- Compose UI tests for critical flows (scan → connect → write settings;
  pick keyfile → flash; pick firmware → DFU progress → confirm)

**E2E:**
- Manual test plan documented per release in `docs/test-plans/v0.x.md`
  pending instrumentation infrastructure
- Eventual goal: real-device CI via Firebase Test Lab or similar

## CI / release

**On every PR:**
- `./gradlew lint testDebugUnitTest assembleDebug`
- `:core:protocol` coverage report (Jacoco) — fail under 90 %
- ktlint + detekt
- ~2-3 min total

**On tag push (`v*`):**
- Build signed release APK + AAB
- Upload to GitHub Release
- Generate release notes from `CHANGELOG.md`

**Signing:**
- Per-environment keys (`release-debug.jks` checked into `.github/secrets/`,
  `release-prod.jks` in repository secrets only)
- Symmetric to firmware MCUboot signing key handling — both should
  document the per-build signing approach in their READMEs

## Implementation phases

| Phase | Scope | Estimated effort |
|---|---|---|
| **Phase 0** | Repo bootstrap: gradle, version catalog, CI skeleton, single empty Activity, "Hello Compose" | 0.5 day |
| **Phase 1** | `:core:protocol` module: all 15 UUIDs, encoders/decoders, validators, JVM tests with high coverage | 1.5 days |
| **Phase 2** | `:core:ble` module: Nordic BLE wrapper, scan + connect + write suspending APIs, GATT-133 retries | 2 days |
| **Phase 3** | Scan + Connect + Settings screens (Compose). Talks to a single tag end-to-end. **First demo-able milestone.** | 2 days |
| **Phase 4** | Keyfile import + apply (file picker, chunked write, validation) | 1 day |
| **Phase 5** | DFU module + screen (mcumgr-ble wrapper, watchdog, progress UI) | 2 days |
| **Phase 6** | Polish: persistence (DataStore), per-tag naming, error UX, accessibility | 2 days |
| **Phase 7** | Signed release APK + GitHub Actions release workflow, README, screenshots | 1 day |

**Estimated total: ~12 working days for v1.** Prioritization: Phases 0-3
get us to "configures a tag end-to-end" within a week. Phases 4-5 are
the high-ROI mobile-native features. Phases 6-7 are polish/release.

## Accepted tradeoffs

- **Single-platform.** Android only. iOS would need a parallel implementation
  in Swift / SwiftUI; not justified by hobbyist user base. Anyone who needs
  iOS today has `conn_beacon.py` (works on macOS).
- **No Play Store presence at v1.** GitHub Releases + sideload only. Adds
  friction for non-technical users but matches scope.
- **Personal-use-only license expectations.** Same posture as firmware:
  AirTag emulation is for enthusiast / personal tracking, not commercial.
  README should mirror the firmware's tone here.
- **No code sharing with the firmware.** Even though the protocol contract
  is the same, the wire-format encoders are reimplemented in Kotlin (vs.
  the C++ in `src/beacon_config.cpp`). The firmware's host-native test
  suite already exercises the firmware side; the app's `:core:protocol`
  tests will exercise the app side; together they cover the protocol.
  KMM (Kotlin Multiplatform Mobile) for shared protocol code is rejected
  for v1 — the encoders are too small (~100 lines total) to justify
  the build complexity and licensing implications of sharing C++/Kotlin.

## Why we're keeping the vasimv credit

The original Android app + the firmware both originated from vasimv.
This plan replaces the app code, but the *idea* of an Android companion
app — and the protocol it speaks to — is part of the lineage. The README
credits section keeps the line attributing the original Android app to
vasimv even after the code is removed; this plan is the bridge between
"we removed the code" and "we eventually have a new app." When the new
app ships, its README will attribute the protocol design (UUIDs,
encoding, auth flow) to the firmware lineage that includes vasimv.
