"""PyObjC CoreBluetooth L2CAP CoC transport for PSE84 voice assistant.

Bleak doesn't expose L2CAP CoC on macOS. This module uses
CoreBluetooth directly via PyObjC to scan, connect, and open a
channel on a fixed PSM (0x0080). Incoming SDUs are fed through
:class:`StreamingFrameParser`; outbound frames are written to the
L2CAP output stream.

Runs an NSRunLoop in a background thread. The caller interacts via
two callbacks:
  - ``on_frame(frame)`` — called for each complete inbound frame
  - ``send(raw_bytes)`` — queues bytes for the output stream

Usage::

    transport = BLETransport(target_name="PSE84-Assistant",
                             psm=0x0080,
                             on_frame=my_frame_handler)
    transport.start()   # starts scan + NSRunLoop in background thread
    transport.wait_connected(timeout=30)
    transport.send(encode_frame(FrameType.TEXT_CHUNK, 0, b"hello"))
    ...
    transport.stop()
"""
from __future__ import annotations

import struct
import threading
import time
from typing import Callable, Optional

from Foundation import (
    NSObject, NSRunLoop, NSDate, NSDefaultRunLoopMode, NSTimer,
)
from CoreBluetooth import (
    CBCentralManager, CBPeripheral,
    CBManagerStatePoweredOn,
    CBUUID,
)
import objc

from .framing import Frame, StreamingFrameParser

import logging
log = logging.getLogger(__name__)


class _Delegate(NSObject):
    """NSObject delegate that bridges CoreBluetooth events to Python."""

    def init(self):
        self = objc.super(_Delegate, self).init()
        if self is None:
            return None
        self.target_name: str = "PSE84-Assistant"
        self.psm: int = 0x0080
        self.peripheral: Optional[CBPeripheral] = None
        self.channel = None
        self.output_stream = None
        self.connected_event = threading.Event()
        self.parser = StreamingFrameParser()
        self._on_frame: Optional[Callable] = None
        self._tx_lock = threading.Lock()
        self._tx_pending = bytearray()
        self._stream_ready = False
        self.central = CBCentralManager.alloc().initWithDelegate_queue_(self, None)
        return self

    # -- CBCentralManagerDelegate --

    def centralManagerDidUpdateState_(self, central):
        if central.state() == CBManagerStatePoweredOn:
            log.info("BT powered on, scanning for %s…", self.target_name)
            central.scanForPeripheralsWithServices_options_(None, None)

    def centralManager_didDiscoverPeripheral_advertisementData_RSSI_(
        self, central, peripheral, ad_data, rssi
    ):
        name = peripheral.name()
        if name and name == self.target_name:
            log.info("Found %s (RSSI %s)", name, rssi)
            self.peripheral = peripheral
            central.stopScan()
            central.connectPeripheral_options_(peripheral, None)

    def centralManager_didConnectPeripheral_(self, central, peripheral):
        log.info("Connected to %s — opening L2CAP PSM 0x%04X",
                 peripheral.name(), self.psm)
        peripheral.setDelegate_(self)
        peripheral.openL2CAPChannel_(self.psm)

    def centralManager_didFailToConnectPeripheral_error_(self, central, peripheral, error):
        log.error("Connect failed: %s", error)

    def centralManager_didDisconnectPeripheral_error_(self, central, peripheral, error):
        log.warning("Disconnected: %s", error)
        self.connected_event.clear()
        self.channel = None
        self.output_stream = None
        self._stream_ready = False

    # -- CBPeripheralDelegate --

    def peripheral_didOpenL2CAPChannel_error_(self, peripheral, channel, error):
        if error:
            log.error("L2CAP open error: %s", error)
            return

        log.info("L2CAP channel opened (PSM 0x%04X)", self.psm)
        self.channel = channel

        # RX stream
        inp = channel.inputStream()
        inp.setDelegate_(self)
        inp.scheduleInRunLoop_forMode_(
            NSRunLoop.currentRunLoop(), NSDefaultRunLoopMode)
        inp.open()

        # TX stream
        out = channel.outputStream()
        out.setDelegate_(self)
        out.scheduleInRunLoop_forMode_(
            NSRunLoop.currentRunLoop(), NSDefaultRunLoopMode)
        out.open()
        self.output_stream = out

        self.parser = StreamingFrameParser()
        self.connected_event.set()

    # -- NSStreamDelegate --

    def stream_handleEvent_(self, stream, event):
        # NSStreamEventHasBytesAvailable = 2
        if event == 2:
            buf = bytearray(4096)
            while stream.hasBytesAvailable():
                result = stream.read_maxLength_(buf, len(buf))
                n = result[0] if isinstance(result, tuple) else result
                if n > 0:
                    for frame in self.parser.feed(bytes(buf[:n])):
                        if self._on_frame:
                            try:
                                self._on_frame(frame)
                            except Exception:
                                log.exception("on_frame callback error")

        # NSStreamEventHasSpaceAvailable = 4
        elif event == 4 and stream is self.output_stream:
            self._stream_ready = True
            self._flush_tx()

    def _flush_tx(self):
        with self._tx_lock:
            if not self._tx_pending or not self._stream_ready:
                return
            data = bytes(self._tx_pending)
            self._tx_pending.clear()

        if self.output_stream is None:
            return
        result = self.output_stream.write_maxLength_(data, len(data))
        n = result[0] if isinstance(result, tuple) else result
        if n < len(data):
            with self._tx_lock:
                self._tx_pending[:0] = data[n:]
            self._stream_ready = False

    def enqueue_tx(self, data: bytes):
        with self._tx_lock:
            self._tx_pending.extend(data)
        if self._stream_ready:
            self._flush_tx()


class BLETransport:
    """High-level BLE L2CAP CoC transport."""

    def __init__(self, target_name: str = "PSE84-Assistant",
                 psm: int = 0x0080,
                 on_frame: Optional[Callable[[Frame], None]] = None):
        self._target_name = target_name
        self._psm = psm
        self._on_frame = on_frame
        self._delegate: Optional[_Delegate] = None
        self._thread: Optional[threading.Thread] = None
        self._stop_event = threading.Event()

    def start(self):
        self._thread = threading.Thread(target=self._run, daemon=True,
                                        name="ble-transport")
        self._thread.start()

    def _run(self):
        self._delegate = _Delegate.alloc().init()
        self._delegate.target_name = self._target_name
        self._delegate.psm = self._psm
        self._delegate._on_frame = self._on_frame

        # Pump the NSRunLoop until stop is requested
        rl = NSRunLoop.currentRunLoop()
        while not self._stop_event.is_set():
            rl.runMode_beforeDate_(NSDefaultRunLoopMode,
                                  NSDate.dateWithTimeIntervalSinceNow_(0.05))

    def wait_connected(self, timeout: float = 30.0) -> bool:
        if self._delegate is None:
            return False
        return self._delegate.connected_event.wait(timeout)

    def send(self, data: bytes):
        if self._delegate:
            self._delegate.enqueue_tx(data)

    @property
    def is_connected(self) -> bool:
        return bool(self._delegate and self._delegate.connected_event.is_set())

    def stop(self):
        self._stop_event.set()
        if self._thread:
            self._thread.join(timeout=5)
