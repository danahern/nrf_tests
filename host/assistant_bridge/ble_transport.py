"""Bleak-based GATT transport for PSE84 voice assistant.

Scans for the device, connects, subscribes to the TX notify
characteristic, and writes to the RX characteristic. Both directions
use the |type|seq|len|payload| framing protocol.

Usage::

    transport = BLETransport(target_name="PSE84-Assistant",
                             on_frame=my_handler)
    await transport.connect()
    transport.send(encode_frame(FrameType.TEXT_CHUNK, 0, b"hello"))
    ...
    await transport.disconnect()
"""
from __future__ import annotations

import asyncio
import logging
from typing import Callable, Optional

from bleak import BleakClient, BleakScanner

from .framing import Frame, StreamingFrameParser

log = logging.getLogger(__name__)

# Must match gatt_svc.h on the firmware side
ASST_SERVICE_UUID = "a0e70001-e8b0-4aba-8200-a0e7a0e7a0e7"
ASST_TX_CHAR_UUID = "a0e70002-e8b0-4aba-8200-a0e7a0e7a0e7"
ASST_RX_CHAR_UUID = "a0e70003-e8b0-4aba-8200-a0e7a0e7a0e7"


class BLETransport:
    def __init__(self, target_name: str = "PSE84-Assistant",
                 on_frame: Optional[Callable[[Frame], None]] = None):
        self._target_name = target_name
        self._on_frame = on_frame
        self._client: Optional[BleakClient] = None
        self._parser = StreamingFrameParser()
        self._connected = False

    async def connect(self, timeout: float = 30.0):
        log.info("Scanning for '%s'…", self._target_name)
        device = await BleakScanner.find_device_by_name(
            self._target_name, timeout=timeout
        )
        if device is None:
            raise RuntimeError(
                f"Device '{self._target_name}' not found after {timeout}s"
            )

        log.info("Found %s (%s), connecting…", device.name, device.address)
        self._client = BleakClient(device)
        await self._client.connect()
        log.info("Connected")

        await self._client.start_notify(
            ASST_TX_CHAR_UUID, self._on_notify
        )
        log.info("Subscribed to TX notifications")
        self._connected = True

    def _on_notify(self, sender, data: bytearray):
        for frame in self._parser.feed(bytes(data)):
            if self._on_frame:
                try:
                    self._on_frame(frame)
                except Exception:
                    log.exception("on_frame error")

    async def send(self, data: bytes):
        if not self._client or not self._client.is_connected:
            return
        await self._client.write_gatt_char(
            ASST_RX_CHAR_UUID, data, response=False
        )

    @property
    def is_connected(self) -> bool:
        return self._connected and self._client is not None and self._client.is_connected

    async def disconnect(self):
        self._connected = False
        if self._client and self._client.is_connected:
            await self._client.disconnect()
        log.info("Disconnected")
