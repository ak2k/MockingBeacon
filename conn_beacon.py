#!/usr/bin/python3
"""Connect to an Everytag beacon and configure settings via BLE GATT."""

import argparse
import binascii
import struct
import sys
import time
from datetime import datetime, timezone

import simplepyble

# ---- GATT UUIDs (from gatt_glue.c) ----

SVC = "5cfce313-a7e3-45c3-933d-418b8100da7f"
UUID_FMDN_FLAG = "8c5debdb-ad8d-4810-a31f-53862e79ee77"
UUID_AIRTAG_FLAG = "8c5debdc-ad8d-4810-a31f-53862e79ee77"
UUID_DELAY = "8c5debdd-ad8d-4810-a31f-53862e79ee77"
UUID_KEYS = "8c5debde-ad8d-4810-a31f-53862e79ee77"
UUID_AUTH = "8c5debdf-ad8d-4810-a31f-53862e79ee77"
UUID_INTERVAL = "8c5debe0-ad8d-4810-a31f-53862e79ee77"
UUID_TXPOWER = "8c5debe1-ad8d-4810-a31f-53862e79ee77"
UUID_FMDN_KEY = "8c5debe2-ad8d-4810-a31f-53862e79ee77"
UUID_TIME = "8c5debe3-ad8d-4810-a31f-53862e79ee77"
UUID_SETTINGS_MAC = "8c5debe4-ad8d-4810-a31f-53862e79ee77"
UUID_STATUS = "8c5debe5-ad8d-4810-a31f-53862e79ee77"
UUID_ACCEL = "8c5debe6-ad8d-4810-a31f-53862e79ee77"


def write(peripheral, uuid, data):
    peripheral.write_request(SVC, uuid, data)


def write_int32(peripheral, uuid, value):
    write(peripheral, uuid, struct.pack("<i", value))


def scan_and_connect(mac):
    adapters = simplepyble.Adapter.get_adapters()
    adapter = adapters[0]
    print("Waiting for connect, place device close to the computer.")
    while True:
        adapter.scan_for(50)
        for peripheral in adapter.scan_get_results():
            if peripheral.address() == mac.upper():
                print(f"Found, trying to connect to {peripheral.address()}")
                try:
                    peripheral.connect()
                    return peripheral
                except KeyboardInterrupt:
                    sys.exit(1)
                except Exception:
                    print("Connect failed, returning to scan")
            else:
                print(f"Found other device: {peripheral.address()}")


ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument("-i", "--macid", required=True, help="BLE MAC address")
ap.add_argument("-a", "--auth", required=True, help="Authentication code")
ap.add_argument("-n", "--newauth", help="New authentication code")
ap.add_argument("-c", "--newmacid", help="New MAC address for settings mode")
ap.add_argument("-g", "--fmdn", help="Google FMDN broadcast (0 or 1)")
ap.add_argument("-t", "--airtag", help="AirTag broadcast (0 or 1)")
ap.add_argument("-d", "--delay", help="Broadcast period multiplier (1, 2, 4, 8)")
ap.add_argument("-r", "--readtime", help="Read time from beacon (pass 1)")
ap.add_argument("-w", "--writetime", help="Write current time (pass 1)")
ap.add_argument("-l", "--interval", help="Key change interval in seconds")
ap.add_argument("-s", "--statusbyte", help="Status byte config (hex)")
ap.add_argument("-m", "--movethreshold", help="Accelerometer threshold (0=off)")
ap.add_argument("-p", "--txpower", help="TX power (0=low, 1=normal, 2=high)")
ap.add_argument("-k", "--keyfile", help="Binary public keys file")
ap.add_argument("-f", "--fmdnkey", help="Google FMDN key (hex)")
ap.add_argument(
    "-u", "--dfu", action="store_true", help="Enter DFU mode after disconnect"
)

args = vars(ap.parse_args())

peripheral = scan_and_connect(args["macid"])
print("Connected")

# Authenticate
write(peripheral, UUID_AUTH, args["auth"].encode())

# Upload keys
if args.get("keyfile"):
    zero_key = bytes(14)
    with open(args["keyfile"], "rb") as f:
        n_keys = min(f.read(1)[0], 40)
        print(f"Reading {n_keys} keys")
        for _ in range(n_keys * 2):
            write(peripheral, UUID_KEYS, f.read(14))
        if n_keys < 40:
            write(peripheral, UUID_KEYS, zero_key)
            write(peripheral, UUID_KEYS, zero_key)

if args.get("fmdn") is not None:
    write_int32(peripheral, UUID_FMDN_FLAG, 0 if args["fmdn"] == "0" else 1)

if args.get("airtag") is not None:
    write_int32(peripheral, UUID_AIRTAG_FLAG, 0 if args["airtag"] == "0" else 1)

if args.get("delay") is not None:
    write_int32(peripheral, UUID_DELAY, int(args["delay"]))

if args.get("txpower") is not None:
    write_int32(peripheral, UUID_TXPOWER, int(args["txpower"]))

if args.get("interval") is not None:
    write_int32(peripheral, UUID_INTERVAL, int(args["interval"]))

if args.get("readtime") is not None:
    value = peripheral.read(SVC, UUID_TIME)
    epoch = int.from_bytes(value, byteorder="little")
    t = datetime.fromtimestamp(epoch, tz=timezone.utc).strftime("%Y-%m-%d %H:%M:%S")
    print(f"Time on beacon (UTC): {t} ({epoch})")

if args.get("writetime") is not None:
    write(peripheral, UUID_TIME, struct.pack("<q", int(time.time())))

if args.get("fmdnkey") is not None:
    write(peripheral, UUID_FMDN_KEY, bytes.fromhex(args["fmdnkey"]))

if args.get("movethreshold") is not None:
    write_int32(peripheral, UUID_ACCEL, int(args["movethreshold"]))

if args.get("statusbyte") is not None:
    write_int32(peripheral, UUID_STATUS, int(args["statusbyte"], 16))

if args.get("newmacid") is not None:
    write(
        peripheral,
        UUID_SETTINGS_MAC,
        binascii.unhexlify(args["newmacid"].replace(":", ""))[::-1],
    )

if args.get("newauth") is not None:
    write(peripheral, UUID_AUTH, args["newauth"].encode())

if args.get("dfu"):
    # Send reserved DFU trigger (0xDFDF0001) via status characteristic.
    # Firmware sets pauseUpload=1, enabling SMP service after disconnect.
    write_int32(peripheral, UUID_STATUS, 0xDFDF0001)

peripheral.disconnect()
