# Everytag vs Apple AirTag — Design Comparison

This document compares the Everytag firmware's approach to that of a real Apple AirTag, based on publicly available reverse engineering research. Understanding these differences helps inform future optimization decisions.

## Sources

- Heinrich, Stute, Kornhuber, Hollick (TU Darmstadt, 2021). "Who Can Find My Devices? Security and Privacy of Apple's Crowd-Sourced Bluetooth Location Tracking System." *Proceedings on Privacy Enhancing Technologies (PoPETs)*, 2021(3). — The foundational reverse engineering paper of the Apple Find My protocol.
- [OpenHaystack](https://github.com/seemoo-lab/openhaystack) (TU Darmstadt Secure Mobile Networking Lab) — open-source framework documenting the advertisement format and key derivation.
- Positive Security / Fabian Bräunlein (2021). "Send My: Arbitrary data transmission via Apple's Find My network." — Proof-of-concept demonstrating data exfiltration over Find My advertisements, with packet capture analysis.
- FCC filing BCG-A2187 — RF test reports for AirTag, confirming BLE operating parameters.
- iFixit AirTag teardown — confirmed nRF52832 SoC, Bosch BMA280 accelerometer, U1 UWB chip.

## Hardware

| | Apple AirTag | Everytag |
|---|---|---|
| SoC | nRF52832 (custom Apple firmware) | nRF52805/810/832/833, nRF54L15 |
| Accelerometer | Bosch BMA280 | LIS2DW12 (on supported boards) |
| UWB | Apple U1 (Precision Finding) | None |
| Battery | CR2032 (~1 year) | Varies by board |
| Speaker | Yes (separation alert) | No |

## BLE Advertising

| | Apple AirTag | Everytag | Notes |
|---|---|---|---|
| Advertisement type | ADV_NONCONN_IND | Connectable (during settings window) | AirTag is non-connectable in normal operation; only becomes connectable on-demand from the owner's iPhone |
| Advertisement interval | ~2 seconds | ~7 seconds (configurable: 1/2/4/8s) | AirTag's 2s provides better detection by passing phones |
| TX power | Constant ~0 to +4 dBm | Configurable: -8/0/+4 dBm | No evidence AirTag varies TX power |
| Periodic power burst | Not observed | Every 68 seconds, brief +4/+8 dBm burst | Everytag's own optimization; not based on AirTag behavior |
| Payload | 28-byte public key + status byte + hint byte | Same format (Apple Offline Finding compatible) | Byte-identical advertisement structure |

## Key Rotation

| | Apple AirTag | Everytag | Notes |
|---|---|---|---|
| Near owner | ~15 minutes | N/A (no owner proximity detection) | AirTag syncs key schedule with paired iPhone |
| Separated from owner | ~24 hours | Configurable (default 10 minutes) | AirTag slows rotation to enable anti-stalking detection |
| Key derivation | Deterministic KDF from shared secret + time counter | Pre-generated key ring loaded via BLE | AirTag derives keys on-device; Everytag loads them externally |
| Max keys | Unlimited (derived on-the-fly) | 40 (stored in flash) | Everytag's 40-key ring wraps around |

## Power Management

| | Apple AirTag | Everytag | Notes |
|---|---|---|---|
| Sleep strategy | Deep sleep with accelerometer interrupt wake | Continuous advertising with periodic settings window | AirTag is more aggressive about sleeping |
| Connectable mode | On-demand only (owner initiates) | 2 seconds every minute | Everytag trades power for field configurability |
| Scanning (RX) | Never | Never | Both are pure broadcasters in normal operation |
| Motion detection | Accelerometer interrupt gates advertising | Accelerometer samples movement for status byte | AirTag uses motion to wake; Everytag uses it for telemetry |
| Estimated battery life | ~1 year (CR2032) | Depends on configuration and board | Lower TX power + longer interval = longer life |

## Status Byte

| | Apple AirTag | Everytag |
|---|---|---|
| Battery level | 2-bit field (full/medium/low/critical) | Configurable: voltage, level, or telemetry cycle |
| Movement flag | Single bit indicating current motion | 7-bit movement summary (time-bucketed history) |
| Temperature | Not reported | Available via LIS2DW12 die temperature |
| Cycling | Fixed format | Can cycle voltage/accel/temperature each minute |

## Anti-Stalking

| | Apple AirTag | Everytag |
|---|---|---|
| Sound alert | After 8-24h separated from owner | None (no speaker) |
| iOS detection | 24h key rotation enables tracking detection | Fast rotation may evade detection |
| Android detection | Google Tracker Detect app | Not specifically addressed |

## Key Differences Summary

1. **Connectivity model**: AirTag is non-connectable almost always; Everytag opens a connectable window every minute for settings. This is the biggest power consumption difference.

2. **Key management**: AirTag derives keys deterministically from a shared secret, enabling unlimited key rotation. Everytag uses a pre-loaded key ring (max 40 keys) that wraps around.

3. **Power bursts**: Everytag's 68-second high-power burst is a unique feature not observed in real AirTags. Its effectiveness is unverified.

4. **Anti-stalking**: AirTag has multiple anti-stalking mechanisms (slow key rotation when separated, sound alerts, iOS/Android detection). Everytag does not implement these.
