---
title: Real Find Hub Network (FHN) participation, OpenHaystack-style
type: feature
status: planned
date: 2026-04-17
---

# Real Find Hub Network (FHN) participation, OpenHaystack-style

## Overview

Replace today's stub "FMDN" advertisement with a real, Google-resolvable
Find Hub Network broadcaster. OpenHaystack-style: no Fast Pair
certification, no Google Model ID registration, backend uploads
per-tag EID precomputes to Google on the user's behalf every ≤3 days.
Unlike Apple Find My (cold-broadcast, stateless server), FHN requires
continuous server-side EID registration — which means a backend + a
companion app, not firmware alone.

**Scope**: enthusiast / personal use only. Commercial use requires
Fast Pair certification (separate project). See ToS note in
"Accepted tradeoffs."

## Problem statement

**Today** (`src/ble_glue.c:15`): a static 20-byte blob inside an
Eddystone-shaped service-data frame (`0xFEAA` + frame type `0x41`).
Never rotated, no crypto. Android Find Hub sees a valid-shape frame
but cannot resolve the "EID" to any owner account, so no reports
round-trip.

**Real FHN** ([spec](https://developers.google.com/nearby/fast-pair/specifications/extensions/fmdn)):

- EID rotates every 1024s + 1–204s jitter. Derived as
  `r = AES-ECB-256(EIK, 32-byte-framed-timestamp) mod n; R = r·G; EID = R.x`
  on **SECP160r1** (default, 20 B / BLE 4 legacy adv) or SECP256R1
  (32 B / BLE 5 extended adv). **EIK is the AES key directly — no HKDF
  in the EID path.**
- Per-rotation MAC: **independently random NRPA**, not derived from EID.
  Must rotate on same 1024s edge for anti-stalking evasion.
- Server-side resolution is a **rainbow-table lookup of 10-byte
  truncated EIDs** with a **~4-day TTL**. Google never sees the EIK;
  the owner (or our backend) pre-computes upcoming EIDs and uploads
  them to Google so relayed reports can be indexed back to the account.
- Reports delivered via **FCM push** to a persistent MCS listener
  (`mtalk.google.com:5228`), not polling. Location is E2EE to the
  owner's EIK; Google sees timestamp + accuracy metadata only.
- Owner authenticates to Google via **gpsoauth** impersonation of
  a GMS app (undocumented; see fragility section).

## Architecture

### Two designs, default per board

| Design | Default on | Tag stores | Tag crypto | Phone-to-tag top-up |
|---|---|---|---|---|
| **A — pregen batch** | **nRF52805 / 52810 / 52832 / 52833** | 4-day EID table (~7 KB) | None | Required every ≤3 days |
| **C — on-device derive** | **nRF54L15** | 32-byte EIK + beacon clock | AES-ECB-256 via Cracen; SECP160r1 via software uECC (~60 ms) or BLE 5 + SECP256R1 via Cracen (~1.5 ms) | **Never** — tag autonomous given stable clock |

Per board, pick the design the hardware is best suited to:

- **nRF52xxx** has no hardware crypto accelerator worth the bother;
  the pregen-table path has zero on-device crypto cost and is
  drop-dead simple. Design A is the natural default.
- **nRF54L15** has Cracen + KMU. Spending them on-device to
  eliminate periodic BLE batch top-ups (and flash wear from batch
  rewrites) is the obvious trade. Design C is the natural default.

**Both designs still require our backend to upload EID precomputes
to Google every ≤3 days** — that's a server-side constraint
(Google's rainbow-table TTL), not a tag constraint. The difference
is purely in who refreshes the *tag's* EID source: phone-proximity
top-up in A, autonomous on-chip derivation in C.

Behind Kconfig each design is still available on the other family
for experimentation (e.g., Design A on 54L15 for A/B testing, or
Design C on 52832 if ever needed) — it's the `defconfig` default
that flips per SoC, not a hard lock.

### Batch size: 4 days

Per-tag batch = 4 days × 84 EIDs/day × 20 B = **6,720 bytes**. Matches
Google's server-side TTL (going larger is wasted — server drops EIDs
older than 4 d). Fits every board, including nRF52810's ~12 KB ZMS
partition.

### Rotation

- **FHN**: 1024s ± (1..204 s random jitter). Mandatory per spec for
  anti-stalking evasion, not optional polish.
- **AirTag**: unchanged — keeps existing `change_interval` (default 600s).
- **MAC**: independently random NRPA regenerated on every FHN
  rotation edge. Not derived from EID — Google doesn't check the
  correlation, and `bt_addr_le_create_nrpa()` is the idiomatic call.
- **Decoupled**: AirTag and FHN run on independent timers.
- **Static-MAC escape hatch**: `CONFIG_EVERYTAG_FHN_STATIC_MAC=y` if
  Android "Unknown Tracker Alert" fires on nearby users. Trades owner
  privacy for detection avoidance.

### Security posture

**Firmware-side authentication on sensitive writes** (`fhnEIK`,
`fhnUnprovision`, batch-chunk). Options:

1. **LESC bonding + `BT_GATT_PERM_WRITE_AUTHEN`** (textbook; requires
   pairing UI on app side)
2. **Pre-shared token verified in firmware before accepting write**
   (QR code printed on PCB at flash time, verified via existing
   session-auth mechanism extended)

Either works for this use case; pick during Phase 1. **Today's
session-auth-without-bonding is insufficient** — a sniffer in range
during provisioning captures the EIK and tracks the owner indefinitely.

### DULT (anti-stalking) — deferred

Full DULT compliance (`0xFEF3` service + physical alert mechanism) is
hardware-gated on some SKUs (no speaker/LED). Per product scope
(enthusiast, personal use), ship without DULT. Monitor field reports.
If Android alerts fire: either flip `FHN_STATIC_MAC` (cheap, privacy
regression) or implement DULT (larger effort). Not a Phase 1-3 concern.

## Firmware

### New files

- `src/fhn_eid.{cpp,hpp}` — pure C++, host-testable:
  - Design A: `eid_for_slot(const EidBatch&, uint32_t slot) -> Eid`
  - Design C: `derive_eid(ICryptoProvider&, const Eik&, uint32_t slot) -> Eid`
- `src/fhn_config.{cpp,hpp}` — EIK + beacon clock + batch cursor
  wrappers on `SettingsManager` / ZMS
- `src/icrypto.hpp` — new peer interface to `IHardware` (do **not**
  leak PSA primitives into `IHardware`):
  ```cpp
  class ICryptoProvider {
   public:
    virtual int derive_eid(const uint8_t eik[32], uint32_t ts_masked,
                           uint8_t eid_out[20]) = 0;
    // Design A impl: returns eid_batch[ts_masked / 1024]
    // Design C impl: runs the AES+ECC derivation via Cracen/uECC
  };
  ```
- `src/cracen_crypto.cpp` (54L15 only, Design C) — `ICryptoProvider`
  impl via PSA Crypto
- `src/table_crypto.cpp` (Design A) — table-lookup impl

### Changes

- `src/beacon_state.{cpp,hpp}` — `handle_fhn_rotation()` on independent
  timer; takes `ICryptoProvider&` alongside existing `IHardware&`
- `src/gatt_glue.c` — new characteristics:
  - `fhnEIK` — write EIK (Design C) or batch chunks (Design A)
  - `fhnUnprovision` — explicit clear; idempotent
  - `fhnStatus` — read EIK-present flag, beacon-clock, days-remaining
  All three are `BT_GATT_PERM_WRITE_AUTHEN`.
- `src/ble_glue.c` — new `adv_fhn[]` template (`0xFEAA` + `0x41` +
  20 B EID + 1 B hashed flags); replaces stub `adv_fmdn[]`
- `src/beacon_logic.{cpp,hpp}` — add `derive_mac_random_nrpa()` helper;
  keep existing `derive_mac_from_key()` for AirTag side
- `src/zephyr_hardware.cpp` — adv-set wiring for FHN; PSA-init on 54L15
- **Rename** `adv_start_fmdn` → `adv_start_fhn`, `adv_update_fmdn` →
  `adv_update_fhn`, `prepare_fmdn` → `prepare_fhn_adv` in IHardware
  interface (Phase 2, don't defer — stale names rot)

### Removed

- `prepare_fmdn()`, `set_fmdn_key()`, `fmdn_key` field, `ID_fmdnKey_NVS`,
  `ID_fmdn_NVS`, `flag_fmdn`, `adv_fmdn[]`, `fmdn_data_store`
- **No per-device anti-spoofing keypair.** Google doesn't check it for
  non-Fast-Pair tags. Cargo cult from `locator_tag`.

### NVS schema versioning

Reserve **`ID_schemaVersion_NVS = 0xFF`** now. Write `1` from the first
FHN firmware. Read-or-default-to-0 at boot. Migration path for
existing field devices: since we're greenfield (per
`src/beacon_nvs_ids.hpp:11`), treat "schema v0 detected → factory
reset" as the v1 migration. Add a `static_assert(sizeof(EidBatch)
== EXPECTED_SIZE)` to catch future curve swaps at compile time.

### Minimal Kconfig (Design C on nRF54L15)

```kconfig
CONFIG_EVERYTAG_FHN=y
CONFIG_EVERYTAG_FHN_DERIVE=y        # 54L15 only
# CONFIG_EVERYTAG_FHN_PREGEN=y      # universal default
# CONFIG_EVERYTAG_FHN_STATIC_MAC=y  # escape hatch

CONFIG_NRF_SECURITY=y
CONFIG_MBEDTLS_PSA_CRYPTO_C=y
CONFIG_PSA_CRYPTO_DRIVER_CRACEN=y
CONFIG_PSA_WANT_KEY_TYPE_AES=y
CONFIG_PSA_WANT_AES_KEY_SIZE_256=y
CONFIG_PSA_WANT_ALG_ECB_NO_PADDING=y
CONFIG_PSA_WANT_ECC_SECP_R1_256=y   # only if BLE 5 + P-256 path
CONFIG_PSA_WANT_GENERATE_RANDOM=y
CONFIG_TRUSTED_STORAGE=y
```

EIK storage option A: KMU with `CRACEN_KMU_KEY_USAGE_SCHEME_PROTECTED`
(hardware-isolated). Option B: Zephyr Settings (same as Nordic's FMDN).
Default to Option B for simplicity; upgrade to Option A if threat
model demands.

## Backend

### Two systemd units sharing one SQLite file

- **`everytag-api.service`** — FastAPI, Uvicorn. REST endpoints for
  companion app (pair, list-tags, list-locations, unprovision). Also
  runs the per-tag EID-upload cron via `apscheduler` (or a systemd
  timer calling `uv run -m everytag.refresh --all`; pick one).
- **`everytag-mcs.service`** — dedicated FCM/MCS listener persistently
  connected to `mtalk.google.com:5228`. Receives encrypted location
  reports, decrypts with per-tag EIK, inserts into `locations` table.
  Separate process so API deploys don't drop report delivery.

Both units run on the same host (Hetzner CX22, €3.85/mo). SQLite in
WAL mode with `busy_timeout=5000`. Graduate to Postgres at ~5k tags.

### Config

**SOPS-encrypted YAML** (`/etc/everytag/config.yaml.sops`) decrypted
at boot with an age key at `/etc/everytag/age.key` (mode 400). Holds:

- Google OAuth refresh tokens (single-tenant: one per user, in a map)
- Per-tag EIKs (single source of truth)
- Pinned `fastPairModelId` and GMS `client_sig` (updateable when
  upstream rotates)

Age key backed up to 1Password on initial deploy. No libsodium
envelope-per-row encryption (single-tenant, file-mode 0600, age
protects at rest).

### SQLite schema

```sql
CREATE TABLE users (
  google_sub TEXT PRIMARY KEY,
  email TEXT,
  created_at INTEGER
);
CREATE TABLE tags (
  id INTEGER PRIMARY KEY,
  user_sub TEXT REFERENCES users,
  eik_hash BLOB UNIQUE NOT NULL,          -- blake2b(eik), not the eik
  google_canonic_id TEXT UNIQUE,          -- set after CreateBleDevice
  pair_date INTEGER,
  status TEXT CHECK(status IN ('pending','registered','unprovisioned')),
  created_at INTEGER,
  last_sync_at INTEGER
);
CREATE TABLE locations (
  id INTEGER PRIMARY KEY,
  tag_id INTEGER REFERENCES tags,
  reported_at INTEGER,
  lat REAL, lon REAL, accuracy REAL,
  decrypted_at INTEGER
);
```

EIK cleartext lives only in the SOPS config; `tags.eik_hash` is for
dedup + lookup. If SQLite leaks, EIKs don't.

### Registration dance (orphan-free)

```python
def register_tag(user_sub, eik):
    # 1. Save EIK to SOPS config + tags row with status=pending
    # 2. Call Spot CreateBleDevice
    # 3. On success: update row status=registered, store canonic_id
    # Crash between 2 and 3: next cron sees status=pending,
    #   calls CreateBleDevice idempotently (retry-safe per Google)
    # Crash between 1 and 2: next cron sees status=pending, retries
```

`UNIQUE(eik_hash)` and `UNIQUE(google_canonic_id)` catch double-registration.
No WAL-table ceremony needed — ordering the writes correctly handles it.

### Google API integration

Two clients, two scopes:

- **Nova** (`android.googleapis.com/nova/*`, HTTP/1.1, ADM bearer via
  `gpsoauth.perform_oauth` with app `com.google.android.apps.adm`):
  `nbe_list_devices`, `nbe_execute_action` (locate)
- **Spot** (`spot-pa.googleapis.com`, gRPC, Spot bearer via same
  gpsoauth with app `com.google.android.gms`):
  `CreateBleDevice`, `UploadPrecomputedPublicKeyIds`

Pin `fastPairModelId = "003200"` and `client_sig =
"38918a453d07199354f8b19af05ec6562ced5788"` in config (both are
upstream invariants today, both rotate-risks).

EID precompute upload: **every tag, every 2 days, at a staggered
minute based on `tag_id % 60`** to avoid thundering-herd. Overlap 24h
backward so a missed run doesn't create a 4-day gap. Use `coincurve`
(native-backed secp160r1) not pure-Python `ecdsa` — collapses EID
generation from ~1 s/tag to ~10 ms/tag.

### Endpoint drift resilience

- **Pydantic response validation** with `extra='allow'`; structured
  log WARN on unknown fields. No raw-body storage (single tenant;
  reproducible from logs).
- **Daily canary cron** (GH Actions): minimal `nbe_list_devices` on a
  dedicated test account; asserts shape against
  `schema-snapshot.json` checked into repo. Fail → email/push alert.
- **Pinned GoogleFindMyTools** (commit-hash ref, not submodule) in
  `backend/references/` — diff-on-update visibility for upstream
  protocol drift.

### Failure modes planned

1. **Refresh-token death** (6mo idle, password change, user revoke,
   100-token cap, 7-day expiry in "Testing" OAuth status):
   publish the OAuth consent screen (unverified is fine); surface
   `invalid_grant` as a push to the app ("reconnect Google")
2. **Wire format change**: canary fires within 24h; pin old client,
   ship new client
3. **KEK loss**: age key lives in 1Password + initial deploy notes.
   Recovery = re-consent everyone. EIK loss = tag factory-reset and
   re-provision (rare).

## Android companion app

### Shape

- Single Activity, Compose, 2 screens: `Home` (list tags + last-seen
  + location), `PairNew` (CDM picker → BLE write)
- **No Room** — backend is source of truth; re-fetch on resume
- **No Hilt** — manual DI is ~5 lines
- **Nordic `no.nordicsemi.android:ble:2.9.0` + `ble-ktx`** for GATT
- Retrofit + OkHttp + `CertificatePinner` for backend HTTPS
  (we control cert rotation)

### CompanionDeviceManager (key decision)

- `cdm.associate()` with `BluetoothLeDeviceFilter` → OS-native picker
- `cdm.startObservingDevicePresence(mac)` → system wakes
  `CompanionDeviceService.onDeviceAppeared()` when tag in range,
  **even if app process is dead**
- In callback: promote to `FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE`,
  run `TopUpWorker`, tear down

Eliminates the entire `ACCESS_FINE_LOCATION` / background-location
permission mess. Minimum sdk 31 (Android 12) required.

### Permissions

```xml
<uses-permission android:name="android.permission.BLUETOOTH_SCAN"
    android:usesPermissionFlags="neverForLocation"/>
<uses-permission android:name="android.permission.BLUETOOTH_CONNECT"/>
<uses-permission android:name="android.permission.FOREGROUND_SERVICE"/>
<uses-permission android:name="android.permission.FOREGROUND_SERVICE_CONNECTED_DEVICE"/>
<uses-permission android:name="android.permission.REQUEST_COMPANION_RUN_IN_BACKGROUND"/>
```

### Top-up flow

1. CDM fires `onDeviceAppeared`
2. Service calls backend `/tags/{id}/pending-writes` → gets next batch
3. Request MTU 247 on connect, request CONNECTION_PRIORITY_HIGH
4. Write batch sequentially (queue-of-one per `BluetoothGatt`)
5. POST `/tags/{id}/sync-confirmed` to backend
6. Tear down

Expected wall-clock for 4-day batch (~7 KB, ~30 writes at ~40ms each):
~1.2 s. Doesn't rely on the existing 2s settings window — opens a
dedicated long-lived connection.

### What's cut

- Three-tier notifications (T-7 / T-2 / T-0): removed. Home screen
  shows "days until top-up stale" — dashboard > notifications for
  this audience.
- Time-since-seen auto-expire unprovisioning: removed. Explicit-only
  via "Remove tag" button. User can re-flash if needed.
- iOS version: deferred indefinitely.

## Testing

No hardware available; sim is everything. Keep all layers.

| Layer | Path | Purpose |
|---|---|---|
| Host KAT + differential | `tests/host/` (extend) | Crypto correctness; diff vs leonboe Python reference |
| BabbleSim frame + rotation | `tests/bsim/` (extend) | On-air validation; scanner asserts `0xFEAA/0x41`, 1024s rotation + jitter, MAC rotates on same edge |
| Bumble GATT | `tests/ble_client/` (extend) | New-char auth enforcement, batch-chunk reassembly, unprovision idempotency |
| ZMS migration | `tests/zms_persist/` (extend) | Schema v0 → v1 (factory-reset path), static_assert sizes |
| Mock FHN server | `tests/fhn_mock_server/` (new) | Backend wire correctness; pytest-httpserver (real TCP) |
| Live network | `tests/fhn_live/` (new, manual) | End-to-end pre-release; dedicated test Google account |
| Endpoint canary | `.github/workflows/fhn-canary.yml` | Daily shape assertion against `schema-snapshot.json` |

**EID KAT strategy**: differential test our C++ impl against
`FMDNCrypto/eid_generator.py` via a Python script that emits
`tests/host/vectors/fhn_eid.json`. No official vectors exist; diff is
our floor.

**BabbleSim time trick**: sim time is decoupled from wall clock.
`-sim_length=3100e6` (~3100s sim) runs in ~15 s wall time, exercises
3 rotations.

**Deferred**: FHN fuzzing targets (existing AirTag-key fuzz
structurally similar; add post-ship if bug warrants), property-based
testing (KAT + differential is enough).

## Implementation phases

### Phase 1 — Crypto + firmware + backend core (~2 weeks)

Firmware and backend build in parallel on the same EID math.
Differential testing between them is the critical de-risk.

Firmware:
- [ ] `src/fhn_eid.{cpp,hpp}` + host tests with KAT JSON
- [ ] `src/icrypto.hpp`, `src/table_crypto.cpp`, `src/cracen_crypto.cpp`
- [ ] New adv frame + NRPA MAC rotation
- [ ] StateMachine integration (independent FHN timer)
- [ ] NVS schema versioning from day 1
- [ ] Rename `adv_*_fmdn` → `adv_*_fhn`

Backend:
- [ ] `backend/fhn_crypto.py` — EID pregen (coincurve); same vectors
- [ ] SQLite schema + `UNIQUE(eik_hash)` + orphan-free registration
- [ ] SOPS config bootstrap + age key setup

Tests:
- [ ] Host differential test (C++ vs Python on shared vectors)
- [ ] BabbleSim scanner asserts frame shape + 3-rotation cycle
- [ ] Bumble tests for `fhnEIK` auth enforcement + batch-chunk writes

### Phase 2 — Integration (~1 week)

Firmware ↔ backend ↔ Google ↔ Android app all connected.

Backend:
- [ ] Nova + Spot clients (gpsoauth chain)
- [ ] `everytag-mcs` systemd unit + MCS listener + report decrypt
- [ ] `everytag-api` systemd unit + REST endpoints
- [ ] Mock-server test fixtures for the Google client paths
- [ ] `.github/workflows/fhn-canary.yml` + `schema-snapshot.json`

Android:
- [ ] CDM pair flow + Home/PairNew screens
- [ ] TopUpWorker + OkHttp backend client + cert pinning
- [ ] LESC bonding during pair (or pre-shared QR — pick during Phase 1)

### Phase 3 — Ship and iterate

- [ ] Flash own devices; validate end-to-end via manual live test
- [ ] Observe: DULT alert reports, power budget impact of 3-frame
      alternation (~+50% adv energy), endpoint drift signals
- [ ] If DULT alerts fire on strangers: flip `FHN_STATIC_MAC` default
- [ ] README + user-facing docs updated once the capability is real
- [ ] Close `TODO.md` item "fold FMDN README wording once real FHN ships"

## Plan B

If Google breaks the RE'd endpoint (likely within 12–24 months given
Feb 2025 TTL tightening + anti-stalking enforcement trajectory):

**Real fallback: AirTag-only degraded mode.** Already fully functional
today. Flip `flag_fhn=false` via settings window; users lose
Google-side findability but retain Apple-side. Low-cost, zero-risk.

**Not a credible fallback**: "vendor in Nordic's `locator_tag` Fast
Pair stack." Reviewers correctly flagged that our design diverges too
far (different EID path, different GATT surface, different provisioning
model) — switching is a rewrite of ~all Phase 1-2 firmware, not a
plugin swap. If Fast Pair certification ever becomes business-viable,
treat it as a new project.

## Accepted tradeoffs

- Backend cron every ≤3 days uploading EID precomputes to Google
  (non-negotiable; inherent to FHN architecture)
- Undocumented/RE'd Google endpoint — may break; mitigated by canary
- nRF52810 has no OTA path — reflash-only recovery if protocol changes
- DULT skipped for now — enthusiast scope; revisit if alerts fire
- No Fast Pair certification, no Google-registered Model ID
- gpsoauth impersonation of GMS app is **ToS-gray**; acceptable for
  enthusiast/personal use per the public precedent of
  GoogleFindMyTools (1k stars, no enforcement observed); **do not
  ship commercially** without Fast Pair cert
- No per-device anti-spoofing keypair (Google doesn't validate it
  without Fast Pair cert anyway)

## Open questions

- [ ] Exact frame type byte for "UT mode OFF" vs "UT mode ON"
      (`0x40` vs `0x41`) — pick before adv template is final
- [ ] LESC bonding vs pre-shared QR token — decide during Phase 1
      crypto week, affects Android pairing UX
- [ ] Confirm `coincurve` secp160r1 support (native library — some
      builds exclude P-160) before locking backend crypto
- [ ] Per-Android-OEM CDM reliability (Xiaomi/Huawei background-kill);
      may need `dontkillmyapp.com` guidance in Pair flow

## References

- [Find Hub Network Accessory Specification](https://developers.google.com/nearby/fast-pair/specifications/extensions/fmdn)
- [leonboe1/GoogleFindMyTools](https://github.com/leonboe1/GoogleFindMyTools) — GPL-3, study-only reference
- [PoPETS 2025 "Okay Google, Where's My Tracker?"](https://petsymposium.org/popets/2025/popets-2025-0147.pdf)
- [Nordic locator_tag sample](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/samples/bluetooth/fast_pair/locator_tag/README.html)
- [OpenHaystack](https://github.com/seemoo-lab/openhaystack) — Apple-side analogue
- [Android Unknown Tracker Alerts](https://support.google.com/android/answer/13658562)
- [CompanionDeviceManager guide](https://developer.android.com/develop/connectivity/bluetooth/ble/background)

## Appendix — EID derivation details

**Inputs:** 32-byte EIK, 32-bit `TS` (seconds since provisioning —
**offset from `pair_date`, not wall clock**), `K = 10` (fixed).

**Pipeline:**

1. `ts_masked = TS & 0xFFFFFC00`
2. Build 32-byte plaintext:
   ```
   offset  bytes
   0x00    0xFF × 11
   0x0B    0x0A (K)
   0x0C    ts_masked (big-endian, 4 B)
   0x10    0x00 × 11
   0x1B    0x0A (K)
   0x1C    ts_masked (big-endian, 4 B)
   ```
3. `r' = AES-ECB-256(EIK, block)` — yields 32 B (two 16-B blocks)
4. `r = int.from_bytes(r', 'big') % curve_order`
5. `R = r * G` on SECP160r1 (20 B) or SECP256R1 (32 B)
6. EID = big-endian `R.x`

**Frame (20-byte EID case):**
```
idx  val   meaning
00   0x02  len (flags AD)
01   0x01  AD = Flags
02   0x06  LE General Discoverable, BR/EDR not supported
03   0x19  len (service data AD = 25)
04   0x16  AD = Service Data 16-bit UUID
05   0xAA
06   0xFE  UUID = 0xFEAA
07   0x40  frame type (0x41 = UT mode ON)
08..1B     20-byte EID
1C         hashed flags byte = raw_flags XOR sha256(r)[31]
```

**Gotchas:**

- EIK is the AES key directly — no HKDF in the EID path (HKDF only
  appears in location-report decryption)
- Plaintext intentionally redundant; both 16-byte halves matter
- Big-endian everywhere; Cracen/mbedTLS may return little-endian
- Hashed flags byte uses `sha256(r)` (the scalar), not `sha256(EID)`
- Rotation jitter 1..204 s is mandatory, not polish
- MAC is independently random NRPA per rotation, **not derived from EID**
- Truncated EID = first 10 bytes; used only by backend in the
  precompute upload to Google, never broadcast

## Appendix — Research sources

Full paper trail for the research behind this plan. Every non-obvious
technical claim above has a source here.

### Google's published Find Hub Network specification

[`developers.google.com/nearby/fast-pair/specifications/extensions/fmdn`](https://developers.google.com/nearby/fast-pair/specifications/extensions/fmdn)
— the primary source. Sections we leaned on:

- §4 (EID computation): the AES-ECB-256 + curve-scalar-mult pipeline
- §6.2.2 (rotation cadence + jitter): 1024s + 1–204s random
- §7.1.4 (UT mode + MAC rotation): 1024s synchronized with EID
- Table 17 (32-byte AES plaintext block layout): exact byte shape
- Table 20 (clock sync event): fallback sync path after power loss
- Appendix A (precomputed EID upload): the 24h-phone-sync + 72h-lookahead
  mechanic, originally surfaced via the PoPETS paper

### leonboe1/GoogleFindMyTools (reference implementation)

[`github.com/leonboe1/GoogleFindMyTools`](https://github.com/leonboe1/GoogleFindMyTools)
— **GPL-3; study-only, do not vendor**. Commit `0003116` (2025-11-08)
used as reference. Key files:

- `FMDNCrypto/eid_generator.py` — EID derivation; the canonical
  Python reference for our C++ differential tests
- `FMDNCrypto/foreign_tracker_cryptor.py`, `FMDNCrypto/sha.py` —
  location-report decryption (AES-EAX, HKDF-SHA256)
- `Auth/{auth_flow,aas_token_retrieval,adm_token_retrieval,token_retrieval}.py`
  — the gpsoauth chain; documents the `client_sig` + app-bundle-id
  magic values
- `NovaApi/nova_request.py` — Nova HTTP endpoint, UA string, content type
- `SpotApi/spot_request.py` — Spot gRPC endpoint, cronet UA
- `SpotApi/CreateBleDevice/{create_ble_device,config}.py` —
  registration payload shape; the `max_truncated_eid_seconds_server`
  constant that changed 168h→96h on 2025-02-28 (commit `b650f8c`)
- `SpotApi/UploadPrecomputedPublicKeyIds/upload_precomputed_public_key_ids.py`
  — batch upload wire shape
- `NovaApi/ExecuteAction/LocateTracker/{location_request,decrypt_locations}.py`
  — locate action + FCM report decrypt
- `KeyBackup/shared_key_flow.py` — the Chrome vault-shared-key JS
  injection required for EIK decryption server-side
- `ESP32Firmware/README.md`, `ZephyrFirmware/README.md` — "the 4-day
  mechanic," "no MAC rotation," experimental-status caveats
- Issues #22 (decrypt InvalidTag), #74 (Runtime Error / ADM token),
  #78 (Google suspicious-activity), #82 (100-tag cap), #99
  (encryptedAccountKey random bytes)
- `BSkando/GoogleFindMyTools` — the most active downstream fork;
  watch for breakage fixes upstream hasn't picked up

### Academic paper

**"Okay Google, Where's My Tracker?" — Heinrich et al., PoPETS 2025/0147.**
[petsymposium.org/popets/2025/popets-2025-0147.pdf](https://petsymposium.org/popets/2025/popets-2025-0147.pdf)
— the only rigorous public security analysis of FHN. Sections we leaned on:

- §4.1.4, §4.1.6 — EID derivation cryptography, curve choices
- §6.1 — frame-layout confirmation from packet captures
- §6.1.1 — Sony WH-1000XM5 as the rare BLE 5 + P-256 observed in-wild
- §6.2.2 — rotation jitter enforcement in real hardware
- §6.2.4 — EID-collision DoS concerns on 10-byte truncation
- §7.1.4 — "MAC rotation interval is 1024 seconds, too short to
  trigger tracking notification"
- Appendix A — `UploadPrecomputedPublicKeyIds` mechanic, 72h window

### Nordic documentation

- [Find Hub locator_tag sample](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/samples/bluetooth/fast_pair/locator_tag/README.html)
  — Fast-Pair-certified path we are NOT using; referenced for
  test-harness patterns and Cracen-driver-selection conventions
- `nrfconnect/sdk-nrf/subsys/bluetooth/services/fast_pair/fp_crypto/fp_crypto_psa.c`
  — cleanest reference for `psa_import_key` + `psa_cipher_encrypt`
  (`PSA_ALG_ECB_NO_PADDING`) idioms; **Nordic-5-Clause, study-only**
- `.../fast_pair/fmdn/state.c` (`eid_encode()` around lines 171–260)
  — Nordic's production EID rotation logic
- `.../fast_pair/fp_storage/fp_storage_eik.c` — minimal Settings-backed
  EIK persistence pattern
- `sdk-nrf/samples/crypto/{hkdf,ecdh,ecdsa,kmu_cracen_usage}` — canonical
  PSA Crypto sample flows; `kmu_cracen_usage` is the definitive KMU
  provisioning reference
- [PSA Crypto + nRF Security DeepWiki](https://deepwiki.com/nrfconnect/sdk-nrf/4.1-psa-crypto-and-nrf-security)
  — practical overview of driver selection between Cracen and Oberon
- [Hardware Crypto Acceleration DeepWiki](https://deepwiki.com/nrfconnect/sdk-nrf/4.2-hardware-crypto-acceleration)
  — qualitative speedups vs software paths
- [Nordic DevZone: Google Find My Device implementation](https://devzone.nordicsemi.com/f/nordic-q-a/116741/google-find-my-device-network-implementation/515645)
  — confirms `0xFEAA` + `0x41` frame wiring

### Android / Google developer documentation

- [Android BLE background communication guide](https://developer.android.com/develop/connectivity/bluetooth/ble/background)
  — CompanionDeviceManager pattern, foreground-service-type requirements
- [Foreground service types (Android 14/15)](https://developer.android.com/about/versions/14/changes/fgs-types-required)
  — `FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE` requirement
- [CompanionDeviceSampleService sample](https://github.com/android/platform-samples/blob/main/samples/connectivity/bluetooth/cdm/CompanionDeviceSampleService.kt)
  — working reference for `onDeviceAppeared()` pattern
- [Punch Through — Android BLE Guide](https://punchthrough.com/android-ble-guide/)
  — GATT 133 lore, queue-of-one behavior
- [Android Unknown Tracker Alerts user docs](https://support.google.com/android/answer/13658562)
  — end-user view of what FHN tags trigger

### Nordic BLE library candidates

- [NordicSemiconductor/Android-Ble-library 2.9.0](https://github.com/NordicSemiconductor/Android-Ble-library)
  — chosen for companion app
- [NordicSemiconductor/Kotlin-BLE-Library 2.0-alphaXX](https://github.com/NordicSemiconductor/Kotlin-BLE-Library/releases)
  — evaluated; rejected (alpha, API unstable)
- [JuulLabs/Kable 0.42.0](https://github.com/JuulLabs/kable) —
  evaluated; noted as multiplatform option if iOS is ever added

### Third-party reverse-engineering coverage

- [CNX-Software (2025-02-12)](https://www.cnx-software.com/2025/02/12/googlefindmytools-locates-esp32-based-trackers-using-google-find-my-device-network/)
  — confirms "re-register every three days" cadence (tighter than
  earlier 4-day source)
- [Hackaday (2025-02-11)](https://hackaday.com/2025/02/11/google-findmy-tools-run-on-an-esp32/)
  — independent confirmation that the ESP32 path works end-to-end
- [Espruino forum discussion #4318](https://forum.espruino.com/conversations/386559/)
  — the "can we just broadcast without Fast Pair?" community
  discussion that predated leonboe's shipping code
- [Caesar Creek Software — Find My and Find Hub Network Research](https://cc-sw.com/find-my-and-find-hub-network-research/)
  — third-party protocol analysis (fetch failed during research;
  reference for future follow-up)
- [GitHub — seemoo-lab/openhaystack discussions #210, issues #245](https://github.com/seemoo-lab/openhaystack/discussions/210)
  — the "will OpenHaystack add FHN?" thread that confirmed there's
  no upstream effort

### Backend best-practice sources

- [OWASP OAuth 2.0 Cheat Sheet](https://cheatsheetseries.owasp.org/cheatsheets/OAuth2_Cheat_Sheet.html)
  — token storage guidance
- [Google Identity — OAuth 2.0 web server flow](https://developers.google.com/identity/protocols/oauth2/web-server)
  — refresh-token invalidation rules (6mo idle, 100-token cap, 7-day
  Testing-mode expiry); critical gotcha for our OAuth consent stance
- [pytest-httpserver](https://github.com/csernazs/pytest-httpserver)
  — real-TCP HTTP fixture for mock-server tests
- [Golioth — pytest on embedded hardware](https://blog.golioth.io/automated-hardware-testing-using-pytest/)
  — reference for structuring live-hardware pytest harnesses

### Cryptographic primitives / libraries

- [coincurve (Python bindings for libsecp256k1)](https://github.com/ofek/coincurve)
  — backend secp160r1 candidate; native-accelerated (open question:
  confirm P-160 support in distributed builds)
- [uECC (micro-ecc)](https://github.com/kmackay/micro-ecc) —
  software fallback for P-160 on 54L15 Design C (Cracen doesn't
  accelerate P-160)
- [eprint/2021/058 — Crypto-Hardware Performance Study on nRF52840](https://eprint.iacr.org/2021/058.pdf)
  — empirical numbers for Nordic hardware crypto vs software;
  methodology reference for if we ever need to measure Cracen energy

### OpenHaystack ecosystem (Apple-side analogue; shape reference)

- [seemoo-lab/openhaystack](https://github.com/seemoo-lab/openhaystack)
  — the original, Apple Find My target
- [dchristl/macless-haystack](https://github.com/dchristl/macless-haystack)
  — self-hosted alternative; AGPL-3 (incompatible for vendoring,
  informative as architectural reference)
- [biemster/FindMy](https://github.com/biemster/FindMy) — Apple-side
  report decryption tooling

### License notes

- Our project: TBD; should be compatible with all hard dependencies
- leonboe1/GoogleFindMyTools: **GPL-3** — study-only, reimplement
  clean in our codebase
- Nordic locator_tag and related FMDN sources: **Nordic-5-Clause** —
  study only; acceptable for runtime use on Nordic silicon if we ever
  choose Fast Pair path
- macless-haystack: **AGPL-3** — do not vendor
- BlueEkko/ADR/crypto primitives: mostly BSD / MIT / Apache — fine
