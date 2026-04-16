#!/usr/bin/env python3
# /// script
# requires-python = ">=3.12"
# dependencies = ["bumble", "pytest", "pytest-asyncio"]
# ///
"""
BLE client tests using Google Bumble virtual transport.
Tests the same GATT operations as conn_beacon.py without real hardware.

Run: uv run tests/ble_client/test_conn_beacon.py
Or:  uv run --with bumble --with pytest --with pytest-asyncio pytest tests/ble_client/ -v
"""

import asyncio

import pytest
import pytest_asyncio
from bumble.att import ATT_WRITE_NOT_PERMITTED_ERROR
from bumble.controller import Controller
from bumble.device import Device, Peer
from bumble.gatt import (
    Attribute,
    Characteristic,
    CharacteristicValue,
    Service,
)
from bumble.host import Host
from bumble.link import LocalLink
from bumble.hci import Address

# ---- Everytag GATT UUIDs (from gatt_glue.c) ----

SERVICE_UUID = "5cfce313-a7e3-45c3-933d-418b8100da7f"

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

AUTH_CODE = b"abcdefgh"
WRONG_AUTH = b"wrongpwd"

WRITABLE = Characteristic.Properties(Characteristic.READABLE | Characteristic.WRITEABLE)
PERMISSIONS = Attribute.Permissions(Attribute.READABLE | Attribute.WRITEABLE)

ALL_SETTING_UUIDS = [
    UUID_FMDN_FLAG,
    UUID_AIRTAG_FLAG,
    UUID_DELAY,
    UUID_KEYS,
    UUID_INTERVAL,
    UUID_TXPOWER,
    UUID_FMDN_KEY,
    UUID_TIME,
    UUID_SETTINGS_MAC,
    UUID_STATUS,
    UUID_ACCEL,
]


# ---- Beacon GATT server (mirrors gatt_glue.c auth logic) ----


class BeaconGattServer:
    """Simulates the Everytag beacon's GATT service with auth enforcement.

    Mirrors gatt_glue.c behavior:
    - write_authorize() rejects all writes unless authorized or targeting auth char
    - Auth char write checks the code and sets authorized/allowedChange
    - Setting char writes check allowedChange before accepting
    """

    def __init__(self):
        self.authorized = False
        self.allowed_change = False
        self.writes: dict[str, list[bytes]] = {}
        self.rejected_writes: dict[str, int] = {}

    # Mirror gatt_glue.c .disconnected + .recycled behavior: clear auth
    # state when the previous client goes away. Without this, a fresh
    # client could inherit prior auth — same defect class as the firmware
    # bug fixed in Phase 1β.
    def on_disconnected(self):
        self.authorized = False
        self.allowed_change = False

    def _make_auth_value(self):
        def write(connection, value):
            if bytes(value) == AUTH_CODE:
                self.authorized = True
                self.allowed_change = True
            else:
                self.authorized = False
                self.allowed_change = False
            self.writes.setdefault(UUID_AUTH, []).append(bytes(value))

        def read(connection):
            return AUTH_CODE

        return CharacteristicValue(read=read, write=write)

    def _make_setting_value(self, uuid: str, initial: bytes = b"\x00\x00\x00\x00"):
        storage = bytearray(initial)

        def write(connection, value):
            if not self.allowed_change:
                self.rejected_writes[uuid] = self.rejected_writes.get(uuid, 0) + 1
                raise ATT_WRITE_NOT_PERMITTED_ERROR
            storage[:] = value
            self.writes.setdefault(uuid, []).append(bytes(value))

        def read(connection):
            return bytes(storage)

        return CharacteristicValue(read=read, write=write)

    def make_service(self) -> Service:
        chars = [
            Characteristic(
                UUID_AUTH, WRITABLE, PERMISSIONS, value=self._make_auth_value()
            ),
        ]
        for uuid in ALL_SETTING_UUIDS:
            chars.append(
                Characteristic(
                    uuid, WRITABLE, PERMISSIONS, value=self._make_setting_value(uuid)
                )
            )
        return Service(SERVICE_UUID, chars)

    def get_writes(self, uuid: str) -> list[bytes]:
        return self.writes.get(uuid, [])

    def get_rejected_count(self, uuid: str) -> int:
        return self.rejected_writes.get(uuid, 0)


# ---- Fixtures ----


@pytest_asyncio.fixture
async def beacon_env():
    """Set up two Bumble devices connected via virtual link."""
    link = LocalLink()

    server_ctrl = Controller("server", link=link)
    client_ctrl = Controller("client", link=link)

    server_host = Host(server_ctrl, server_ctrl)
    client_host = Host(client_ctrl, client_ctrl)

    server = BeaconGattServer()
    server_device = Device(
        name="beacon", address=Address("F0:F1:F2:F3:F4:F5"), host=server_host
    )
    server_device.add_service(server.make_service())

    client_device = Device(
        name="client", address=Address("C0:C1:C2:C3:C4:C5"), host=client_host
    )

    await server_device.power_on()
    await client_device.power_on()
    await server_device.start_advertising(auto_restart=True)

    connection = await client_device.connect(Address("F0:F1:F2:F3:F4:F5"))
    peer = Peer(connection)
    await peer.discover_services()
    await peer.discover_characteristics()

    yield server, peer, connection

    await connection.disconnect()


# ---- Helpers ----


async def write_char(peer: Peer, uuid: str, value: bytes):
    target = uuid.upper()
    for service in peer.services:
        for char in service.characteristics:
            if str(char.uuid).upper() == target:
                await peer.write_value(char, value, with_response=True)
                return
    raise ValueError(f"Characteristic {uuid} not found")


async def read_char(peer: Peer, uuid: str) -> bytes:
    target = uuid.upper()
    for service in peer.services:
        for char in service.characteristics:
            if str(char.uuid).upper() == target:
                return await peer.read_value(char)
    raise ValueError(f"Characteristic {uuid} not found")


# ---- Tests: Auth enforcement ----


@pytest.mark.asyncio
async def test_write_before_auth_rejected(beacon_env):
    """Writes to setting characteristics before auth should be rejected."""
    server, peer, _ = beacon_env

    # Try writing without authenticating first
    with pytest.raises(Exception):
        await write_char(peer, UUID_AIRTAG_FLAG, b"\x01\x00\x00\x00")

    await asyncio.sleep(0.05)
    assert server.get_writes(UUID_AIRTAG_FLAG) == []
    assert server.get_rejected_count(UUID_AIRTAG_FLAG) >= 1


@pytest.mark.asyncio
async def test_wrong_auth_then_write_rejected(beacon_env):
    """Wrong auth code should leave writes rejected."""
    server, peer, _ = beacon_env

    await write_char(peer, UUID_AUTH, WRONG_AUTH)
    await asyncio.sleep(0.05)
    assert not server.authorized

    with pytest.raises(Exception):
        await write_char(peer, UUID_AIRTAG_FLAG, b"\x01\x00\x00\x00")

    await asyncio.sleep(0.05)
    assert server.get_writes(UUID_AIRTAG_FLAG) == []


@pytest.mark.asyncio
async def test_auth_always_writable(beacon_env):
    """Auth characteristic should always accept writes (even when not authorized)."""
    server, peer, _ = beacon_env

    # First write with wrong code — should still be accepted (not rejected)
    await write_char(peer, UUID_AUTH, WRONG_AUTH)
    await asyncio.sleep(0.05)
    assert server.get_writes(UUID_AUTH) == [WRONG_AUTH]
    assert not server.authorized

    # Second write with correct code
    await write_char(peer, UUID_AUTH, AUTH_CODE)
    await asyncio.sleep(0.05)
    assert server.authorized


# ---- Tests: Authenticated operations ----


@pytest.mark.asyncio
async def test_auth_then_enable_airtag(beacon_env):
    """Auth with correct code, then enable AirTag broadcasting."""
    server, peer, _ = beacon_env

    await write_char(peer, UUID_AUTH, AUTH_CODE)
    await asyncio.sleep(0.05)
    assert server.authorized

    await write_char(peer, UUID_AIRTAG_FLAG, b"\x01\x00\x00\x00")
    await asyncio.sleep(0.05)
    assert server.get_writes(UUID_AIRTAG_FLAG) == [b"\x01\x00\x00\x00"]


@pytest.mark.asyncio
async def test_key_upload(beacon_env):
    """Upload key chunks (same as conn_beacon.py keyfile flow)."""
    server, peer, _ = beacon_env

    await write_char(peer, UUID_AUTH, AUTH_CODE)
    await asyncio.sleep(0.05)

    # 3 keys × 2 chunks × 14 bytes each
    for i in range(6):
        await write_char(peer, UUID_KEYS, bytes([i] * 14))

    await asyncio.sleep(0.05)
    assert len(server.get_writes(UUID_KEYS)) == 6


@pytest.mark.asyncio
async def test_time_read_write(beacon_env):
    """Write time, read it back."""
    server, peer, _ = beacon_env

    await write_char(peer, UUID_AUTH, AUTH_CODE)
    await asyncio.sleep(0.05)

    timestamp = (1700000000).to_bytes(8, byteorder="little")
    await write_char(peer, UUID_TIME, timestamp)
    await asyncio.sleep(0.05)

    readback = await read_char(peer, UUID_TIME)
    assert readback == timestamp


@pytest.mark.asyncio
async def test_all_settings(beacon_env):
    """Set every configurable option (mirrors conn_beacon.py full invocation)."""
    server, peer, _ = beacon_env

    await write_char(peer, UUID_AUTH, AUTH_CODE)
    await asyncio.sleep(0.05)

    settings = {
        UUID_FMDN_FLAG: b"\x01\x00\x00\x00",
        UUID_AIRTAG_FLAG: b"\x01\x00\x00\x00",
        UUID_DELAY: b"\x02\x00\x00\x00",
        UUID_TXPOWER: b"\x02\x00\x00\x00",
        UUID_INTERVAL: (600).to_bytes(4, byteorder="little"),
        UUID_STATUS: (0x438000).to_bytes(4, byteorder="little"),
        UUID_ACCEL: (800).to_bytes(4, byteorder="little"),
        UUID_FMDN_KEY: bytes(20),
        UUID_SETTINGS_MAC: bytes.fromhex("F5F4F3F2F1F0"),
    }

    for uuid, value in settings.items():
        await write_char(peer, uuid, value)

    await asyncio.sleep(0.05)
    for uuid, value in settings.items():
        assert server.get_writes(uuid) == [value], f"Mismatch for {uuid}"


@pytest.mark.asyncio
async def test_new_auth_code(beacon_env):
    """Change auth code (writing to auth char after authorization)."""
    server, peer, _ = beacon_env

    await write_char(peer, UUID_AUTH, AUTH_CODE)
    await asyncio.sleep(0.05)
    assert server.authorized

    # Write new auth code (same characteristic, while authorized)
    await write_char(peer, UUID_AUTH, b"newpassw")
    await asyncio.sleep(0.05)
    # Server should record both writes
    assert server.get_writes(UUID_AUTH) == [AUTH_CODE, b"newpassw"]


# ---- Run directly ----

if __name__ == "__main__":
    pytest.main([__file__, "-v"])
