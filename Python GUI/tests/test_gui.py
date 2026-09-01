from __future__ import annotations

import json
import threading
import time
from typing import Any

import pytest
from PySide6.QtCore import QObject, QTimer, Signal

import main as gui
from krul_simulator import PIN_NAMES, KrulSimulator, SimulatorServer
from krul_wire import FORMAT_BSON, FORMAT_CBOR, FORMAT_JSON


class FakeWorker(QObject):
    opened = Signal()
    open_failed = Signal(str)
    line_received = Signal(str)
    line_sent = Signal(str)
    transport_error = Signal(str)

    def __init__(self, port: str, baudrate: int, parent: QObject | None = None):
        super().__init__(parent)
        self.port = port
        self.baudrate = baudrate
        self.simulator = KrulSimulator()
        self.sent: list[dict[str, Any]] = []
        self.running = False

    def start(self) -> None:
        self.running = True
        QTimer.singleShot(0, self.opened.emit)

    def send_line(self, line: str) -> None:
        request = json.loads(line)
        self.sent.append(request)
        self.line_sent.emit(line)
        dispatched = self.simulator.dispatch(request)
        messages = [dispatched.response, *dispatched.events]
        for index, message in enumerate(messages):
            encoded = json.dumps(message, ensure_ascii=False, separators=(",", ":"))
            QTimer.singleShot(index, lambda value=encoded: self.line_received.emit(value))

    def stop(self) -> None:
        self.running = False

    def wait(self, _milliseconds: int) -> bool:
        return True


@pytest.mark.parametrize("wire_format", [FORMAT_JSON, FORMAT_BSON, FORMAT_CBOR])
def test_serial_worker_supports_socket_url(qtbot, wire_format: str) -> None:
    server = SimulatorServer(("127.0.0.1", 0), KrulSimulator())
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    worker = gui.SerialWorker(
        f"socket://127.0.0.1:{server.server_address[1]}", 115200
    )
    worker.set_wire_format(wire_format)
    try:
        with qtbot.waitSignal(worker.opened, timeout=2000):
            worker.start()
        with qtbot.waitSignal(worker.line_received, timeout=2000) as received:
            worker.send_line('{"cmd":"WHOAMI","id":1}')
        response = json.loads(received.args[0])
        assert response["result"]["device_name"] == "ARK-PC-SIM"
    finally:
        worker.stop()
        worker.wait(2000)
        server.shutdown()
        server.server_close()


def test_full_discovery_over_fake_transport(qtbot, monkeypatch) -> None:
    monkeypatch.setattr(gui, "CONNECT_SETTLE_MS", 0)
    created: list[FakeWorker] = []

    def factory(port: str, baudrate: int, parent: QObject) -> FakeWorker:
        worker = FakeWorker(port, baudrate, parent)
        created.append(worker)
        return worker

    window = gui.MainWindow(worker_factory=factory)
    qtbot.addWidget(window)
    window.port_combo.setCurrentText("fake://ark")
    window._toggle_connection()

    qtbot.waitUntil(lambda: window.io_panel is not None, timeout=3000)
    assert "ARK-PC-SIM" in window.device_label.text()
    assert "ECHO" in window.descriptors
    echo_form = next(form for form in window.forms if form.command == "ECHO")
    assert echo_form.title() == (
        "Echo — Type something; the virtual rabbit will echo it."
    )
    assert set(window.io_panel.cards) == set(PIN_NAMES)
    assert created[0].sent[0]["cmd"] == "WHOAMI"


def test_nogui_command_is_not_rendered(qtbot) -> None:
    window = gui.MainWindow()
    qtbot.addWidget(window)
    window.descriptors = {
        "VISIBLE": {"cmd": "VISIBLE", "title": "Visible"},
        "HIDDEN": {"cmd": "HIDDEN", "title": "Hidden", "nogui": True},
    }

    window._build_dynamic_tabs()

    assert {form.command for form in window.forms} == {"VISIBLE"}


def test_response_ids_events_and_timeouts(qtbot) -> None:
    window = gui.MainWindow()
    qtbot.addWidget(window)
    worker = FakeWorker("fake://ark", 115200, window)
    window.worker = worker
    callbacks: list[tuple[str, int]] = []

    first = window.send_request(
        "ECHO", {"text": "one"},
        callback=lambda message: callbacks.append(("first", message["id"])),
    )
    second = window.send_request(
        "ECHO", {"text": "two"},
        callback=lambda message: callbacks.append(("second", message["id"])),
    )
    assert first is not None and second is not None

    # Replace automatically scheduled responses with an explicit out-of-order pair.
    window.pending.clear()
    window.pending[first] = (
        "ECHO", lambda message: callbacks.append(("first", message["id"])),
        time.monotonic() + 1,
    )
    window.pending[second] = (
        "ECHO", lambda message: callbacks.append(("second", message["id"])),
        time.monotonic() + 1,
    )
    window._receive_line(json.dumps({"id": second, "success": True, "result": {"text": "two"}}))
    window._receive_line(json.dumps({"id": first, "success": True, "result": {"text": "one"}}))
    assert callbacks[-2:] == [("second", second), ("first", first)]

    window._receive_line(json.dumps({
        "event": "log", "data": {"severity": "warning", "message": "simulated warning"}
    }))
    assert "simulated warning" in window.terminal.toPlainText()

    expired: list[dict[str, Any]] = []
    window.pending[999] = ("DELAY", expired.append, time.monotonic() - 1)
    window._expire_requests()
    assert expired[0]["error"]["code"] == -1
