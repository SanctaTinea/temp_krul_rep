#!/usr/bin/env python3
"""Universal Krul JSON/BSON/CBOR control panel for compatible controllers."""

from __future__ import annotations

import json
import queue
import sys
import threading
import time
from collections import defaultdict
from typing import Any, Callable

import serial
import serial.tools.list_ports
from krul_wire import (
    FORMAT_BSON,
    FORMAT_CBOR,
    FORMAT_JSON,
    FrameParser,
    decode_payload,
    encode_frame,
)
from PySide6.QtCore import QEvent, QObject, QThread, QTimer, Qt, Signal
from PySide6.QtGui import QColor, QTextCharFormat, QTextCursor
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QComboBox,
    QDoubleSpinBox,
    QFormLayout,
    QFrame,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QPlainTextEdit,
    QPushButton,
    QScrollArea,
    QSizePolicy,
    QSpinBox,
    QSplitter,
    QStyle,
    QTabWidget,
    QToolButton,
    QVBoxLayout,
    QWidget,
)

PROTOCOL_VERSION = 3
DEFAULT_BAUDRATE = 115200
SERIAL_TIMEOUT = 0.1
RESPONSE_TIMEOUT_S = 4.0
DISCOVERY_TIMEOUT_S = 8.0
CONNECT_SETTLE_MS = 500


class SerialWorker(QThread):
    opened = Signal()
    open_failed = Signal(str)
    line_received = Signal(str)
    line_sent = Signal(str)
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


class Indicator(QWidget):
    def __init__(self) -> None:
        super().__init__()

        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(6)
        #layout.setStyleSheet(f"""border: 1px solid""")

        self.led = QLabel()
        self.led.setFixedSize(14, 14)

        self.text = QLabel()
        self.text.setMinimumWidth(35)

        layout.addWidget(self.led)
        layout.addWidget(self.text)

        self.set_state(False)

    def set_state(self, state: bool) -> None:
        color = "#22C55E" if state else "#EF4444"
        border = "#15803D" if state else "#B91C1C"

        self.led.setStyleSheet(f"""
            QLabel {{
                background-color: {color};
                border: 1px solid {border};
                border-radius: 7px;
            }}
        """)

        self.text.setText("HIGH" if state else "LOW")
        self.text.setStyleSheet(f"""
            QLabel {{
                color: {color};
                font-weight: 600;
                font-size: 11px;
            }}
        """)


class PinCard(QFrame):
    set_requested = Signal(str, int)

    def __init__(self, name: str, pin_type: str, state: int):
        super().__init__()
        self.name = name
        self.pin_type = pin_type
        self.setFrameShape(QFrame.StyledPanel)
        layout = QHBoxLayout(self)
        layout.setContentsMargins(10, 3, 5, 3)

        self.indicator = Indicator()
        layout.addWidget(self.indicator)

        label = QLabel(name)
        label.setTextInteractionFlags(Qt.TextSelectableByMouse)
        label.setStyleSheet(f"""
                            QLabel {{
                                font-weight: regular;
                                letter-spacing: 2px;
                            }}""")
        layout.addWidget(label, 1)

        if pin_type == "OUT":
            on_button = QPushButton("1")
            off_button = QPushButton("0")
            on_button.setFixedSize(40, 26)
            off_button.setFixedSize(40, 26)
            on_button.setStyleSheet("""
                                        QPushButton {
                                            background-color: #20C45A;
                                            color: white;
                                            font-weight: bold;
                                            border: none;
                                            border-radius: 2px;
                                        }
                                    
                                        QPushButton:hover {
                                            background-color: #28E06A;
                                        }
                                    
                                        QPushButton:pressed {
                                            background-color: #16A34A;
                                        }
                                    """)
            off_button.setStyleSheet("""
                                        QPushButton {
                                            background-color: #F04444;
                                            color: white;
                                            font-weight: bold;
                                            border: 0px solid;
                                            border-radius: 2px;
                                        }
                                    
                                        QPushButton:hover {
                                            background-color: #FF5555;
                                        }
                                    
                                        QPushButton:pressed {
                                            background-color: #D92D2D;
                                        }
                                    """)
            on_button.setCursor(Qt.PointingHandCursor)
            off_button.setCursor(Qt.PointingHandCursor)
            on_button.clicked.connect(lambda: self.set_requested.emit(self.name, 1))
            off_button.clicked.connect(lambda: self.set_requested.emit(self.name, 0))
            layout.addWidget(on_button)
            layout.addWidget(off_button)
        self.set_state(state)

    def set_state(self, state: int) -> None:
        self.indicator.set_state(bool(state))

class ResultIntLabel(QLabel):
    def __init__(self):
        super().__init__("—")
        self.setAlignment(Qt.AlignRight | Qt.AlignVCenter)
        self.setTextInteractionFlags(Qt.TextSelectableByMouse)
        self.setMinimumWidth(80)

        font = self.font()
        font.setFamily("Consolas")
        self.setFont(font)

        self.setSizePolicy(
            QSizePolicy.Maximum,
            QSizePolicy.Preferred
        )

        self.setStyleSheet("""
            QLabel {
                margin-left: 100px;
                padding: 3px 8px;
                font-weight: 600;
            }
        """)

class ResultBoolLabel(QLabel):
    def __init__(self):
        super().__init__("—")

        self.setAlignment(Qt.AlignCenter)
        self.setMinimumWidth(55)

    def setValue(self, value: bool):
        if value:
            self.setText("ON")
            self.setStyleSheet("""
                QLabel {
                    padding: 3px 8px;
                    border-radius: 5px;
                    background: #d8f5df;
                    color: #16752d;
                    font-weight: 600;
                }
            """)
        else:
            self.setText("OFF")
            self.setStyleSheet("""
                QLabel {
                    padding: 3px 8px;
                    border-radius: 5px;
                    background: #f5dddd;
                    color: #8b2020;
                    font-weight: 600;
                }
            """)

class ResponsivePinGrid(QWidget):
    def __init__(self):
        super().__init__()
        self.cards: list[PinCard] = []
        self.grid = QGridLayout(self)
        self.grid.setAlignment(Qt.AlignTop | Qt.AlignLeft)
        self._columns = 0
        self._filter_text = ""

    def add_card(self, card: PinCard) -> None:
        self.cards.append(card)
        self._relayout(force=True)

    def resizeEvent(self, event) -> None:  # noqa: N802 - Qt API
        super().resizeEvent(event)
        self._relayout()

    def _relayout(self, force: bool = False) -> None:
        columns = max(1, self.width() // 260)
        if columns == self._columns and not force:
            return
        self._columns = columns
        for card in self.cards:
            self.grid.removeWidget(card)
        visible = [
            card for card in self.cards
            if not self._filter_text or self._filter_text in card.name.casefold()
        ]
        for index, card in enumerate(visible):
            self.grid.addWidget(card, index // columns, index % columns)

    def apply_filter(self, text: str) -> None:
        self._filter_text = text.casefold().strip()
        for card in self.cards:
            card.setVisible(
                not self._filter_text or self._filter_text in card.name.casefold()
            )
        self._relayout(force=True)


class IOPanel(QWidget):
    def __init__(self, pins: list[dict[str, Any]], sender: Callable,
                 target: str | None = None):
        super().__init__()
        self.sender = sender
        self.target = target
        self.cards: dict[str, PinCard] = {}
        self._in_flight = False
        outer = QVBoxLayout(self)

        filter_row = QHBoxLayout()
        filter_row.addWidget(QLabel("Фильтр:"))
        self.filter_edit = QLineEdit()
        self.filter_edit.setPlaceholderText("Имя пина")
        filter_row.addWidget(self.filter_edit, 1)
        outer.addLayout(filter_row)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        body = QWidget()
        body_layout = QVBoxLayout(body)
        self.input_grid = ResponsivePinGrid()
        self.output_grid = ResponsivePinGrid()

        inputs = QGroupBox("Входы")
        input_layout = QVBoxLayout(inputs)
        read_button = QPushButton("Прочитать")
        read_button.clicked.connect(self.poll_inputs)
        input_layout.addWidget(read_button, 0, Qt.AlignLeft)
        input_layout.addWidget(self.input_grid)

        outputs = QGroupBox("Выходы")
        output_layout = QVBoxLayout(outputs)
        all_row = QHBoxLayout()
        on_all = QPushButton("Активировать все")
        off_all = QPushButton("Деактивировать все")
        on_all.clicked.connect(lambda: self._set_all(1))
        off_all.clicked.connect(lambda: self._set_all(0))
        read_outputs = QPushButton("Прочитать")
        read_outputs.clicked.connect(self.poll_outputs)
        all_row.addWidget(read_outputs)
        all_row.addWidget(on_all)
        all_row.addWidget(off_all)
        all_row.addStretch()
        output_layout.addLayout(all_row)
        output_layout.addWidget(self.output_grid)

        body_layout.addWidget(inputs)
        body_layout.addWidget(outputs)
        body_layout.addStretch()
        scroll.setWidget(body)
        outer.addWidget(scroll)

        for pin in sorted(pins, key=lambda value: str(value.get("name", "")).casefold()):
            name = str(pin.get("name", ""))
            pin_type = str(pin.get("direction", pin.get("type", "IN"))).upper()
            if not name or pin_type not in {"IN", "OUT"}:
                continue
            card = PinCard(name, pin_type, int(pin.get("state", 0)))
            card.set_requested.connect(self._set_one)
            self.cards[name] = card
            (self.output_grid if pin_type == "OUT" else self.input_grid).add_card(card)
        self.filter_edit.textChanged.connect(self._filter)

    def _filter(self, text: str) -> None:
        self.input_grid.apply_filter(text)
        self.output_grid.apply_filter(text)

    def _set_one(self, name: str, state: int) -> None:
        if self.target is None:
            params = {"pins": [{"name": name, "state": state}]}
        else:
            params = {"target": self.target, "name": name, "state": str(state)}
        self.sender("PIN_SET", params, self._updated)

    def _set_all(self, state: int) -> None:
        if not any(card.pin_type == "OUT" for card in self.cards.values()):
            return
        if self.target is None:
            params = {"pins": [{"name": "ALL", "state": state}]}
        else:
            params = {"target": self.target, "name": "ALL", "state": str(state)}
        self.sender("PIN_SET", params, self._updated)

    def poll_inputs(self) -> None:
        if self._in_flight:
            return
        self._in_flight = True
        if self.target is None:
            names = [card.name for card in self.cards.values() if card.pin_type == "IN"]
            params = {"pins": names}
        else:
            params = {"target": self.target, "name": "IN"}
        self.sender("PIN_GET", params, self._updated)

    def poll_outputs(self) -> None:
        if self._in_flight or not any(
                card.pin_type == "OUT" for card in self.cards.values()):
            return
        self._in_flight = True
        if self.target is None:
            names = [
                card.name for card in self.cards.values()
                if card.pin_type == "OUT"
            ]
            params = {"pins": names}
        else:
            params = {"target": self.target, "name": "OUT"}
        self.sender("PIN_GET", params, self._updated)

    def _updated(self, message: dict[str, Any]) -> None:
        self._in_flight = False
        if not message.get("success"):
            return
        result = message.get("result", {})
        pins = result.get("pins", [])
        if not pins and result.get("name"):
            pins = [result]
        for pin in pins:
            if str(pin.get("name", "")).upper() == "ALL" and "state" in pin:
                for card in self.cards.values():
                    if card.pin_type == "OUT":
                        card.set_state(int(pin["state"]))
                continue
            card = self.cards.get(str(pin.get("name", "")))
            if card is not None:
                card.set_state(int(pin.get("state", 0)))


class CommandForm(QGroupBox):
    def __init__(self, descriptor: dict[str, Any], sender: Callable):
        title = str(descriptor.get("title") or descriptor.get("cmd"))
        description = descriptor.get("description")
        if isinstance(description, str) and description:
            title = f"{title} — {description}"
        super().__init__(title)
        self.descriptor = descriptor
        self.command = str(descriptor["cmd"])
        self.sender = sender
        self.param_widgets: dict[str, QWidget] = {}
        self.result_widgets: dict[str, QWidget] = {}
        self.in_flight = False
        self.last_poll = 0.0
        self.global_autopoll_enabled = False

        layout = QVBoxLayout(self)
        form = QFormLayout()
        for field in descriptor.get("params", []):
            widget = self._make_param_widget(field)
            self.param_widgets[str(field["name"])] = widget
            form.addRow(str(field.get("label") or field["name"]), widget)
        layout.addLayout(form)

        result_fields = descriptor.get("result", [])
        if result_fields:
            line = QFrame()
            line.setFrameShape(QFrame.HLine)
            layout.addWidget(line)
            results = QFormLayout()
            for field in result_fields:
                #print(
                #    "RESULT FIELD:",
                #    field.get("name"),
                #    "type =", repr(field.get("type"))
                #)
                if field.get("type") == "console_string":
                    continue
                if field.get("type") == "string":
                    output = QLabel()
                    output.setTextInteractionFlags(Qt.TextSelectableByMouse)
                    output.setWordWrap(True)
                    output.setAutoFillBackground(False)
                    output.setStyleSheet(
                        "QLabel { border: none; background: transparent; }"
                    )
                elif field.get("type") == "integer":
                    output = ResultIntLabel()
                    output.setTextInteractionFlags(Qt.TextSelectableByMouse)
                    output.setWordWrap(True)
                    output.setAutoFillBackground(False)
                    output.setStyleSheet(
                        "QLabel { border: none; background: transparent; }"
                    )
                elif field.get("type") == "boolean":
                    output = ResultBoolLabel()
                else:
                    output = QLineEdit()
                    output.setReadOnly(True)
                self.result_widgets[str(field["name"])] = output
                results.addRow(str(field.get("label") or field["name"]), output)
            layout.addLayout(results)

        row = QHBoxLayout()
        self.execute_button = QPushButton("Выполнить")
        self.execute_button.clicked.connect(self.execute)
        row.addWidget(self.execute_button)
        if descriptor.get("autoupdate"):
            self.auto_enabled = QCheckBox("Авто")
            self.auto_enabled.setChecked(True)
            self.auto_enabled.toggled.connect(
                lambda _checked: self._update_execute_button()
            )
            row.addWidget(self.auto_enabled)
        else:
            self.auto_enabled = None
        row.addStretch()
        layout.addLayout(row)

    def set_global_autopoll(self, enabled: bool) -> None:
        self.global_autopoll_enabled = enabled
        self._update_execute_button()

    def _update_execute_button(self) -> None:
        auto_active = (
            self.global_autopoll_enabled
            and self.auto_enabled is not None
            and self.auto_enabled.isChecked()
        )
        self.execute_button.setEnabled(not self.in_flight and not auto_active)

    @staticmethod
    def _make_param_widget(field: dict[str, Any]) -> QWidget:
        field_type = field.get("type")
        default = field.get("default")
        constraints = field.get("constraints", {})
        if field_type == "integer":
            widget = QSpinBox()
            widget.setRange(int(constraints.get("minimum", -2147483648)),
                            int(constraints.get("maximum", 2147483647)))
            widget.setValue(int(default if default is not None else widget.minimum()))
            return widget
        if field_type == "float":
            widget = QDoubleSpinBox()
            widget.setDecimals(6)
            widget.setRange(float(constraints.get("minimum", -1e9)),
                            float(constraints.get("maximum", 1e9)))
            widget.setSingleStep(float(constraints.get("step", 0.1)))
            widget.setValue(float(default if default is not None else widget.minimum()))
            return widget
        if field_type == "boolean":
            widget = QCheckBox()
            widget.setChecked(bool(default))
            return widget
        if field_type == "enum":
            widget = QComboBox()
            selected = 0
            for index, value in enumerate(constraints.get("values", [])):
                widget.addItem(str(value.get("title", value.get("value", ""))),
                               value.get("value"))
                if value.get("value") == default:
                    selected = index
            widget.setCurrentIndex(selected)
            return widget
        widget = QLineEdit(str(default or ""))
        widget.setMaxLength(int(constraints.get("maxLength", 32767)))
        return widget

    def parameters(self) -> dict[str, Any]:
        values: dict[str, Any] = {}
        fields = {str(field["name"]): field for field in self.descriptor.get("params", [])}
        for name, widget in self.param_widgets.items():
            field = fields[name]
            if isinstance(widget, QSpinBox):
                values[name] = widget.value()
            elif isinstance(widget, QDoubleSpinBox):
                values[name] = widget.value()
            elif isinstance(widget, QCheckBox):
                values[name] = widget.isChecked()
            elif isinstance(widget, QComboBox):
                values[name] = widget.currentData()
            elif isinstance(widget, QLineEdit):
                value = widget.text()
                constraints = field.get("constraints", {})
                minimum = int(constraints.get("minLength", 0))
                if len(value.encode("utf-8")) < minimum:
                    raise ValueError(f"Поле «{field.get('label', name)}» слишком короткое")
                values[name] = value
        return values

    def execute(self) -> None:
        try:
            params = self.parameters()
        except ValueError as exc:
            QMessageBox.warning(self, "Параметры", str(exc))
            return
        self.in_flight = True
        self._update_execute_button()
        self.sender(self.command, params, self.handle_response)

    def handle_response(self, message: dict[str, Any]) -> None:
        self.in_flight = False
        self._update_execute_button()
        if not message.get("success"):
            return
        result = message.get("result", {})
        for name, widget in self.result_widgets.items():
            value = result.get(name, "")
            
            if isinstance(widget, ResultBoolLabel) :
                widget.setValue(value)
                continue
            
            widget.setText(
                json.dumps(value, ensure_ascii=False)
                if isinstance(value, (dict, list))
                else str(value)
            )

    def poll(self, requested_period_ms: int) -> None:
        if self.auto_enabled is None or not self.auto_enabled.isChecked() or self.in_flight:
            return
        auto = self.descriptor.get("autoupdate", {})
        period_ms = max(int(auto.get("min_period", requested_period_ms)), requested_period_ms)
        maximum = int(auto.get("max_period", period_ms))
        period_ms = min(period_ms, maximum)
        now = time.monotonic()
        if now - self.last_poll < period_ms / 1000.0:
            return
        self.last_poll = now
        self.execute()


class MainWindow(QMainWindow):
    def __init__(self, worker_factory: Callable[..., SerialWorker] = SerialWorker):
        super().__init__()
        self.setWindowTitle("АРК — Krul Control Panel")
        self.resize(1450, 850)
        self.worker: SerialWorker | None = None
        self._worker_factory = worker_factory
        self.next_id = 1
        self.pending: dict[int, tuple[str, Callable | None, float]] = {}
        self.descriptors: dict[str, dict[str, Any]] = {}
        self.forms: list[CommandForm] = []
        self.io_panel: IOPanel | None = None
        self.io_panels: list[IOPanel] = []
        self.raw_history: list[str] = []
        self.raw_history_index = 0
        self._build_ui()
        self._refresh_ports()

        self.timeout_timer = QTimer(self)
        self.timeout_timer.timeout.connect(self._expire_requests)
        self.timeout_timer.start(250)
        self.poll_timer = QTimer(self)
        self.poll_timer.timeout.connect(self._poll)
        self.poll_timer.start(50)

    def _build_ui(self) -> None:
        central = QWidget()
        root = QVBoxLayout(central)
        root.setContentsMargins(6, 6, 6, 6)

        connection = QHBoxLayout()
        connection.addWidget(QLabel("Порт:"))
        self.port_combo = QComboBox()
        self.port_combo.setEditable(True)
        self.port_combo.setMinimumWidth(260)
        self.port_combo.lineEdit().setPlaceholderText(
            "COM3 или socket://127.0.0.1:7000"
        )
        connection.addWidget(self.port_combo)
        refresh = QToolButton()
        refresh.setIcon(self.style().standardIcon(QStyle.SP_BrowserReload))
        refresh.setToolTip("Обновить список портов")
        refresh.clicked.connect(self._refresh_ports)
        connection.addWidget(refresh)
        connection.addWidget(QLabel("Скорость:"))
        self.baud_combo = QComboBox()
        self.baud_combo.setEditable(True)
        for baud in (9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600):
            self.baud_combo.addItem(str(baud))
        self.baud_combo.setCurrentText(str(DEFAULT_BAUDRATE))
        connection.addWidget(self.baud_combo)
        connection.addWidget(QLabel("Codec:"))
        self.format_combo = QComboBox()
        self.format_combo.addItem("JSON", FORMAT_JSON)
        self.format_combo.addItem("BSON", FORMAT_BSON)
        self.format_combo.addItem("CBOR", FORMAT_CBOR)
        connection.addWidget(self.format_combo)
        self.connect_button = QPushButton("Подключить")
        self.connect_button.clicked.connect(self._toggle_connection)
        connection.addWidget(self.connect_button)
        self.rebuild_button = QPushButton("Переформировать GUI")
        self.rebuild_button.setEnabled(False)
        self.rebuild_button.clicked.connect(self._discover)
        connection.addWidget(self.rebuild_button)
        self.device_label = QLabel("МК: —")
        connection.addWidget(self.device_label, 1)
        root.addLayout(connection)

        splitter = QSplitter(Qt.Horizontal)
        self.tabs = QTabWidget()
        splitter.addWidget(self.tabs)
        splitter.addWidget(self._build_terminal())
        splitter.setSizes([900, 550])
        root.addWidget(splitter, 1)
        self.setCentralWidget(central)
        self._reset_tabs()

    def _build_terminal(self) -> QWidget:
        panel = QWidget()
        layout = QVBoxLayout(panel)
        header = QHBoxLayout()
        title = QLabel("Терминал")
        title.setStyleSheet("font-weight:bold")
        header.addWidget(title)
        header.addStretch()
        clear = QToolButton()
        clear.setIcon(self.style().standardIcon(QStyle.SP_DialogResetButton))
        clear.setToolTip("Очистить")
        clear.clicked.connect(lambda: self.terminal.clear())
        header.addWidget(clear)
        self.developer_check = QCheckBox("Режим разработчика")
        self.developer_check.toggled.connect(self._toggle_developer)
        header.addWidget(self.developer_check)
        layout.addLayout(header)

        self.terminal = QPlainTextEdit()
        self.terminal.setReadOnly(True)
        self.terminal.setMaximumBlockCount(5000)
        layout.addWidget(self.terminal, 1)

        self.raw_row = QWidget()
        raw_layout = QHBoxLayout(self.raw_row)
        raw_layout.setContentsMargins(0, 0, 0, 0)
        self.raw_edit = QLineEdit()
        self.raw_edit.setPlaceholderText('{"cmd":"WHOAMI","id":1}')
        self.raw_edit.installEventFilter(self)
        self.raw_edit.returnPressed.connect(self._send_raw)
        raw_layout.addWidget(self.raw_edit, 1)
        send = QPushButton("Отправить")
        send.clicked.connect(self._send_raw)
        raw_layout.addWidget(send)
        self.raw_row.hide()
        layout.addWidget(self.raw_row)

        controls = QHBoxLayout()
        self.autoscroll_check = QCheckBox("Автопрокрутка")
        self.autoscroll_check.setChecked(True)
        controls.addWidget(self.autoscroll_check)
        self.autopoll_check = QCheckBox("Автоопрос")
        self.autopoll_check.toggled.connect(self._autopoll_toggled)
        controls.addWidget(self.autopoll_check)
        self.poll_period_combo = QComboBox()
        for period in (100, 250, 500, 1000, 2000, 5000):
            self.poll_period_combo.addItem(str(period), period)
        self.poll_period_combo.setCurrentText("500")
        controls.addWidget(self.poll_period_combo)
        controls.addWidget(QLabel("мс"))
        controls.addStretch()
        layout.addLayout(controls)
        return panel

    def eventFilter(self, watched: QObject, event: QEvent) -> bool:  # noqa: N802
        if watched is self.raw_edit and event.type() == QEvent.KeyPress:
            if event.key() == Qt.Key_Up and self.raw_history:
                self.raw_history_index = max(0, self.raw_history_index - 1)
                self.raw_edit.setText(self.raw_history[self.raw_history_index])
                return True
            if event.key() == Qt.Key_Down and self.raw_history:
                self.raw_history_index = min(len(self.raw_history), self.raw_history_index + 1)
                self.raw_edit.setText(
                    "" if self.raw_history_index == len(self.raw_history)
                    else self.raw_history[self.raw_history_index]
                )
                return True
        return super().eventFilter(watched, event)

    def _refresh_ports(self) -> None:
        selected = self.port_combo.currentText()
        ports = sorted((item.device for item in serial.tools.list_ports.comports()),
                       key=str.casefold)
        self.port_combo.clear()
        self.port_combo.addItems(ports)
        if selected and selected not in ports:
            self.port_combo.addItem(selected)
        if selected:
            self.port_combo.setCurrentText(selected)

    def _toggle_connection(self) -> None:
        if self.worker is not None:
            self._disconnect()
            return
        port = self.port_combo.currentText().strip()
        try:
            baudrate = int(self.baud_combo.currentText())
            if not port or baudrate <= 0:
                raise ValueError
        except ValueError:
            QMessageBox.warning(self, "Подключение", "Выберите порт и корректную скорость.")
            return
        self.worker = self._worker_factory(port, baudrate, self)
        if hasattr(self.worker, "set_wire_format"):
            self.worker.set_wire_format(self.format_combo.currentData())
        self.worker.opened.connect(self._serial_opened)
        self.worker.open_failed.connect(self._open_failed)
        self.worker.transport_error.connect(self._transport_failed)
        self.worker.line_received.connect(self._receive_line)
        self.worker.line_sent.connect(self._sent_line)
        if hasattr(self.worker, "frame_error"):
            self.worker.frame_error.connect(
                lambda error: self._append_terminal(
                    f"Krul transport frame: {error}", "warning"))
        self.connect_button.setText("Подключение…")
        self.connect_button.setEnabled(False)
        self.format_combo.setEnabled(False)
        self.worker.start()

    def _serial_opened(self) -> None:
        self.connect_button.setText("Отключить")
        self.connect_button.setEnabled(True)
        self.rebuild_button.setEnabled(True)
        self._append_terminal("Последовательный порт открыт", "info")
        QTimer.singleShot(CONNECT_SETTLE_MS, self._discover)

    def _open_failed(self, error: str) -> None:
        QMessageBox.critical(self, "Ошибка порта", error)
        self._disconnect(from_worker=True)

    def _transport_failed(self, error: str) -> None:
        QMessageBox.critical(self, "Ошибка связи", error)
        self._disconnect(from_worker=True)

    def _disconnect(self, from_worker: bool = False) -> None:
        worker = self.worker
        self.worker = None
        if worker is not None:
            worker.stop()
            if not from_worker:
                worker.wait(1000)
            worker.deleteLater()
        self.pending.clear()
        self.descriptors.clear()
        self.forms.clear()
        self.io_panel = None
        self.io_panels.clear()
        self.device_label.setText("МК: —")
        self.connect_button.setText("Подключить")
        self.connect_button.setEnabled(True)
        self.rebuild_button.setEnabled(False)
        self.format_combo.setEnabled(True)
        self._reset_tabs()

    def closeEvent(self, event) -> None:  # noqa: N802
        self._disconnect()
        event.accept()

    def _allocate_id(self) -> int:
        transaction = self.next_id
        self.next_id = 1 if transaction == 0xFFFFFFFF else transaction + 1
        return transaction

    def send_request(self, command: str, params: dict[str, Any] | None = None,
                     callback: Callable | None = None,
                     timeout: float | None = None) -> int | None:
        if self.worker is None:
            return None
        if timeout is None:
            declared_ms = int(self.descriptors.get(command, {}).get("timeout_ms", 0))
            timeout = max(RESPONSE_TIMEOUT_S, declared_ms / 1000.0 + 1.0)
        transaction = self._allocate_id()
        request: dict[str, Any] = {"cmd": command, "id": transaction}
        if params is not None:
            request["params"] = params
        line = json.dumps(request, ensure_ascii=False, separators=(",", ":"))
        self.pending[transaction] = (command, callback, time.monotonic() + timeout)
        self.worker.send_line(line)
        return transaction

    def _sent_line(self, line: str) -> None:
        if self.developer_check.isChecked():
            self._append_terminal(f"TX > {line}", "debug")

    def _receive_line(self, line: str) -> None:
        if self.developer_check.isChecked():
            self._append_terminal(f"RX < {line}", "debug")
        try:
            message = json.loads(line)
        except json.JSONDecodeError:
            if not self.developer_check.isChecked():
                self._append_terminal(f"Некорректный JSON от МК: {line}", "warning")
            return
        if not isinstance(message, dict):
            self._append_terminal("Корневое значение ответа не является object", "warning")
            return
        if "event" in message:
            if message.get("event") == "log":
                data = message.get("data", {})
                if isinstance(data, dict):
                    self._append_terminal(
                        str(data.get("message", "")),
                        str(data.get("severity", "info")),
                    )
                    return
            self._append_terminal(
                f"Событие {message.get('event')}: "
                f"{json.dumps(message.get('data', {}), ensure_ascii=False)}",
                "warning",
            )
            return
        transaction = message.get("id")
        pending = self.pending.pop(transaction, None) if isinstance(transaction, int) else None
        if not message.get("success", False):
            error = message.get("error", {})
            self._append_terminal(
                f"Ошибка {error.get('code', '?')}: {error.get('message', 'Без описания')}",
                "error",
            )
        else:
            command = pending[0] if pending else None
            descriptor = self.descriptors.get(command or "", {})
            result = message.get("result", {})
            for field in descriptor.get("result", []):
                if field.get("type") == "console_string" and field.get("name") in result:
                    constraints = field.get("constraints", {})
                    self._append_terminal(str(result[field["name"]]),
                                          str(constraints.get("severity", "info")))
        if pending and pending[1]:
            pending[1](message)

    def _expire_requests(self) -> None:
        now = time.monotonic()
        expired = [transaction for transaction, (_, _, deadline) in self.pending.items()
                   if deadline <= now]
        for transaction in expired:
            command, callback, _ = self.pending.pop(transaction)
            message = {
                "id": transaction,
                "success": False,
                "error": {"code": -1, "message": f"Тайм-аут команды {command}"},
            }
            self._append_terminal(message["error"]["message"], "error")
            if callback:
                callback(message)

    def _discover(self) -> None:
        if self.worker is None:
            return
        self.pending.clear()
        self.descriptors.clear()
        self.forms.clear()
        self.io_panel = None
        self.io_panels.clear()
        self._reset_tabs()
        self.device_label.setText("МК: опрос…")
        self.send_request("WHOAMI", callback=self._whoami_received,
                          timeout=DISCOVERY_TIMEOUT_S)

    def _whoami_received(self, message: dict[str, Any]) -> None:
        if not message.get("success"):
            self.device_label.setText("МК: ошибка WHOAMI")
            return
        result = message.get("result", {})
        version = result.get("protocol_version")
        if version != PROTOCOL_VERSION:
            QMessageBox.critical(
                self, "Версия протокола",
                f"GUI поддерживает версию {PROTOCOL_VERSION}, МК сообщил {version}.",
            )
            return
        self.device_label.setText(
            f"МК: {result.get('device_name', '?')} · FW {result.get('firmware', '?')} · JSON v{version}"
        )
        self.send_request("CMD_LIST", callback=self._command_list_received,
                          timeout=DISCOVERY_TIMEOUT_S)

    def _command_list_received(self, message: dict[str, Any]) -> None:
        if not message.get("success"):
            return
        names = [str(name) for name in message.get("result", {}).get("cmd_name", [])]
        if not names:
            self._append_terminal("CMD_LIST вернул пустой список", "error")
            return
        waiting = set(names)

        def described(response: dict[str, Any], expected: str) -> None:
            waiting.discard(expected)
            if response.get("success"):
                descriptor = response.get("result", {})
                if isinstance(descriptor, dict) and descriptor.get("cmd"):
                    self.descriptors[str(descriptor["cmd"])] = descriptor
            if not waiting:
                self._build_dynamic_tabs()
                self._discover_io()

        for name in names:
            self.send_request(
                "DESCRIBE", {"name": name},
                lambda response, expected=name: described(response, expected),
                timeout=DISCOVERY_TIMEOUT_S,
            )

    def _reset_tabs(self) -> None:
        while self.tabs.count():
            widget = self.tabs.widget(0)
            self.tabs.removeTab(0)
            widget.deleteLater()
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        welcome = QWidget()
        layout = QVBoxLayout(welcome)
        rabbit = QLabel("🧛🐇  Кролик вампир")
        rabbit.setAlignment(Qt.AlignCenter)
        rabbit.setStyleSheet("font-size:28px; color:#6d3aa8")
        layout.addWidget(rabbit)
        layout.addWidget(QLabel(
            "Подключитесь к контроллеру — остальные вкладки будут построены "
            "по его DESCRIBE-метаданным."), 0, Qt.AlignCenter)
        self.default_commands_layout = QVBoxLayout()
        layout.addLayout(self.default_commands_layout)
        layout.addStretch()
        scroll.setWidget(welcome)
        self.tabs.addTab(scroll, "Кролик вампир")

    def _populate_command_groups(
        self, body_layout: QVBoxLayout, descriptors: list[dict[str, Any]]
    ) -> None:
        groups: dict[str, tuple[QGroupBox, QVBoxLayout]] = {}
        for descriptor in sorted(
            descriptors,
            key=lambda item: (int(item.get("order", 0)), str(item.get("cmd", ""))),
        ):
            group_name = str(descriptor.get("group") or "Команды")
            if group_name not in groups:
                box = QGroupBox(group_name)
                box.setObjectName("commandGroup")
                box.setStyleSheet("""
                    QGroupBox#commandGroup {
                        background-color: #FFFFFF;
                        border: 1px solid #D5DCE5;
                        border-radius: 2px;
                        margin-top: 12px;
                        padding: 8px;
                    }

                    QGroupBox::title#commandGroup {
                        subcontrol-origin: margin;
                        left: 12px;
                        padding: 0 6px;
                        letter-spacing: 2px;
                        color: #FFFFFF;
                        font-weight: 600;
                        background-color: #cf2e2e;
                    }
                """)
                box_layout = QVBoxLayout(box)
                groups[group_name] = (box, box_layout)
                body_layout.addWidget(box)
            form = CommandForm(descriptor, self.send_request)
            form.set_global_autopoll(self.autopoll_check.isChecked())
            groups[group_name][1].addWidget(form)
            self.forms.append(form)

    def _build_dynamic_tabs(self) -> None:
        normal = [descriptor for descriptor in self.descriptors.values()
                  if not descriptor.get("builtin")
                  and not descriptor.get("nogui")]
        if self._has_targeted_io():
            normal = [descriptor for descriptor in normal
                      if descriptor.get("cmd") not in {"PIN_LIST", "PIN_GET", "PIN_SET"}]
        by_tab: dict[str, list[dict[str, Any]]] = defaultdict(list)
        for descriptor in normal:
            by_tab[str(descriptor.get("tab") or "Кролик вампир")].append(descriptor)

        self._populate_command_groups(
            self.default_commands_layout, by_tab.pop("Кролик вампир", [])
        )

        for tab_name, descriptors in by_tab.items():
            scroll = QScrollArea()
            scroll.setWidgetResizable(True)
            body = QWidget()
            body_layout = QVBoxLayout(body)
            self._populate_command_groups(body_layout, descriptors)
            body_layout.addStretch()
            scroll.setWidget(body)
            self.tabs.addTab(scroll, tab_name)

    def _pins_received(self, message: dict[str, Any]) -> None:
        if not message.get("success"):
            return
        pins = message.get("result", {}).get("pins", [])
        if not isinstance(pins, list):
            return
        self.io_panel = IOPanel(pins, self.send_request)
        self.io_panels.append(self.io_panel)
        self.tabs.insertTab(1, self.io_panel, "IO")

    def _has_targeted_io(self) -> bool:
        descriptor = self.descriptors.get("PIN_GET", {})
        names = {str(field.get("name")) for field in descriptor.get("params", [])}
        return {"target", "name"}.issubset(names)

    def _discover_io(self) -> None:
        descriptor = self.descriptors.get("PIN_GET", {})
        if descriptor.get("nogui"):
            return
        names = {str(field.get("name")) for field in descriptor.get("params", [])}
        if "pins" in names:
            self.send_request("PIN_GET", {"pins": []}, self._pins_received,
                              timeout=DISCOVERY_TIMEOUT_S)
        elif self._has_targeted_io():
            self.send_request("TARGET_LIST", callback=self._targets_received,
                              timeout=DISCOVERY_TIMEOUT_S)

    def _targets_received(self, message: dict[str, Any]) -> None:
        if not message.get("success"):
            return
        targets = message.get("result", {}).get("targets", [])
        pending_targets = [
            str(target_info.get("name", ""))
            for target_info in (targets if isinstance(targets, list) else [])
            if target_info.get("available", False)
            and str(target_info.get("name", ""))
        ]

        # Discovery responses can be several kilobytes. Read one controller at
        # a time so UART1 never receives the next request while a large response
        # is still being placed into the CVM DMA transmit ring.
        def request_next() -> None:
            if not pending_targets:
                return
            target = pending_targets.pop(0)

            def received(response: dict[str, Any]) -> None:
                self._target_pins_received(target, response)
                request_next()

            self.send_request(
                "PIN_GET",
                {"target": target, "name": "ALL"},
                received,
            )

        request_next()

    def _target_pins_received(self, target: str,
                              message: dict[str, Any]) -> None:
        if not message.get("success"):
            return
        pins = message.get("result", {}).get("pins", [])
        if not isinstance(pins, list):
            return
        panel = IOPanel(pins, self.send_request, target)
        self.io_panels.append(panel)
        self.tabs.addTab(panel, f"IO {target}")

    def _toggle_developer(self, enabled: bool) -> None:
        self.raw_row.setVisible(enabled)

    def _send_raw(self) -> None:
        if self.worker is None:
            return
        line = self.raw_edit.text().strip()
        if not line:
            return
        try:
            request = json.loads(line)
            if not isinstance(request, dict):
                raise ValueError("Корень должен быть object")
            if "id" not in request:
                request["id"] = self._allocate_id()
            transaction = request["id"]
            if not isinstance(transaction, int) or not 1 <= transaction <= 0xFFFFFFFF:
                raise ValueError("id должен быть uint32 и не равен нулю")
            line = json.dumps(request, ensure_ascii=False, separators=(",", ":"))
        except (json.JSONDecodeError, ValueError) as exc:
            QMessageBox.warning(self, "JSON", str(exc))
            return
        self.raw_history.append(line)
        self.raw_history_index = len(self.raw_history)
        self.raw_edit.clear()
        self.worker.send_line(line)

    def _poll(self) -> None:
        if not self.autopoll_check.isChecked() or self.worker is None:
            return
        period = int(self.poll_period_combo.currentData())
        for form in self.forms:
            form.poll(period)
        if self.io_panels:
            now_slot = int(time.monotonic() * 1000) // period
            if getattr(self, "_io_poll_slot", None) != now_slot:
                self._io_poll_slot = now_slot
                for panel in self.io_panels:
                    panel.poll_inputs()

    def _autopoll_toggled(self, enabled: bool) -> None:
        for form in self.forms:
            form.set_global_autopoll(enabled)

    def _append_terminal(self, text: str, severity: str = "info") -> None:
        colors = {
            "debug": QColor("#7d8790"),
            "info": QColor("#1f6feb"),
            "warning": QColor("#b7791f"),
            "error": QColor("#c53030"),
        }
        cursor = self.terminal.textCursor()
        cursor.movePosition(QTextCursor.End)
        fmt = QTextCharFormat()
        fmt.setForeground(colors.get(severity, colors["info"]))
        timestamp = time.strftime("%H:%M:%S")
        cursor.insertText(f"[{timestamp}] {text}\n", fmt)
        if self.autoscroll_check.isChecked():
            self.terminal.setTextCursor(cursor)
            self.terminal.ensureCursorVisible()


def main() -> int:
    app = QApplication(sys.argv)
    app.setApplicationName("ARK Krul Control Panel")
    window = MainWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
