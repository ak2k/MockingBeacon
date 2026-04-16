#!/usr/bin/env python3
# /// script
# requires-python = ">=3.12"
# dependencies = ["bumble", "pytest", "pytest-asyncio"]
# ///
"""
Integration test: runs conn_beacon.py's GATT operations against a
Bumble virtual peripheral. Mocks simplepyble to route BLE calls
through Bumble's in-process virtual transport.

This tests the actual conn_beacon.py logic without a real BLE adapter.

Run: uv run --with bumble --with pytest --with pytest-asyncio \
     pytest tests/ble_client/test_conn_beacon_integration.py -v
"""

import asyncio
import sys
import types
from pathlib import Path
from unittest.mock import MagicMock

import pytest
from bumble.controller import Controller
from bumble.device import Device, Peer
from bumble.host import Host
from bumble.link import LocalLink
from bumble.hci import Address

# Reuse UUIDs and server from the unit tests
from test_conn_beacon import (
    UUID_AIRTAG_FLAG,
    UUID_AUTH,
    UUID_DELAY,
    UUID_FMDN_FLAG,
    UUID_FMDN_KEY,
    UUID_KEYS,
    UUID_TXPOWER,
    UUID_INTERVAL,
    UUID_TIME,
    UUID_SETTINGS_MAC,
    UUID_STATUS,
    UUID_ACCEL,
    BeaconGattServer,
)


class BumbleMockPeripheral:
    """Mocks simplepyble.Peripheral backed by a Bumble GATT client."""

    def __init__(self, peer: Peer, addr: str):
        self._peer = peer
        self._addr = addr
        self._connected = True
        self._chars: dict[str, object] = {}
        # Build char lookup
        for service in peer.services:
            for char in service.characteristics:
                self._chars[str(char.uuid).upper()] = char

    def address(self):
        return self._addr

    def connect(self):
        pass  # already connected

    def disconnect(self):
        self._connected = False

    def services(self):
        return []  # not used by conn_beacon.py

    def write_request(self, service_uuid, char_uuid, data):
        char = self._chars.get(char_uuid.upper())
        if char is None:
            raise ValueError(f"Characteristic {char_uuid} not found")
        loop = asyncio.get_event_loop()
        loop.run_until_complete(self._peer.write_value(char, data, with_response=True))

    def read(self, service_uuid, char_uuid):
        char = self._chars.get(char_uuid.upper())
        if char is None:
            raise ValueError(f"Characteristic {char_uuid} not found")
        loop = asyncio.get_event_loop()
        return loop.run_until_complete(self._peer.read_value(char))


def make_mock_simplepyble(peer: Peer, target_addr: str):
    """Create a mock simplepyble module that returns our Bumble-backed peripheral."""
    mock_peripheral = BumbleMockPeripheral(peer, target_addr)

    mock_adapter = MagicMock()
    mock_adapter.scan_for = MagicMock()
    mock_adapter.scan_get_results = MagicMock(return_value=[mock_peripheral])

    module = types.ModuleType("simplepyble")
    module.Adapter = MagicMock()
    module.Adapter.get_adapters = MagicMock(return_value=[mock_adapter])

    return module, mock_peripheral


async def _setup_beacon():
    """Async setup for Bumble virtual BLE."""
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

    return server, peer, connection


@pytest.fixture
def beacon_and_mock():
    """Set up Bumble virtual BLE and return mock simplepyble + server."""
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)

    server, peer, connection = loop.run_until_complete(_setup_beacon())
    mock_module, mock_peripheral = make_mock_simplepyble(peer, "F0:F1:F2:F3:F4:F5")

    yield server, mock_module, mock_peripheral

    loop.run_until_complete(connection.disconnect())
    loop.close()


def run_conn_beacon(mock_simplepyble, args: list[str]):
    """Run conn_beacon.py with mocked simplepyble and given CLI args."""
    # Inject mock simplepyble
    original = sys.modules.get("simplepyble")
    sys.modules["simplepyble"] = mock_simplepyble

    # Set sys.argv for argparse
    original_argv = sys.argv
    sys.argv = ["conn_beacon.py"] + args

    try:
        # Read and exec the script
        script = Path(__file__).parent.parent.parent / "conn_beacon.py"
        exec(compile(script.read_text(), str(script), "exec"), {"__name__": "__main__"})
    finally:
        sys.argv = original_argv
        if original is not None:
            sys.modules["simplepyble"] = original
        else:
            sys.modules.pop("simplepyble", None)


# ---- Integration tests ----


def test_enable_airtag(beacon_and_mock):
    """conn_beacon.py -i <MAC> -a <auth> -t 1 should enable AirTag."""
    server, mock_module, _ = beacon_and_mock

    run_conn_beacon(
        mock_module, ["-i", "F0:F1:F2:F3:F4:F5", "-a", "abcdefgh", "-t", "1"]
    )

    assert server.authorized
    assert server.get_writes(UUID_AIRTAG_FLAG) == [b"\x01\x00\x00\x00"]


def test_set_tx_power(beacon_and_mock):
    """conn_beacon.py -p 2 should set high TX power."""
    server, mock_module, _ = beacon_and_mock

    run_conn_beacon(
        mock_module, ["-i", "F0:F1:F2:F3:F4:F5", "-a", "abcdefgh", "-p", "2"]
    )

    assert server.authorized
    assert server.get_writes(UUID_TXPOWER) == [b"\x02\x00\x00\x00"]


def test_set_interval(beacon_and_mock):
    """conn_beacon.py -l 600 should set 600-second key interval."""
    server, mock_module, _ = beacon_and_mock

    run_conn_beacon(
        mock_module, ["-i", "F0:F1:F2:F3:F4:F5", "-a", "abcdefgh", "-l", "600"]
    )

    assert server.get_writes(UUID_INTERVAL) == [(600).to_bytes(4, byteorder="little")]


def test_multiple_settings(beacon_and_mock):
    """Set multiple options in one invocation."""
    server, mock_module, _ = beacon_and_mock

    run_conn_beacon(
        mock_module,
        [
            "-i",
            "F0:F1:F2:F3:F4:F5",
            "-a",
            "abcdefgh",
            "-t",
            "1",
            "-g",
            "1",
            "-d",
            "2",
            "-p",
            "0",
        ],
    )

    assert server.get_writes(UUID_AIRTAG_FLAG) == [b"\x01\x00\x00\x00"]
    assert server.get_writes(UUID_FMDN_FLAG) == [b"\x01\x00\x00\x00"]
    assert server.get_writes(UUID_DELAY) == [b"\x02\x00\x00\x00"]
    assert server.get_writes(UUID_TXPOWER) == [b"\x00\x00\x00\x00"]


def test_new_auth_code(beacon_and_mock):
    """conn_beacon.py -n newpass1 should write new auth code."""
    server, mock_module, _ = beacon_and_mock

    run_conn_beacon(
        mock_module,
        ["-i", "F0:F1:F2:F3:F4:F5", "-a", "abcdefgh", "-n", "newpass1"],
    )

    # First write is the auth, second is the new auth code
    writes = server.get_writes(UUID_AUTH)
    assert len(writes) == 2
    assert writes[0] == b"abcdefgh"
    assert writes[1] == b"newpass1"


def test_new_settings_mac(beacon_and_mock):
    """conn_beacon.py -c AA:BB:CC:DD:EE:FF should set settings MAC."""
    server, mock_module, _ = beacon_and_mock

    run_conn_beacon(
        mock_module,
        ["-i", "F0:F1:F2:F3:F4:F5", "-a", "abcdefgh", "-c", "AA:BB:CC:DD:EE:FF"],
    )

    # conn_beacon.py reverses the MAC bytes
    expected = bytes.fromhex("AABBCCDDEEFF")[::-1]
    assert server.get_writes(UUID_SETTINGS_MAC) == [expected]


def test_write_time(beacon_and_mock):
    """conn_beacon.py -w 1 should write current time."""
    server, mock_module, _ = beacon_and_mock

    run_conn_beacon(
        mock_module,
        ["-i", "F0:F1:F2:F3:F4:F5", "-a", "abcdefgh", "-w", "1"],
    )

    writes = server.get_writes(UUID_TIME)
    assert len(writes) == 1
    # Should be 8 bytes little-endian, value close to current time
    assert len(writes[0]) == 8


def test_read_time(beacon_and_mock):
    """conn_beacon.py -r 1 should read time (no write to time char)."""
    server, mock_module, _ = beacon_and_mock

    run_conn_beacon(
        mock_module,
        ["-i", "F0:F1:F2:F3:F4:F5", "-a", "abcdefgh", "-r", "1"],
    )

    # Read-only — no writes to time characteristic
    assert server.get_writes(UUID_TIME) == []


def test_statusbyte(beacon_and_mock):
    """conn_beacon.py -s 438000 should write status byte config."""
    server, mock_module, _ = beacon_and_mock

    run_conn_beacon(
        mock_module,
        ["-i", "F0:F1:F2:F3:F4:F5", "-a", "abcdefgh", "-s", "438000"],
    )

    expected = (0x438000).to_bytes(4, byteorder="little")
    assert server.get_writes(UUID_STATUS) == [expected]


def test_movethreshold(beacon_and_mock):
    """conn_beacon.py -m 800 should write accelerometer threshold."""
    server, mock_module, _ = beacon_and_mock

    run_conn_beacon(
        mock_module,
        ["-i", "F0:F1:F2:F3:F4:F5", "-a", "abcdefgh", "-m", "800"],
    )

    expected = (800).to_bytes(4, byteorder="little")
    assert server.get_writes(UUID_ACCEL) == [expected]


def test_fmdn_key(beacon_and_mock):
    """conn_beacon.py -f <hex> should write FMDN key."""
    server, mock_module, _ = beacon_and_mock

    key_hex = "0102030405060708090a0b0c0d0e0f1011121314"
    run_conn_beacon(
        mock_module,
        ["-i", "F0:F1:F2:F3:F4:F5", "-a", "abcdefgh", "-f", key_hex],
    )

    assert server.get_writes(UUID_FMDN_KEY) == [bytes.fromhex(key_hex)]


def test_keyfile(beacon_and_mock, tmp_path):
    """conn_beacon.py -k <keyfile> should upload keys in 14-byte chunks."""
    server, mock_module, _ = beacon_and_mock

    # Create a test keyfile: 1 byte (num_keys=2) + 2 keys × 28 bytes
    keyfile = tmp_path / "test.keys"
    num_keys = 2
    key_data = bytes([num_keys]) + bytes(range(56))  # 2 keys × 28 bytes
    keyfile.write_bytes(key_data)

    run_conn_beacon(
        mock_module,
        ["-i", "F0:F1:F2:F3:F4:F5", "-a", "abcdefgh", "-k", str(keyfile)],
    )

    # 2 keys × 2 chunks = 4 data writes + 2 zero-terminator writes = 6
    writes = server.get_writes(UUID_KEYS)
    assert len(writes) == 6
    for w in writes:
        assert len(w) == 14
    # Last two are zero terminators
    assert writes[-1] == b"\x00" * 14
    assert writes[-2] == b"\x00" * 14


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
