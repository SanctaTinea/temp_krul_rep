"""Serial transport worker and connection timing constants for Starset."""

from __future__ import annotations

import json
import queue
import threading

import serial
from krul_wire import (
    FORMAT_BSON,
    FORMAT_CBOR,
    FORMAT_JSON,
    FrameParser,
    decode_payload,
    encode_frame,
)
from PySide6.QtCore import QObject, QThread, Signal

PROTOCOL_VERSION = 4
DEFAULT_BAUDRATE = 115200
SERIAL_TIMEOUT = 0.1
RESPONSE_TIMEOUT_S = 4.0
DISCOVERY_TIMEOUT_S = 8.0
CONNECT_SETTLE_MS = 500
HEARTBEAT_INTERVAL_MS = 1000
HEARTBEAT_MISS_LIMIT = 3

# Client-only status. It is never serialized as a Krul protocol error code.
CLIENT_ERROR_TIMEOUT = -1

class SerialWorker(QThread):
    opened = Signal()
    open_failed = Signal(str)
    line_received = Signal(str)
    line_sent = Signal(str)
    bytes_received = Signal(int)
    bytes_sent = Signal(int)
    transport_error = Signal(str)
    frame_error = Signal(str)

    def __init__(self, port: str, baudrate: int, parent: QObject | None = None):
        super().__init__(parent)
        self.port = port
        self.baudrate = baudrate
        self._outgoing: queue.Queue[str] = queue.Queue()
        self._running = threading.Event()
        self._running.set()
        self._serial: serial.SerialBase | None = None
        self._wire_format = FORMAT_JSON

    def set_wire_format(self, wire_format: str) -> None:
        if self.isRunning():
            raise RuntimeError("wire format cannot change while connected")
        if wire_format not in (FORMAT_JSON, FORMAT_BSON, FORMAT_CBOR):
            raise ValueError(f"unsupported wire format: {wire_format}")
        self._wire_format = wire_format

    def send_line(self, line: str) -> None:
        if self._running.is_set():
            self._outgoing.put(line)

    def stop(self) -> None:
        self._running.clear()

    def run(self) -> None:
        try:
            link = serial.serial_for_url(
                self.port,
                baudrate=self.baudrate,
                timeout=SERIAL_TIMEOUT,
                write_timeout=1.0,
                xonxoff=False,
                rtscts=False,
                dsrdtr=False,
                do_not_open=True,
            )
            link.dtr = False
            link.rts = False
            link.open()
            link.dtr = False
            link.rts = False
            link.reset_input_buffer()
            self._serial = link
        except Exception as exc:
            self.open_failed.emit(str(exc))
            return

        self.opened.emit()
        parser = FrameParser()
        try:
            while self._running.is_set():
                try:
                    while True:
                        line = self._outgoing.get_nowait()
                        request = json.loads(line)
                        if not isinstance(request, dict):
                            raise ValueError("Krul request root must be an object")
                        encoded = encode_frame(request, self._wire_format)
                        written = link.write(encoded)
                        self.bytes_sent.emit(written)
                        if written != len(encoded):
                            raise serial.SerialTimeoutException(
                                "incomplete Krul frame write")
                        self.line_sent.emit(line)
                except queue.Empty:
                    pass
                except (ValueError, json.JSONDecodeError) as exc:
                    self.frame_error.emit(str(exc))
                raw = link.read(max(1, min(link.in_waiting, 4096)))
                if raw:
                    self.bytes_received.emit(len(raw))
                    frames, errors = parser.feed(raw)
                    for error in errors:
                        self.frame_error.emit(error)
                    for frame in frames:
                        try:
                            message = decode_payload(frame.payload,
                                                     frame.wire_format)
                            decoded = json.dumps(
                                message, ensure_ascii=False,
                                separators=(",", ":"))
                            self.line_received.emit(decoded)
                        except (UnicodeDecodeError, ValueError,
                                json.JSONDecodeError) as exc:
                            self.frame_error.emit(str(exc))
        except Exception as exc:
            if self._running.is_set():
                self.transport_error.emit(str(exc))
        finally:
            if self._serial is not None and self._serial.is_open:
                self._serial.close()
            self._serial = None
