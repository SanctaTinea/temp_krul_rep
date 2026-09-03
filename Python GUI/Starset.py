#!/usr/bin/env python3
"""Universal Krul JSON/BSON/CBOR control panel for compatible controllers."""

from __future__ import annotations

import json
import math
import queue
import sys
import threading
import time
from collections import defaultdict
from pathlib import Path
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
from PySide6.QtCore import QEvent, QObject, QRectF, QSize, QThread, QTimer, Qt, Signal
from PySide6.QtGui import QColor, QPainter, QPalette, QTextCharFormat, QTextCursor
from PySide6.QtWidgets import (
    QAbstractButton,
    QAbstractSpinBox,
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
    QSlider,
    QSpinBox,
    QSplitter,
    QStyle,
    QTabWidget,
    QToolButton,
    QVBoxLayout,
    QWidget,
)

from GrathPlot import GrathPlotWindow, discover_measurements
from starset_config import DeviceProfileStore

PROTOCOL_VERSION = 4
DEFAULT_BAUDRATE = 115200
SERIAL_TIMEOUT = 0.1
RESPONSE_TIMEOUT_S = 4.0
DISCOVERY_TIMEOUT_S = 8.0
CONNECT_SETTLE_MS = 500
HEARTBEAT_INTERVAL_MS = 1000
HEARTBEAT_MISS_LIMIT = 3

# Layout dimensions (pixels). Keep geometry tuning in one place.
COMMAND_GROUP_SPACING = 0
COMMAND_FORM_SPACING = 6
COMMAND_SECTION_SPACING = 4
COMMAND_EXECUTE_TOP_SPACING = 20
COMMAND_GRID_MIN_CARD_WIDTH = 300
COMMAND_GRID_MIN_GROUP_WIDTH = 360
COMMAND_GRID_HORIZONTAL_SPACING = 12
COMMAND_GRID_VERTICAL_SPACING = 12
COMMAND_GRID_MAX_COLUMNS = 3
IO_FILTER_FIELD_SPACING = 8
IO_SECTION_TOP_MARGIN = 15
IO_OUTPUT_ACTIONS_TOP_SPACING = 12
PIN_GRID_VERTICAL_SPACING = 2
PIN_GRID_HORIZONTAL_SPACING = 15
PIN_CARD_VERTICAL_MARGIN = 3


# ---------------------------------------------------------------------------
# Centralized color themes
# Change only DEFAULT_THEME to select the theme used at startup.
# ---------------------------------------------------------------------------
DEFAULT_THEME = "dark"

THEMES: dict[str, dict[str, str]] = {
    "light": {
        "background": "#FFFFFF",
        "surface": "#F7F8FA",
        "terminal_background": "#F7F8FA",
        "default_input_background": "#FFFFFF",
        "card_color": "#F7F8FA",
        "white": "#1F2937",
        "red": "#FF0000",
        "button_text": "#FFFFFF",
        "surface_alt": "#F1F3F5",
        "input_background": "#FFFFFF",
        "border": "#DDE1E6",
        "border_strong": "#C8CED6",
        "text": "#1F2937",
        "text_secondary": "#6B7280",
        "text_disabled": "#9CA3AF",
        "accent": "#2563EB",
        "accent_hover": "#1D4ED8",
        "accent_pressed": "#1E40AF",
        "accent_text": "#FFFFFF",
        "success": "#22C55E",
        "success_hover": "#28E06A",
        "success_pressed": "#16A34A",
        "success_border": "#15803D",
        "success_soft": "#D8F5DF",
        "success_text": "#16752D",
        "danger": "#EF4444",
        "danger_hover": "#FF5555",
        "danger_pressed": "#D92D2D",
        "danger_border": "#B91C1C",
        "danger_soft": "#F5DDDD",
        "danger_text": "#8B2020",
        "inactive": "#5F5AE2",
        "inactive_border": "#4540C9",
        "pending": "#C4C92E",
        "pending_border": "#929617",
        "pending_text": "#1F2937",
        "utility_button": "#E5E7EB",
        "utility_button_hover": "#D1D5DB",
        "utility_button_pressed": "#C4C8CF",
        "warning": "#B7791F",
        "debug": "#7D8790",
        "terminal_warning": "#B7791F",
        "terminal_debug": "#7D8790",
        "terminal_info": "#1F6FEB",
        "terminal_error": "#C53030",
        "group_title": "#888888",
        "group_title_text": "#FFFFFF",
        "welcome_accent": "#6D3AA8",
    },
    "dark": {
        "background": "#1E1F22",
        "surface": "#2B2D30",
        "terminal_background": "#101015",
        "default_input_background": "#2B2D30",
        "accent": "#3574F0",
        "border": "#5A5D63",
        "card_color": "#2B2D30",
        "white": "#FFFFFF",

        "button_text": "#FFFFFF",

        "terminal_warning": "#E0A84B",
        "terminal_debug": "#8D98A7",
        "terminal_info": "#69A2FF",
        "terminal_error": "#FF7474",

        "red": "#FF0000",

        "surface_alt": "#252A33",
        "input_background": "#20252D",

        "border_strong": "#46505E",
        "text": "#E7EAF0",
        "text_secondary": "#AAB2BF",
        "text_disabled": "#707987",

        "accent_hover": "#6AA1FF",
        "accent_pressed": "#3478E5",
        "accent_text": "#FFFFFF",
        "success": "#35C96F",
        "success_hover": "#49DB80",
        "success_pressed": "#22AD5B",
        "success_border": "#24864D",
        "success_soft": "#173825",
        "success_text": "#76E39C",
        "danger": "#F05A5A",
        "danger_hover": "#FF7070",
        "danger_pressed": "#D94747",
        "danger_border": "#A63D3D",
        "danger_soft": "#3A2023",
        "danger_text": "#FF9494",
        "inactive": "#5F5AE2",
        "inactive_border": "#4540C9",
        "pending": "#C4C92E",
        "pending_border": "#929617",
        "pending_text": "#1F2937",
        "utility_button": "#3A3F47",
        "utility_button_hover": "#48505B",
        "utility_button_pressed": "#30353C",
        "warning": "#E0A84B",
        "debug": "#8D98A7",


        "group_title": "#555555",
        "group_title_text": "#FFFFFF",
        "welcome_accent": "#FFFFFF",
    },
}

_active_theme = DEFAULT_THEME


def theme_color(name: str) -> str:
    return THEMES[_active_theme][name]


def set_button_pending(button: QPushButton, pending: bool) -> None:
    """Refresh the shared visual state for an in-flight command button."""

    button.setProperty("commandPending", pending)
    button.style().unpolish(button)
    button.style().polish(button)
    button.update()


def set_theme(name: str) -> None:
    global _active_theme
    if name not in THEMES:
        raise ValueError(f"Unknown theme: {name}")
    _active_theme = name


def application_palette() -> QPalette:
    c = THEMES[_active_theme]
    palette = QPalette()

    # QPalette меняет цвета стандартных Qt-контролов, не подменяя их
    # нативную геометрию, padding и размеры, как это делает глобальный QSS.
    palette.setColor(QPalette.Window, QColor(c["background"]))
    palette.setColor(QPalette.WindowText, QColor(c["text"]))
    palette.setColor(QPalette.Base, QColor(c["input_background"]))
    palette.setColor(QPalette.AlternateBase, QColor(c["surface_alt"]))
    palette.setColor(QPalette.Text, QColor(c["text"]))
    palette.setColor(QPalette.Button, QColor(c["surface"]))
    palette.setColor(QPalette.ButtonText, QColor(c["button_text"]))
    palette.setColor(QPalette.Highlight, QColor(c["accent"]))
    palette.setColor(QPalette.HighlightedText, QColor(c["accent_text"]))
    palette.setColor(QPalette.PlaceholderText, QColor(c["text_disabled"]))
    palette.setColor(QPalette.ToolTipBase, QColor(c["surface"]))
    palette.setColor(QPalette.ToolTipText, QColor(c["text"]))

    disabled = QPalette.Disabled
    palette.setColor(disabled, QPalette.WindowText, QColor(c["text_disabled"]))
    palette.setColor(disabled, QPalette.Text, QColor(c["text_disabled"]))
    palette.setColor(disabled, QPalette.ButtonText, QColor(c["text_disabled"]))

    return palette


def application_stylesheet() -> str:
    c = THEMES[_active_theme]
    checkmark = (Path(__file__).resolve().parent / "assets" / "check.svg").as_posix()
    combo_arrow = (
        Path(__file__).resolve().parent
        / "assets"
        / ("combo-arrow-dark.svg" if _active_theme == "dark" else "combo-arrow-light.svg")
    ).as_posix()
    return f"""
        /* Только специальные элементы приложения.
           Стандартные QPushButton/QLineEdit/QComboBox/... намеренно здесь
           не стилизуются, чтобы Qt сохранил их родные padding и метрики. */
           
        QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox, QPlainTextEdit {{
            background: {c["default_input_background"]};
            border: 1px solid {c["border"]};
            border-radius: 5px;
            padding: 3px;
        }}

        QSpinBox, QDoubleSpinBox, QComboBox {{
            padding-right: 3px;
        }}

        QSpinBox::up-button, QSpinBox::down-button,
        QDoubleSpinBox::up-button, QDoubleSpinBox::down-button {{
            border: none;
            background: transparent;
            width: 0px;
        }}

        QSpinBox::up-arrow, QSpinBox::down-arrow,
        QDoubleSpinBox::up-arrow, QDoubleSpinBox::down-arrow {{
            image: none;
            width: 0px;
            height: 0px;
        }}

        QComboBox::drop-down {{
            border: none;
            background: transparent;
            width: 22px;
        }}

        QComboBox::down-arrow {{
            image: url("{combo_arrow}");
            width: 10px;
            height: 6px;
        }}

        QMessageBox {{
            background-color: {c["surface"]};
        }}

        QMessageBox QLabel {{
            color: {c["text"]};
            background: transparent;
        }}

        QMessageBox QPushButton {{
            color: {c["accent_text"]};
            min-width: 72px;
        }}
        
        QPushButton {{
            background-color: {c["accent"]};
            border: 0px solid {c["border"]};
            border-radius: 5px;
            padding: 5px;
        }}

        QPushButton[commandPending="true"],
        QPushButton[commandPending="true"]:disabled {{
            background-color: {c["pending"]};
            color: {c["pending_text"]};
        }}

        QToolButton#utilityButton {{
            background-color: {c["utility_button"]};
            border: 1px solid {c["border_strong"]};
            border-radius: 5px;
            padding: 3px;
        }}

        QToolButton#utilityButton:hover {{
            background-color: {c["utility_button_hover"]};
        }}

        QToolButton#utilityButton:pressed {{
            background-color: {c["utility_button_pressed"]};
        }}

        QLabel#commandDescription {{
            color: {c["text_secondary"]};
        }}

        QLabel#welcomeTitle {{
            color: {c["welcome_accent"]};
        }}

        QPlainTextEdit#terminal {{
            background: {c["terminal_background"]};
        }}

        QCheckBox::indicator {{
            width: 14px;
            height: 14px;
            background-color: {c["surface"]};
            border: 1px solid {c["border"]};
            border-radius: 2px;
        }}

        QCheckBox::indicator:checked {{
            background-color: {c["accent"]};
            border: 1px solid {c["accent"]};
            image: url("{checkmark}");
        }}
        
        QFrame {{
            border: 0px solid {c["red"]};;
        }}

        QTabWidget::pane {{
            border: 0px solid {c["red"]};
        }}
        
        QTabBar::tab {{
            background-color: {c["surface"]};
            color: {c["text_secondary"]};
            border: 0px solid {c["red"]};
            padding: 5px 10px 5px 10px;
        }}

        QTabBar::tab:selected {{
            background-color: {c["background"]};
            color: {c["text"]};
            padding: 7px 12px 7px 12px;
        }}

        QGroupBox#commandForm {{
            background-color: {c["card_color"]};
            color: {c["white"]};
            border: 0px solid red;
            margin-top: 0px;
            padding-top: 8px;
            
        }}

        QGroupBox#commandGroup {{
            background-color: {c["surface"]};
            border: 0px solid {c["red"]};
            border-radius: 4px;
            margin-top: 12px;
            padding: 8px;
        }}

        QGroupBox#commandGroup::title {{
            
            left: 12px;
            padding: 0 6px;
            letter-spacing: 2px;
            color: {c["group_title_text"]};
            font-weight: 600;
            background-color: {c["group_title"]};
        }}
        
        QGroupBox {{
            border: 0px solid {c["red"]};
            color: {c["text"]};
            background-color: {c["surface"]};
            border-radius: 4px;
            margin-top: 12px;
            padding: 8px;
        }}
        
        QWidget#GPIO_outer {{
            background-color: {c["surface"]};
            border: 0px solid {c["red"]};
        }}

        QGroupBox#gpioSection {{
            padding-left: 0px;
            padding-right: 0px;
        }}
        
        
    
        
        QScrollArea {{
            border: none;
            background: transparent;
        }}
        
        QScrollBar:vertical {{
            background: transparent;
            width: 10px;
            margin: 2px;
        }}
        
        QScrollBar::handle:vertical {{
            background: {c["border"]};
            min-height: 30px;
            border-radius: 4px;
        }}
        
        QScrollBar::handle:vertical:hover {{
            background: {c["text_disabled"]};
        }}
        
        QScrollBar::handle:vertical:pressed {{
            background: {c["text_secondary"]};
        }}
        
        QScrollBar::add-line:vertical,
        QScrollBar::sub-line:vertical {{
            height: 0px;
        }}
        
        QScrollBar::add-page:vertical,
        QScrollBar::sub-page:vertical {{
            background: transparent;
        }}
        
        
        QScrollBar:horizontal {{
            background: transparent;
            height: 10px;
            margin: 2px;
        }}
        
        QScrollBar::handle:horizontal {{
            background: {c["border"]};
            min-width: 30px;
            border-radius: 4px;
        }}
        
        QScrollBar::handle:horizontal:hover {{
            background: {c["text_disabled"]};
        }}
        
        QScrollBar::handle:horizontal:pressed {{
            background: {c["text_secondary"]};
        }}
        
        QScrollBar::add-line:horizontal,
        QScrollBar::sub-line:horizontal {{
            width: 0px;
        }}
        
        QScrollBar::add-page:horizontal,
        QScrollBar::sub-page:horizontal {{
            background: transparent;
        }}
    """


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


class Indicator(QWidget):
    def __init__(self) -> None:
        super().__init__()

        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(6)
        # layout.setStyleSheet(f"""border: 1px solid""")

        self.led = QLabel()
        self.led.setFixedSize(14, 14)

        self.text = QLabel()
        self.text.setMinimumWidth(35)

        layout.addWidget(self.led)
        layout.addWidget(self.text)

        self.set_state(False)

    def set_state(self, state: bool) -> None:
        color = theme_color("success") if state else theme_color("inactive")
        border = theme_color("success_border") if state else theme_color("inactive_border")

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


class PinSwitch(QAbstractButton):
    """GPIO output switch whose position follows confirmed pin state."""

    state_requested = Signal(int)

    def __init__(self, state: int = 0) -> None:
        super().__init__()
        self._state = bool(state)
        self._pending = False
        self.setFixedSize(40, 20)
        self.setCursor(Qt.PointingHandCursor)
        self.setAccessibleName("Состояние выхода")
        self.clicked.connect(self._request_opposite_state)
        self._refresh_tooltip()

    def set_state(self, state: int) -> None:
        confirmed_state = bool(state)
        self._state = confirmed_state
        self.update()
        self._refresh_tooltip()

    def state(self) -> int:
        return int(self._state)

    def set_pending(self, pending: bool) -> None:
        self._pending = pending
        self._refresh_tooltip()
        self.update()

    def is_pending(self) -> bool:
        return self._pending

    def _request_opposite_state(self) -> None:
        if self._pending:
            return
        self.state_requested.emit(0 if self._state else 1)

    def _refresh_tooltip(self) -> None:
        if self._pending:
            self.setToolTip("Ожидание ответа…")
        else:
            self.setToolTip("Выключить" if self._state else "Включить")

    def paintEvent(self, event) -> None:  # noqa: N802 - Qt API
        del event
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing)

        if self._pending:
            color_name = "pending"
            border_name = "pending_border"
        else:
            color_name = "success" if self._state else "inactive"
            border_name = "success_border" if self._state else "inactive_border"
        track_color = QColor(theme_color(color_name))
        border_color = QColor(theme_color(border_name))
        if self.isDown():
            track_color = track_color.darker(112)
            border_color = border_color.darker(112)
        if not self.isEnabled():
            track_color.setAlpha(110)
            border_color.setAlpha(110)

        track = QRectF(1.0, 1.0, self.width() - 2.0, self.height() - 2.0)
        radius = track.height() / 2.0
        painter.setPen(border_color)
        painter.setBrush(track_color)
        painter.drawRoundedRect(track, radius, radius)

        knob_size = self.height() - 6.0
        knob_x = self.width() - knob_size - 3.0 if self._state else 3.0
        painter.setPen(Qt.NoPen)
        painter.setBrush(QColor("#FFFFFF"))
        painter.drawEllipse(QRectF(knob_x, 3.0, knob_size, knob_size))


class PinCard(QFrame):
    set_requested = Signal(object, int)

    def __init__(self, name: str, pin_type: str, state: int,
                 wire_value: Any | None = None):
        super().__init__()
        self.name = name
        self.wire_value = name if wire_value is None else wire_value
        self.pin_type = pin_type
        self.setFrameShape(QFrame.StyledPanel)
        layout = QHBoxLayout(self)
        layout.setContentsMargins(
            0, PIN_CARD_VERTICAL_MARGIN, 0, PIN_CARD_VERTICAL_MARGIN
        )
        # layout.setContentsMargins(
        #     0, 0, 0, 0
        # )

        self.indicator = Indicator()
        layout.addWidget(self.indicator)

        label = QLabel(name)
        label.setTextInteractionFlags(Qt.TextSelectableByMouse)
        label.setStyleSheet(f"""
                            QLabel {{
                                font-weight: regular;
                                letter-spacing: 1px;
                            }}""")
        layout.addWidget(label, 1)

        self.switch: PinSwitch | None = None
        if pin_type == "OUT":
            self.switch = PinSwitch(state)
            self.switch.state_requested.connect(
                lambda requested: self.set_requested.emit(
                    self.wire_value, requested)
            )
            layout.addWidget(self.switch)
        self.set_state(state)

    def set_state(self, state: int) -> None:
        self.state = int(bool(state))
        self.indicator.set_state(bool(self.state))
        if self.switch is not None:
            self.switch.set_state(self.state)

    def set_pending(self, pending: bool) -> None:
        if self.switch is not None:
            self.switch.set_pending(pending)


class ResultIntLabel(QLabel):
    def __init__(self, compact: bool = False, emphasized: bool = True):
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

        margin = 0 if compact else 100
        font_weight = 600 if emphasized else 400
        self.setStyleSheet(f"""
            QLabel {{
                margin-left: {margin}px;
                padding: 3px 8px;
                font-weight: {font_weight};
            }}
        """)


class WidgetCompatibilityError(ValueError):
    """Descriptor cannot be handled by a registered widget class."""


class ParameterWidget(QWidget):
    """Base contract for registered input widgets."""

    def __init__(self, field: dict[str, Any]) -> None:
        super().__init__()
        self.field = field

    @classmethod
    def validate_descriptor(cls, field: dict[str, Any]) -> None:
        if not isinstance(field, dict):
            raise WidgetCompatibilityError("дескриптор поля должен быть object")

    def value(self) -> Any:
        raise NotImplementedError


class SpecialDacParameterWidget(ParameterWidget):
    """Synchronized full-width slider and numeric editor for a DAC code."""

    @classmethod
    def validate_descriptor(cls, field: dict[str, Any]) -> None:
        super().validate_descriptor(field)
        if field.get("type") != "integer":
            raise WidgetCompatibilityError(
                "special_dac поддерживает только поле типа integer"
            )
        constraints = field.get("constraints")
        if not isinstance(constraints, dict):
            raise WidgetCompatibilityError(
                "special_dac требует числовые ограничения minimum/maximum"
            )
        try:
            minimum = int(constraints["minimum"])
            maximum = int(constraints["maximum"])
        except (KeyError, TypeError, ValueError):
            raise WidgetCompatibilityError(
                "special_dac требует числовые ограничения minimum/maximum"
            ) from None
        if minimum >= maximum:
            raise WidgetCompatibilityError(
                "special_dac требует minimum меньше maximum"
            )

    def __init__(self, field: dict[str, Any]) -> None:
        super().__init__(field)
        constraints = field["constraints"]
        minimum = int(constraints["minimum"])
        maximum = int(constraints["maximum"])
        default = int(field.get("default", minimum))

        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(8)
        self.slider = QSlider(Qt.Horizontal)
        self.slider.setObjectName("dacSlider")
        self.slider.setRange(minimum, maximum)
        self.slider.setValue(default)
        self.spinbox = QSpinBox()
        self.spinbox.setObjectName("dacValue")
        self.spinbox.setRange(minimum, maximum)
        self.spinbox.setValue(default)
        self.spinbox.setMinimumWidth(90)
        self.slider.valueChanged.connect(self.spinbox.setValue)
        self.spinbox.valueChanged.connect(self.slider.setValue)
        layout.addWidget(self.slider, 1)
        layout.addWidget(self.spinbox)

    def value(self) -> int:
        return self.spinbox.value()


class ResultWidget(QWidget):
    """Base contract for registered result widgets."""

    bold_label = False

    def __init__(self, field: dict[str, Any]) -> None:
        super().__init__()
        self.field = field

    @classmethod
    def validate_descriptor(cls, field: dict[str, Any]) -> None:
        if not isinstance(field, dict):
            raise WidgetCompatibilityError("дескриптор поля должен быть object")

    def setValue(self, value: Any) -> None:
        raise NotImplementedError


class SpecialAdcResultWidget(ResultWidget):
    """Raw integer ADC value with client-side scaling controls."""

    @classmethod
    def validate_descriptor(cls, field: dict[str, Any]) -> None:
        super().validate_descriptor(field)
        if field.get("type") != "integer":
            raise WidgetCompatibilityError(
                "special_adc поддерживает только поле типа integer"
            )

    def __init__(self, _field: dict[str, Any]) -> None:
        super().__init__(_field)
        self._raw_value: int | None = None

        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(6)

        self.raw_output = ResultIntLabel(compact=True, emphasized=False)
        self.raw_output.setObjectName("adcRawValue")
        self.raw_output.setToolTip("RAW значение АЦП")
        layout.addWidget(self.raw_output)

        layout.addWidget(QLabel("Vоп:"))
        self.reference_voltage = QDoubleSpinBox()
        self.reference_voltage.setObjectName("adcReferenceVoltage")
        self.reference_voltage.setDecimals(6)
        self.reference_voltage.setRange(0.0, 1e9)
        self.reference_voltage.setSingleStep(0.1)
        self.reference_voltage.setValue(3.3)
        self.reference_voltage.setToolTip("Опорное напряжение")
        self.reference_voltage.setFixedWidth(90)
        self.reference_voltage.setButtonSymbols(QDoubleSpinBox.NoButtons)
        layout.addWidget(self.reference_voltage)

        layout.addWidget(QLabel("Коэф.:"))
        self.scale_factor = QDoubleSpinBox()
        self.scale_factor.setObjectName("adcScaleFactor")
        self.scale_factor.setDecimals(6)
        self.scale_factor.setRange(-1e9, 1e9)
        self.scale_factor.setSingleStep(0.1)
        self.scale_factor.setValue(1.0)
        self.scale_factor.setToolTip("Дополнительный коэффициент")
        self.scale_factor.setFixedWidth(90)
        self.scale_factor.setButtonSymbols(QDoubleSpinBox.NoButtons)
        layout.addWidget(self.scale_factor)

        layout.addWidget(QLabel("Vбаз:"))
        self.base_voltage = QDoubleSpinBox()
        self.base_voltage.setObjectName("adcBaseVoltage")
        self.base_voltage.setDecimals(6)
        self.base_voltage.setRange(-1e9, 1e9)
        self.base_voltage.setSingleStep(0.1)
        self.base_voltage.setValue(0.0)
        self.base_voltage.setToolTip("Напряжение, добавляемое к результату")
        self.base_voltage.setFixedWidth(90)
        self.base_voltage.setButtonSymbols(QDoubleSpinBox.NoButtons)
        layout.addWidget(self.base_voltage)

        layout.addWidget(QLabel("Бит:"))
        self.resolution_bits = QSpinBox()
        self.resolution_bits.setObjectName("adcResolutionBits")
        self.resolution_bits.setRange(1, 32)
        self.resolution_bits.setValue(12)
        self.resolution_bits.setToolTip("Разрядность АЦП")
        self.resolution_bits.setFixedWidth(30)
        self.resolution_bits.setButtonSymbols(QDoubleSpinBox.NoButtons)

        layout.addWidget(self.resolution_bits)

        layout.addWidget(QLabel("="))
        self.scaled_output = QLabel("—")
        self.scaled_output.setObjectName("adcScaledValue")
        self.scaled_output.setAlignment(Qt.AlignRight | Qt.AlignVCenter)
        self.scaled_output.setTextInteractionFlags(Qt.TextSelectableByMouse)
        self.scaled_output.setMinimumWidth(90)
        self.scaled_output.setStyleSheet(
            "QLabel { padding: 3px 8px; font-weight: 600; }"
        )
        self.scaled_output.setToolTip(
            "RAW × Vоп × коэффициент / (2^разрядность − 1) + Vбаз"
        )
        layout.addWidget(self.scaled_output)
        layout.addStretch()

        self.reference_voltage.valueChanged.connect(self._update_scaled_value)
        self.scale_factor.valueChanged.connect(self._update_scaled_value)
        self.base_voltage.valueChanged.connect(self._update_scaled_value)
        self.resolution_bits.valueChanged.connect(self._update_scaled_value)

    def setValue(self, value: Any) -> None:
        try:
            self._raw_value = int(value)
        except (TypeError, ValueError, OverflowError):
            self._raw_value = None

        self.raw_output.setText(
            "—" if self._raw_value is None else str(self._raw_value)
        )
        self._update_scaled_value()

    def profile_configuration(self) -> dict[str, float | int]:
        return {
            "reference_voltage": self.reference_voltage.value(),
            "scale_factor": self.scale_factor.value(),
            "base_voltage": self.base_voltage.value(),
            "resolution_bits": self.resolution_bits.value(),
        }

    def apply_profile_configuration(self, config: dict[str, Any]) -> None:
        controls = {
            "reference_voltage": self.reference_voltage,
            "scale_factor": self.scale_factor,
            "base_voltage": self.base_voltage,
            "resolution_bits": self.resolution_bits,
        }
        for name, control in controls.items():
            if name not in config:
                continue
            blocked = control.blockSignals(True)
            try:
                control.setValue(config[name])
            except (TypeError, ValueError, OverflowError):
                pass
            finally:
                control.blockSignals(blocked)
        self._update_scaled_value()

    def _update_scaled_value(self) -> None:
        if self._raw_value is None:
            self.scaled_output.setText("—")
            return

        full_scale = (1 << self.resolution_bits.value()) - 1
        scaled = (
            self._raw_value
            * self.reference_voltage.value()
            * self.scale_factor.value()
            / full_scale
            + self.base_voltage.value()
        )
        self.scaled_output.setText(f"{scaled:.9g}")


class SpecialAdcGroupResultWidget(ResultWidget):
    """ADC object whose integer fields share one scaling configuration."""

    bold_label = True

    @classmethod
    def validate_descriptor(cls, field: dict[str, Any]) -> None:
        super().validate_descriptor(field)
        if field.get("type") != "object":
            raise WidgetCompatibilityError(
                "special_adc_group поддерживает только поле типа object"
            )
        children = field.get("fields")
        if not isinstance(children, list) or not children:
            raise WidgetCompatibilityError(
                "special_adc_group требует непустой список fields"
            )
        invalid = [
            str(child.get("name") or "?")
            for child in children
            if not isinstance(child, dict)
            or child.get("type") != "integer"
            or not child.get("name")
        ]
        if invalid:
            raise WidgetCompatibilityError(
                "все fields special_adc_group должны быть именованными integer: "
                + ", ".join(invalid)
            )

    def __init__(self, field: dict[str, Any]) -> None:
        super().__init__(field)
        self._raw_values: dict[str, int | None] = {}
        self.raw_outputs: dict[str, ResultIntLabel] = {}
        self.scaled_outputs: dict[str, QLabel] = {}

        layout = QVBoxLayout(self)
        layout.setContentsMargins(12, 8, 0, 8)
        layout.setSpacing(6)

        controls = QHBoxLayout()
        controls.addWidget(QLabel("Vоп:"))
        self.reference_voltage = QDoubleSpinBox()
        self.reference_voltage.setObjectName("adcGroupReferenceVoltage")
        self.reference_voltage.setDecimals(6)
        self.reference_voltage.setRange(0.0, 1e9)
        self.reference_voltage.setSingleStep(0.1)
        self.reference_voltage.setValue(3.3)
        self.reference_voltage.setToolTip("Опорное напряжение группы")
        self.reference_voltage.setFixedWidth(90)
        self.reference_voltage.setButtonSymbols(QDoubleSpinBox.NoButtons)
        controls.addWidget(self.reference_voltage)

        controls.addWidget(QLabel("Коэф.:"))
        self.scale_factor = QDoubleSpinBox()
        self.scale_factor.setObjectName("adcGroupScaleFactor")
        self.scale_factor.setDecimals(6)
        self.scale_factor.setRange(-1e9, 1e9)
        self.scale_factor.setSingleStep(0.1)
        self.scale_factor.setValue(1.0)
        self.scale_factor.setToolTip("Общий дополнительный коэффициент")
        self.scale_factor.setFixedWidth(90)
        self.scale_factor.setButtonSymbols(QDoubleSpinBox.NoButtons)
        controls.addWidget(self.scale_factor)

        controls.addWidget(QLabel("Vбаз:"))
        self.base_voltage = QDoubleSpinBox()
        self.base_voltage.setObjectName("adcGroupBaseVoltage")
        self.base_voltage.setDecimals(6)
        self.base_voltage.setRange(-1e9, 1e9)
        self.base_voltage.setSingleStep(0.1)
        self.base_voltage.setValue(0.0)
        self.base_voltage.setToolTip(
            "Напряжение, добавляемое к результатам группы"
        )
        self.base_voltage.setFixedWidth(90)
        self.base_voltage.setButtonSymbols(QDoubleSpinBox.NoButtons)
        controls.addWidget(self.base_voltage)

        controls.addWidget(QLabel("Бит:"))
        self.resolution_bits = QSpinBox()
        self.resolution_bits.setObjectName("adcGroupResolutionBits")
        self.resolution_bits.setRange(1, 32)
        self.resolution_bits.setValue(12)
        self.resolution_bits.setToolTip("Общая разрядность АЦП")
        self.resolution_bits.setFixedWidth(30)
        self.resolution_bits.setButtonSymbols(QDoubleSpinBox.NoButtons)
        controls.addWidget(self.resolution_bits)
        controls.addStretch()
        layout.addLayout(controls)

        values = QGridLayout()
        values.setContentsMargins(0, 0, 0, 0)
        values.setHorizontalSpacing(12)
        values.addWidget(QLabel("Канал"), 0, 0)
        values.addWidget(QLabel("RAW"), 0, 1)
        values.addWidget(QLabel("Результат"), 0, 2)

        row = 1
        for child in field.get("fields", []):
            if child.get("type") != "integer" or not child.get("name"):
                continue
            name = str(child["name"])
            label = str(child.get("label") or name)
            raw_output = ResultIntLabel(compact=True, emphasized=False)
            raw_output.setObjectName(f"adcGroupRaw_{name}")
            scaled_output = QLabel("—")
            scaled_output.setObjectName(f"adcGroupScaled_{name}")
            scaled_output.setAlignment(Qt.AlignRight | Qt.AlignVCenter)
            scaled_output.setTextInteractionFlags(Qt.TextSelectableByMouse)
            scaled_output.setMinimumWidth(90)
            scaled_output.setStyleSheet(
                "QLabel { padding: 3px 8px; font-weight: 600; }"
            )
            scaled_output.setToolTip(
                "RAW × Vоп × коэффициент / (2^разрядность − 1) + Vбаз"
            )

            self._raw_values[name] = None
            self.raw_outputs[name] = raw_output
            self.scaled_outputs[name] = scaled_output
            values.addWidget(QLabel(label), row, 0)
            values.addWidget(raw_output, row, 1)
            values.addWidget(scaled_output, row, 2)
            row += 1

        values.setColumnStretch(3, 1)
        layout.addLayout(values)

        self.reference_voltage.valueChanged.connect(self._update_scaled_values)
        self.scale_factor.valueChanged.connect(self._update_scaled_values)
        self.base_voltage.valueChanged.connect(self._update_scaled_values)
        self.resolution_bits.valueChanged.connect(self._update_scaled_values)

    def setValue(self, value: Any) -> None:
        result = value if isinstance(value, dict) else {}
        for name, raw_output in self.raw_outputs.items():
            raw_value = result.get(name)
            try:
                parsed_value = int(raw_value)
            except (TypeError, ValueError, OverflowError):
                parsed_value = None
            self._raw_values[name] = parsed_value
            raw_output.setText("—" if parsed_value is None else str(parsed_value))
        self._update_scaled_values()

    def profile_configuration(self) -> dict[str, float | int]:
        return {
            "reference_voltage": self.reference_voltage.value(),
            "scale_factor": self.scale_factor.value(),
            "base_voltage": self.base_voltage.value(),
            "resolution_bits": self.resolution_bits.value(),
        }

    def apply_profile_configuration(self, config: dict[str, Any]) -> None:
        controls = {
            "reference_voltage": self.reference_voltage,
            "scale_factor": self.scale_factor,
            "base_voltage": self.base_voltage,
            "resolution_bits": self.resolution_bits,
        }
        for name, control in controls.items():
            if name not in config:
                continue
            blocked = control.blockSignals(True)
            try:
                control.setValue(config[name])
            except (TypeError, ValueError, OverflowError):
                pass
            finally:
                control.blockSignals(blocked)
        self._update_scaled_values()

    def _update_scaled_values(self) -> None:
        full_scale = (1 << self.resolution_bits.value()) - 1
        multiplier = (
            self.reference_voltage.value() * self.scale_factor.value()
        )
        for name, scaled_output in self.scaled_outputs.items():
            raw_value = self._raw_values[name]
            if raw_value is None:
                scaled_output.setText("—")
                continue
            scaled_output.setText(
                f"{raw_value * multiplier / full_scale + self.base_voltage.value():.9g}"
            )


PARAM_WIDGETS: dict[str, type[ParameterWidget]] = {
    "special_dac": SpecialDacParameterWidget,
}

RESULT_WIDGETS: dict[str, type[ResultWidget]] = {
    "special_adc": SpecialAdcResultWidget,
    "special_adc_group": SpecialAdcGroupResultWidget,
}


class ResultBoolLabel(QLabel):
    def __init__(self):
        super().__init__("—")

        self.setAlignment(Qt.AlignCenter)
        self.setMinimumWidth(55)

    def setValue(self, value: bool):
        if value:
            self.setText("SUCCESS")
            self.setStyleSheet(f"""
                QLabel {{
                    padding: 3px 8px;
                    border-radius: 5px;
                    background: {theme_color("success_soft")};
                    color: {theme_color("success_text")};
                    font-weight: 600;
                }}
            """)
        else:
            self.setText("FAIL")
            self.setStyleSheet(f"""
                QLabel {{
                    padding: 3px 8px;
                    border-radius: 5px;
                    background: {theme_color("danger_soft")};
                    color: {theme_color("danger_text")};
                    font-weight: 600;
                }}
            """)


class ResultEnumLabel(QLabel):
    """Read-only enum result rendered with its descriptor title."""

    def __init__(self, field: dict[str, Any]):
        super().__init__("—")
        self._titles: dict[str, str] = {}
        constraints = field.get("constraints", {})
        for item in constraints.get("values", []):
            if not isinstance(item, dict) or "value" not in item:
                continue
            value = str(item["value"])
            self._titles[value] = str(item.get("title", value))
        self.setTextInteractionFlags(Qt.TextSelectableByMouse)
        self.setWordWrap(True)
        self.setAutoFillBackground(False)
        self.setStyleSheet(
            "QLabel { border: none; background: transparent; }"
        )

    def setValue(self, value: Any) -> None:
        if value is None:
            self.setText("—")
            self.setToolTip("")
            return
        raw_value = str(value)
        title = self._titles.get(raw_value, raw_value)
        self.setText(title)
        self.setToolTip(
            f"Значение протокола: {raw_value}" if title != raw_value else ""
        )


class ResponsiveCardGrid(QWidget):
    """Width-aware masonry grid for command and command-group cards."""

    def __init__(self, min_column_width: int, max_columns: int,
                 parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.min_column_width = max(1, min_column_width)
        self.max_columns = max(1, max_columns)
        self.cards: list[QWidget] = []
        self._columns = 1
        self._content_height = 0
        self._card_spans: dict[QWidget, int] = {}
        self._relayout_timer = QTimer(self)
        self._relayout_timer.setSingleShot(True)
        self._relayout_timer.timeout.connect(self._relayout)
        self.setObjectName("responsiveCardGrid")
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)

    def add_card(self, card: QWidget) -> None:
        card.setParent(self)
        card.installEventFilter(self)
        card.show()
        self.cards.append(card)
        self._schedule_relayout()

    def eventFilter(self, watched: QObject, event: QEvent) -> bool:  # noqa: N802
        if watched in self.cards and event.type() in {
            QEvent.LayoutRequest,
            QEvent.Show,
            QEvent.Hide,
        }:
            self._schedule_relayout()
        return super().eventFilter(watched, event)

    def resizeEvent(self, event) -> None:  # noqa: N802 - Qt API
        super().resizeEvent(event)
        self._relayout()

    def sizeHint(self) -> QSize:  # noqa: N802 - Qt API
        visible = [card for card in self.cards if not card.isHidden()]
        if not visible:
            return QSize(self.min_column_width, 0)
        recommended_columns = min(
            self.max_columns, max(1, math.ceil(math.sqrt(len(visible))))
        )
        base_width = (
            recommended_columns * self.min_column_width
            + (recommended_columns - 1) * COMMAND_GRID_HORIZONTAL_SPACING
        )
        widest = max(self._preferred_width(card) for card in visible)
        maximum_width = (
            self.max_columns * self.min_column_width
            + (self.max_columns - 1) * COMMAND_GRID_HORIZONTAL_SPACING
        )
        width = max(base_width, min(widest, maximum_width))
        height = self._content_height or max(
            card.sizeHint().height() for card in visible
        )
        return QSize(width, height)

    def minimumSizeHint(self) -> QSize:  # noqa: N802 - Qt API
        return QSize(self.min_column_width, self._content_height)

    def _schedule_relayout(self) -> None:
        if not self._relayout_timer.isActive():
            self._relayout_timer.start(0)

    def request_relayout(self) -> None:
        self._schedule_relayout()

    @staticmethod
    def _preferred_width(card: QWidget) -> int:
        return max(
            1,
            card.minimumSizeHint().width(),
            card.sizeHint().width(),
        )

    @staticmethod
    def _height_for_width(card: QWidget, width: int) -> int:
        height = card.heightForWidth(width) if card.hasHeightForWidth() else -1
        if height < 0:
            height = card.sizeHint().height()
        return max(1, card.minimumSizeHint().height(), height)

    def _span_for_card(self, card: QWidget, columns: int,
                       column_width: float) -> int:
        if card.property("gridSpanMode") == "full":
            return columns
        minimum_width = max(1, card.minimumSizeHint().width())
        preferred_width = max(minimum_width, card.sizeHint().width())
        # A sizeHint can become very wide because of labels or a temporary
        # layout state. Treat it as a soft preference; only minimumSizeHint is
        # allowed to force an aggressively wide card.
        required_width = max(
            minimum_width,
            min(preferred_width, round(self.min_column_width * 1.35)),
        )
        natural_span = math.ceil(
            required_width / max(1.0, column_width * 1.25)
        )
        declared_span = card.property("gridPreferredSpan")
        if not isinstance(declared_span, int):
            declared_span = 1
        return min(columns, max(1, natural_span, declared_span))

    def _relayout(self) -> None:
        available_width = max(1, self.contentsRect().width())
        spacing_x = COMMAND_GRID_HORIZONTAL_SPACING
        spacing_y = COMMAND_GRID_VERTICAL_SPACING
        columns = max(
            1,
            min(
                self.max_columns,
                (available_width + spacing_x)
                // (self.min_column_width + spacing_x),
            ),
        )
        self._columns = columns
        column_width = (
            available_width - spacing_x * (columns - 1)
        ) / columns
        occupied: list[tuple[int, int, float, float]] = []
        self._card_spans.clear()

        for card in self.cards:
            if card.isHidden():
                continue
            span = self._span_for_card(card, columns, column_width)
            width = column_width * span + spacing_x * (span - 1)
            height = self._height_for_width(card, round(width))
            candidates: list[tuple[float, int]] = []
            for start in range(columns - span + 1):
                end = start + span
                horizontal = [
                    rectangle for rectangle in occupied
                    if start < rectangle[1] and end > rectangle[0]
                ]
                possible_tops = sorted({0.0, *(item[3] for item in horizontal)})
                for top in possible_tops:
                    bottom = top + height + spacing_y
                    if all(
                        bottom <= item[2] or top >= item[3]
                        for item in horizontal
                    ):
                        candidates.append((top, start))
                        break
            top, start = min(candidates)
            left = start * (column_width + spacing_x)
            card.setGeometry(round(left), round(top), round(width), height)
            bottom = top + height + spacing_y
            occupied.append((start, start + span, top, bottom))
            self._card_spans[card] = span

        content_height = max(
            0,
            round(max((item[3] for item in occupied), default=0.0) - spacing_y),
        )
        if self._content_height != content_height:
            self._content_height = content_height
            self.setFixedHeight(content_height)
            self.updateGeometry()


class ResponsivePinGrid(QWidget):
    def __init__(self):
        super().__init__()
        self.cards: list[PinCard] = []
        self.grid = QGridLayout(self)
        self.grid.setContentsMargins(0, 0, 0, 0)
        self.grid.setHorizontalSpacing(PIN_GRID_HORIZONTAL_SPACING)
        self.grid.setAlignment(Qt.AlignTop | Qt.AlignLeft)
        self.grid.setVerticalSpacing(PIN_GRID_VERTICAL_SPACING)
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
                 target: str | None = None,
                 get_command: str = "PIN_GET",
                 set_command: str = "PIN_SET",
                 all_value: Any = "ALL",
                 update_observer: Callable | None = None,
                 use_internal_scroll: bool = True):
        super().__init__()
        self.sender = sender
        self.target = target
        self.get_command = get_command
        self.set_command = set_command
        self.all_value = all_value
        self.update_observer = update_observer
        self.cards: dict[str, PinCard] = {}
        self._cards_by_wire: dict[Any, PinCard] = {}
        self._in_flight = False
        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(0)

        filter_row = QHBoxLayout()
        filter_row.setContentsMargins(0, 0, 0, 0)
        filter_row.setSpacing(IO_FILTER_FIELD_SPACING)
        filter_row.addWidget(QLabel("Фильтр:"))
        self.filter_edit = QLineEdit()
        self.filter_edit.setPlaceholderText("Имя пина")
        filter_row.addWidget(self.filter_edit, 1)
        outer.addLayout(filter_row)

        body = QWidget()
        body_layout = QVBoxLayout(body)
        body_layout.setContentsMargins(0, 0, 0, 0)
        body_layout.setSpacing(0)
        body.setObjectName("GPIO_outer")
        self.input_grid = ResponsivePinGrid()
        self.output_grid = ResponsivePinGrid()

        inputs = QGroupBox("Входы")
        inputs.setObjectName("gpioSection")
        input_layout = QVBoxLayout(inputs)
        input_layout.setContentsMargins(0, IO_SECTION_TOP_MARGIN, 0, 0)
        self.read_inputs_button = QPushButton("Прочитать")
        self.read_inputs_button.clicked.connect(
            lambda _checked=False: self.poll_inputs(self.read_inputs_button)
        )
        input_layout.addWidget(self.input_grid)
        input_layout.addWidget(self.read_inputs_button, 0, Qt.AlignRight)

        outputs = QGroupBox("Выходы")
        outputs.setObjectName("gpioSection")
        output_layout = QVBoxLayout(outputs)
        output_layout.setContentsMargins(0, IO_SECTION_TOP_MARGIN, 0, 0)
        all_row = QHBoxLayout()
        all_row.setContentsMargins(0, 0, 0, 0)

        all_row.addStretch()

        output_layout.addWidget(self.output_grid)
        output_layout.addSpacing(IO_OUTPUT_ACTIONS_TOP_SPACING)
        output_layout.addLayout(all_row)

        body_layout.addWidget(inputs)
        body_layout.addWidget(outputs)

        self.activate_all_button = QPushButton("Активировать все")
        self.deactivate_all_button = QPushButton("Деактивировать все")
        self.activate_all_button.clicked.connect(
            lambda _checked=False: self._set_all(1, self.activate_all_button)
        )
        self.deactivate_all_button.clicked.connect(
            lambda _checked=False: self._set_all(0, self.deactivate_all_button)
        )
        self.read_outputs_button = QPushButton("Прочитать")
        self.read_outputs_button.clicked.connect(
            lambda _checked=False: self.poll_outputs(self.read_outputs_button)
        )
        all_row.addWidget(self.read_outputs_button)
        all_row.addWidget(self.activate_all_button)
        all_row.addWidget(self.deactivate_all_button)
        body_layout.addStretch()
        if use_internal_scroll:
            scroll = QScrollArea()
            scroll.setWidgetResizable(True)
            scroll.setWidget(body)
            outer.addWidget(scroll)
        else:
            # The command tab already owns a vertical QScrollArea.  Let this
            # panel expose its complete size hint so many pins enlarge the
            # page instead of being squeezed into a nested scroll viewport.
            self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Minimum)
            body.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Minimum)
            outer.addWidget(body)

        for pin in sorted(pins, key=lambda value: str(value.get("name", "")).casefold()):
            name = str(pin.get("name", ""))
            pin_type = str(pin.get("direction", pin.get("type", "IN"))).upper()
            if not name or pin_type not in {"IN", "OUT"}:
                continue
            wire_name = pin.get("_wire_name", pin.get("name"))
            card = PinCard(name, pin_type, int(pin.get("state", 0)), wire_name)
            card.set_requested.connect(self._set_one)
            self.cards[name] = card
            self._cards_by_wire[wire_name] = card
            (self.output_grid if pin_type == "OUT" else self.input_grid).add_card(card)
        self.filter_edit.textChanged.connect(self._filter)

    def _filter(self, text: str) -> None:
        self.input_grid.apply_filter(text)
        self.output_grid.apply_filter(text)

    def _set_one(self, name: Any, state: int) -> None:
        card = self._cards_by_wire.get(name)
        if card is not None:
            card.set_pending(True)
        if self.target is None:
            params = {"pins": [{"name": name, "state": state}]}
        else:
            params = {"target": self.target, "name": name, "state": str(state)}

        def updated(message: dict[str, Any]) -> None:
            if card is not None:
                card.set_pending(False)
            self._updated(message)

        transaction = self.sender(self.set_command, params, updated)
        if transaction is None and card is not None:
            card.set_pending(False)

    def _set_all(self, state: int,
                 action_button: QPushButton | None = None) -> None:
        output_cards = [
            card for card in self.cards.values() if card.pin_type == "OUT"
        ]
        if not output_cards:
            return
        for card in output_cards:
            card.set_pending(True)
        if action_button is not None:
            set_button_pending(action_button, True)
            action_button.setEnabled(False)
        if self.target is None:
            params = {"pins": [{"name": self.all_value, "state": state}]}
        else:
            params = {"target": self.target, "name": "ALL", "state": str(state)}

        def updated(message: dict[str, Any]) -> None:
            for card in output_cards:
                card.set_pending(False)
            if action_button is not None:
                set_button_pending(action_button, False)
                action_button.setEnabled(True)
            self._updated(message)

        transaction = self.sender(self.set_command, params, updated)
        if transaction is None:
            for card in output_cards:
                card.set_pending(False)
            if action_button is not None:
                set_button_pending(action_button, False)
                action_button.setEnabled(True)

    def poll_inputs(self, action_button: QPushButton | None = None) -> None:
        if self._in_flight:
            return
        self._in_flight = True
        if action_button is not None:
            set_button_pending(action_button, True)
            action_button.setEnabled(False)
        if self.target is None:
            names = [card.wire_value for card in self.cards.values()
                     if card.pin_type == "IN"]
            params = {"pins": names}
        else:
            params = {"target": self.target, "name": "IN"}

        def updated(message: dict[str, Any]) -> None:
            if action_button is not None:
                set_button_pending(action_button, False)
                action_button.setEnabled(True)
            self._updated(message)

        transaction = self.sender(self.get_command, params, updated)
        if transaction is None:
            self._in_flight = False
            if action_button is not None:
                set_button_pending(action_button, False)
                action_button.setEnabled(True)

    def poll_outputs(self, action_button: QPushButton | None = None) -> None:
        if self._in_flight or not any(
                card.pin_type == "OUT" for card in self.cards.values()):
            return
        self._in_flight = True
        if action_button is not None:
            set_button_pending(action_button, True)
            action_button.setEnabled(False)
        if self.target is None:
            names = [
                card.wire_value for card in self.cards.values()
                if card.pin_type == "OUT"
            ]
            params = {"pins": names}
        else:
            params = {"target": self.target, "name": "OUT"}

        def updated(message: dict[str, Any]) -> None:
            if action_button is not None:
                set_button_pending(action_button, False)
                action_button.setEnabled(True)
            self._updated(message)

        transaction = self.sender(self.get_command, params, updated)
        if transaction is None:
            self._in_flight = False
            if action_button is not None:
                set_button_pending(action_button, False)
                action_button.setEnabled(True)

    def poll_pins(self, names: set[str]) -> None:
        """Poll an arbitrary graph-selected subset of input/output pins."""

        selected = [
            card.wire_value for card in self.cards.values()
            if card.name in names
        ]
        if self._in_flight or not selected:
            return
        self._in_flight = True
        if self.target is None:
            params = {"pins": selected}
        else:
            directions = {self.cards[name].pin_type for name in selected}
            selector = directions.pop() if len(directions) == 1 else "ALL"
            params = {"target": self.target, "name": selector}
        self.sender(self.get_command, params, self._updated)

    def _updated(self, message: dict[str, Any]) -> None:
        self._in_flight = False
        if not message.get("success"):
            return
        self.apply_update(message)
        if self.update_observer is not None:
            self.update_observer(self, message)

    def apply_update(self, message: dict[str, Any]) -> None:
        result = message.get("result", {})
        pins = result.get("pins", [])
        if not pins and result.get("name"):
            pins = [result]
        for pin in pins:
            if not isinstance(pin, dict) or "state" not in pin:
                continue
            wire_name = pin.get("name")
            if wire_name == self.all_value:
                try:
                    state = int(pin["state"])
                except (TypeError, ValueError):
                    continue
                for card in self.cards.values():
                    if card.pin_type == "OUT":
                        card.set_state(state)
                continue
            card = self._cards_by_wire.get(wire_name)
            if card is not None:
                try:
                    state = int(pin["state"])
                except (TypeError, ValueError):
                    continue
                card.set_state(state)


class CommandWidget:
    """Base contract for UI handlers that consume whole commands."""

    def __init__(self, window: "MainWindow",
                 descriptors: list[dict[str, Any]]) -> None:
        self.window = window
        self.descriptors = descriptors

    @classmethod
    def validate_descriptors(
            cls, descriptors: list[dict[str, Any]]) -> None:
        if not descriptors:
            raise WidgetCompatibilityError("список команд пуст")

    def handled_command_names(self) -> set[str]:
        return {str(descriptor["cmd"]) for descriptor in self.descriptors}

    def hidden_command_names(self) -> set[str]:
        return self.handled_command_names()

    def create_widget(self, _descriptor: dict[str, Any]) -> QWidget | None:
        """Create the command's content for one normal tab/group placement."""
        return None

    def build(self) -> None:
        raise NotImplementedError

    def poll(self, _period_ms: int) -> None:
        return


class SpecialGpioCommandWidget(CommandWidget):
    """Renders GPIO commands inside the normal descriptor-driven layout."""

    @staticmethod
    def _field_contains_name(field: dict[str, Any], name: str) -> bool:
        if field.get("name") == name:
            return True
        items = field.get("items")
        if (
            isinstance(items, dict)
            and SpecialGpioCommandWidget._field_contains_name(items, name)
        ):
            return True
        return any(
            isinstance(child, dict)
            and SpecialGpioCommandWidget._field_contains_name(child, name)
            for child in (field.get("fields") or [])
        )

    @classmethod
    def _split_descriptors(
            cls, descriptors: list[dict[str, Any]],
    ) -> tuple[list[dict[str, Any]], dict[str, Any]]:
        set_commands = [
            descriptor for descriptor in descriptors
            if any(
                cls._field_contains_name(field, "state")
                for field in descriptor.get("params", [])
            )
        ]
        get_commands = [
            descriptor for descriptor in descriptors
            if descriptor not in set_commands
        ]
        if not get_commands or len(set_commands) != 1:
            raise WidgetCompatibilityError(
                "special_gpio ожидает хотя бы одну команду чтения и ровно "
                "одну команду записи; роли определяются по параметру state"
            )
        return get_commands, set_commands[0]

    @classmethod
    def validate_descriptors(
            cls, descriptors: list[dict[str, Any]]) -> None:
        super().validate_descriptors(descriptors)
        get_descriptors, _ = cls._split_descriptors(descriptors)
        for get_descriptor in get_descriptors:
            names = {
                str(field.get("name"))
                for field in get_descriptor.get("params", [])
            }
            if "pins" not in names and not {"target", "name"}.issubset(names):
                raise WidgetCompatibilityError(
                    f"команда чтения {get_descriptor.get('cmd', '?')} должна "
                    "принимать pins либо пару target/name"
                )

    def __init__(self, window: "MainWindow",
                 descriptors: list[dict[str, Any]]) -> None:
        super().__init__(window, descriptors)
        self.get_descriptors, self.set_descriptor = self._split_descriptors(
            descriptors
        )
        self.set_command = str(self.set_descriptor["cmd"])
        self.panels: list[IOPanel] = []
        self.hosts: dict[str, list[QVBoxLayout]] = defaultdict(list)
        self._poll_slot: int | None = None

    def hidden_command_names(self) -> set[str]:
        # Read commands keep their normal tab/group/order placement.  The write
        # command is an implementation detail of their special GPIO control.
        return {self.set_command}

    def create_widget(self, descriptor: dict[str, Any]) -> QWidget | None:
        command = str(descriptor.get("cmd", ""))
        if not any(str(item.get("cmd", "")) == command
                   for item in self.get_descriptors):
            return None

        host = QGroupBox()
        host.setObjectName("commandForm")
        host.setProperty("gridSpanMode", "full")
        layout = QVBoxLayout(host)
        layout.setSpacing(COMMAND_FORM_SPACING)

        title = QLabel(str(descriptor.get("title") or command))
        title.setObjectName("commandTitle")
        title_font = title.font()
        title_font.setBold(True)
        title.setFont(title_font)
        layout.addWidget(title)

        description = descriptor.get("description")
        if isinstance(description, str) and description:
            description_label = QLabel(description)
            description_label.setObjectName("commandDescription")
            description_label.setWordWrap(True)
            layout.addWidget(description_label)
        layout.addSpacing(COMMAND_SECTION_SPACING)

        content = QVBoxLayout()
        content.setContentsMargins(0, 0, 0, 0)
        layout.addLayout(content)
        self.hosts[command].append(content)
        return host

    def build(self) -> None:
        for descriptor in self.get_descriptors:
            names = {
                str(field.get("name"))
                for field in descriptor.get("params", [])
            }
            if "pins" in names:
                self.window.send_request(
                    str(descriptor["cmd"]),
                    {"pins": []},
                    lambda message, item=descriptor: self._pins_received(
                        item, message
                    ),
                    timeout=DISCOVERY_TIMEOUT_S,
                )
            else:
                self.window.send_request(
                    "TARGET_LIST",
                    callback=lambda message, item=descriptor: (
                        self._targets_received(item, message)
                    ),
                    timeout=DISCOVERY_TIMEOUT_S,
                )

    def _make_panel(self, descriptor: dict[str, Any],
                    pins: list[dict[str, Any]],
                    target: str | None = None) -> IOPanel:
        normalized = self._decode_pin_enums(descriptor, pins)
        all_value = self._all_pin_value()
        return IOPanel(
            normalized,
            self.window.send_request,
            target,
            get_command=str(descriptor["cmd"]),
            set_command=self.set_command,
            all_value=all_value,
            update_observer=self._panel_updated,
            use_internal_scroll=False,
        )

    @staticmethod
    def _enum_titles(value: Any, field_name: str) -> dict[Any, str]:
        if isinstance(value, dict):
            if value.get("name") == field_name and value.get("type") == "enum":
                return {
                    item.get("value"): str(item.get("title", item.get("value", "")))
                    for item in value.get("constraints", {}).get("values", [])
                    if isinstance(item, dict) and "value" in item
                }
            for child in value.values():
                found = SpecialGpioCommandWidget._enum_titles(child, field_name)
                if found:
                    return found
        elif isinstance(value, list):
            for child in value:
                found = SpecialGpioCommandWidget._enum_titles(child, field_name)
                if found:
                    return found
        return {}

    def _decode_pin_enums(self, descriptor: dict[str, Any],
                          pins: list[dict[str, Any]]) -> list[dict[str, Any]]:
        names = self._enum_titles(descriptor.get("result", []), "name")
        types = self._enum_titles(descriptor.get("result", []), "type")
        decoded: list[dict[str, Any]] = []
        for pin in pins:
            if not isinstance(pin, dict):
                continue
            item = dict(pin)
            raw_name = item.get("name")
            raw_type = item.get("type")
            item["_wire_name"] = raw_name
            item["name"] = names.get(raw_name, raw_name)
            item["type"] = types.get(raw_type, raw_type)
            decoded.append(item)
        return decoded

    def _all_pin_value(self) -> Any:
        values = self._enum_titles(
            self.set_descriptor.get("params", []), "name")
        return next(iter(values), "ALL")

    def _pins_received(self, descriptor: dict[str, Any],
                       message: dict[str, Any]) -> None:
        pins = self._result_pins(message)
        if pins is None:
            return
        self._add_panels(descriptor, pins)

    @staticmethod
    def _result_pins(
            message: dict[str, Any],
    ) -> list[dict[str, Any]] | None:
        if not message.get("success"):
            return None
        pins = message.get("result", {}).get("pins", [])
        return pins if isinstance(pins, list) else None

    def _add_panels(self, descriptor: dict[str, Any],
                    pins: list[dict[str, Any]],
                    target: str | None = None) -> None:
        command = str(descriptor["cmd"])
        for content in self.hosts.get(command, []):
            panel = self._make_panel(descriptor, pins, target)
            self.window._apply_button_cursors(panel)
            if target is None:
                content.addWidget(panel)
            else:
                target_box = QGroupBox(target)
                target_layout = QVBoxLayout(target_box)
                target_layout.addWidget(panel)
                content.addWidget(target_box)
            self.panels.append(panel)
            self.window.io_panels.append(panel)
            if self.window.io_panel is None:
                self.window.io_panel = panel
        self.window._sync_graph_gpio_inputs()

    def _panel_updated(self, source: IOPanel,
                       message: dict[str, Any]) -> None:
        for panel in self.panels:
            if (
                panel is not source
                and panel.target == source.target
                and panel.get_command == source.get_command
            ):
                panel.apply_update(message)
        self.window._gpio_panel_updated(source, message)

    def _targets_received(self, descriptor: dict[str, Any],
                          message: dict[str, Any]) -> None:
        if not message.get("success"):
            return
        targets = message.get("result", {}).get("targets", [])
        pending_targets = [
            str(target_info.get("name", ""))
            for target_info in (targets if isinstance(targets, list) else [])
            if target_info.get("available", False)
            and str(target_info.get("name", ""))
        ]

        def request_next() -> None:
            if not pending_targets:
                return
            target = pending_targets.pop(0)

            def received(response: dict[str, Any]) -> None:
                self._target_pins_received(descriptor, target, response)
                request_next()

            self.window.send_request(
                str(descriptor["cmd"]),
                {"target": target, "name": "ALL"},
                received,
            )

        request_next()

    def _target_pins_received(self, descriptor: dict[str, Any], target: str,
                              message: dict[str, Any]) -> None:
        pins = self._result_pins(message)
        if pins is None:
            return
        self._add_panels(descriptor, pins, target)

    def poll(self, period_ms: int) -> None:
        slot = int(time.monotonic() * 1000) // period_ms
        if self._poll_slot == slot:
            return
        self._poll_slot = slot
        polled_targets: set[tuple[str, str | None]] = set()
        for panel in self.panels:
            key = (panel.get_command, panel.target)
            if key in polled_targets:
                continue
            polled_targets.add(key)
            panel.poll_inputs()


class SpecialPwmCommandWidget(CommandWidget):
    """Compact channel/duty/period control for the unified PWM command."""

    @classmethod
    def validate_descriptors(
            cls, descriptors: list[dict[str, Any]]) -> None:
        super().validate_descriptors(descriptors)
        if len(descriptors) != 1:
            raise WidgetCompatibilityError(
                "special_pwm ожидает ровно одну команду"
            )
        fields = {
            str(field.get("name")): field
            for field in descriptors[0].get("params", [])
        }
        if set(fields) != {"channel", "duty_cycle", "period_counter"}:
            raise WidgetCompatibilityError(
                "special_pwm требует параметры channel, duty_cycle и period_counter"
            )
        if fields["channel"].get("type") != "enum":
            raise WidgetCompatibilityError(
                "параметр channel special_pwm должен иметь тип enum"
            )
        for name in ("duty_cycle", "period_counter"):
            if fields[name].get("type") != "integer":
                raise WidgetCompatibilityError(
                    f"параметр {name} special_pwm должен иметь тип integer"
                )

    def hidden_command_names(self) -> set[str]:
        return set()

    @staticmethod
    def _integer_range(field: dict[str, Any]) -> tuple[int, int, int]:
        constraints = field.get("constraints", {})
        minimum = int(constraints.get("minimum", -2147483648))
        maximum = int(constraints.get("maximum", 2147483647))
        default = int(field.get("default", minimum))
        return minimum, maximum, default

    def create_widget(self, descriptor: dict[str, Any]) -> QWidget | None:
        fields = {
            str(field["name"]): field
            for field in descriptor.get("params", [])
        }
        host = QGroupBox()
        host.setObjectName("commandForm")
        host.setProperty("gridSpanMode", "full")
        layout = QVBoxLayout(host)
        layout.setSpacing(COMMAND_FORM_SPACING)

        title = QLabel(str(descriptor.get("title") or descriptor["cmd"]))
        title.setObjectName("commandTitle")
        font = title.font()
        font.setBold(True)
        title.setFont(font)
        layout.addWidget(title)

        channel_field = fields["channel"]
        duty_min, duty_max, duty_default = self._integer_range(
            fields["duty_cycle"]
        )
        period_min, period_max, period_default = self._integer_range(
            fields["period_counter"]
        )
        period_label = str(
            fields["period_counter"].get("label")
            or "Период счётчика (такты таймера)"
        )
        channels = channel_field.get("constraints", {}).get("values", [])
        for index, item in enumerate(channels):
            raw_channel = item.get("value")
            if not isinstance(raw_channel, str) or not raw_channel:
                continue
            row = QWidget()
            row.setObjectName("pwmChannelRow")
            row_layout = QHBoxLayout(row)
            row_layout.setContentsMargins(0, 0, 0, 0)
            row_layout.setSpacing(8)

            channel_name = QLabel(str(item.get("title") or raw_channel))
            channel_name.setObjectName("pwmChannelName")
            channel_name.setMinimumWidth(120)
            row_layout.addWidget(channel_name)
            row_layout.addWidget(QLabel("Скважность, %:"))

            duty_slider = QSlider(Qt.Horizontal)
            duty_slider.setObjectName("pwmDutySlider")
            duty_slider.setRange(duty_min, duty_max)
            duty_slider.setValue(duty_default)
            duty_slider.setMinimumWidth(140)
            duty_spin = QSpinBox()
            duty_spin.setObjectName("pwmDutyValue")
            duty_spin.setRange(duty_min, duty_max)
            duty_spin.setValue(duty_default)
            duty_spin.setSuffix(" %")
            duty_slider.valueChanged.connect(duty_spin.setValue)
            duty_spin.valueChanged.connect(duty_slider.setValue)
            row_layout.addWidget(duty_slider, 1)
            row_layout.addWidget(duty_spin)

            row_layout.addWidget(QLabel(f"{period_label}:"))
            period = QSpinBox()
            period.setObjectName("pwmPeriodValue")
            period.setRange(period_min, period_max)
            period.setValue(period_default)
            period.setMinimumWidth(100)
            row_layout.addWidget(period)

            button = QPushButton("Применить")
            button.setObjectName("pwmApplyButton")

            def submit(
                    _checked: bool = False, *, channel: str = raw_channel,
                    duty_editor: QSpinBox = duty_spin,
                     period_editor: QSpinBox = period,
                     apply_button: QPushButton = button) -> None:
                set_button_pending(apply_button, True)
                apply_button.setEnabled(False)

                def finished(_message: dict[str, Any]) -> None:
                    set_button_pending(apply_button, False)
                    apply_button.setEnabled(True)

                transaction = self.window.send_request(
                    str(descriptor["cmd"]),
                    {
                        "channel": channel,
                        "duty_cycle": duty_editor.value(),
                        "period_counter": period_editor.value(),
                    },
                    finished,
                )
                if transaction is None:
                    set_button_pending(apply_button, False)
                    apply_button.setEnabled(True)

            button.clicked.connect(submit)
            row_layout.addWidget(button)
            layout.addWidget(row)
        return host

    def build(self) -> None:
        return


COMMAND_WIDGETS: dict[str, type[CommandWidget]] = {
    "special_gpio": SpecialGpioCommandWidget,
    "special_pwm": SpecialPwmCommandWidget,
}


class CommandForm(QGroupBox):
    def __init__(self, descriptor: dict[str, Any], sender: Callable,
                 warning_sink: Callable[[str], None] | None = None):
        title = str(descriptor.get("title") or descriptor.get("cmd"))
        description = descriptor.get("description")
        super().__init__()
        self.setObjectName("commandForm")
        self.descriptor = descriptor
        self.command = str(descriptor["cmd"])
        self.sender = sender
        self.warning_sink = warning_sink
        self.param_widgets: dict[str, QWidget] = {}
        self.result_widgets: dict[str, QWidget] = {}
        self.in_flight = False
        self.last_poll = 0.0
        self.global_autopoll_enabled = False

        layout = QVBoxLayout(self)
        layout.setSpacing(COMMAND_FORM_SPACING)
        self.command_title_label = QLabel(title)
        self.command_title_label.setObjectName("commandTitle")
        title_font = self.command_title_label.font()
        title_font.setBold(True)
        self.command_title_label.setFont(title_font)
        layout.addWidget(self.command_title_label)
        self.description_label: QLabel | None = None
        if isinstance(description, str) and description:
            self.description_label = QLabel(description)
            self.description_label.setObjectName("commandDescription")
            self.description_label.setWordWrap(True)
            layout.addWidget(self.description_label)
        layout.addSpacing(COMMAND_SECTION_SPACING)

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
                if field.get("type") == "console_string":
                    continue
                output = self._make_result_widget(field)
                self.result_widgets[str(field["name"])] = output
                result_label: str | QWidget = str(
                    field.get("label") or field["name"]
                )
                if isinstance(output, ResultWidget) and output.bold_label:
                    label_widget = QLabel(result_label)
                    label_font = label_widget.font()
                    label_font.setBold(True)
                    label_widget.setFont(label_font)
                    label_widget.setAlignment(Qt.AlignLeft | Qt.AlignTop)
                    result_label = label_widget
                results.addRow(result_label, output)
            layout.addLayout(results)

        layout.addSpacing(COMMAND_EXECUTE_TOP_SPACING)
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
        set_button_pending(self.execute_button, self.in_flight)
        self.execute_button.setEnabled(not self.in_flight and not auto_active)

    def _warn_widget_fallback(self, registry: str, hint: str,
                              reason: str) -> None:
        if self.warning_sink is not None:
            self.warning_sink(
                f"Виджет {registry} '{hint}' для команды {self.command} "
                f"отклонён: {reason}. Используется стандартное отображение."
            )

    def _make_param_widget(self, field: dict[str, Any]) -> QWidget:
        hint = field.get("widget_hint")
        if isinstance(hint, str) and hint:
            widget_class = PARAM_WIDGETS.get(hint)
            if widget_class is None:
                self._warn_widget_fallback(
                    "PARAM_WIDGETS", hint, "класс не зарегистрирован"
                )
            elif (
                not isinstance(widget_class, type)
                or not issubclass(widget_class, ParameterWidget)
            ):
                self._warn_widget_fallback(
                    "PARAM_WIDGETS", hint, "класс не наследует ParameterWidget"
                )
            else:
                try:
                    widget_class.validate_descriptor(field)
                    return widget_class(field)
                except WidgetCompatibilityError as exc:
                    self._warn_widget_fallback("PARAM_WIDGETS", hint, str(exc))
        return self._make_default_param_widget(field)

    @staticmethod
    def _make_default_param_widget(field: dict[str, Any]) -> QWidget:
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

    def _make_result_widget(self, field: dict[str, Any]) -> QWidget:
        hint = field.get("widget_hint")
        if isinstance(hint, str) and hint:
            widget_class = RESULT_WIDGETS.get(hint)
            if widget_class is None:
                self._warn_widget_fallback(
                    "RESULT_WIDGETS", hint, "класс не зарегистрирован"
                )
            elif (
                not isinstance(widget_class, type)
                or not issubclass(widget_class, ResultWidget)
            ):
                self._warn_widget_fallback(
                    "RESULT_WIDGETS", hint, "класс не наследует ResultWidget"
                )
            else:
                try:
                    widget_class.validate_descriptor(field)
                    return widget_class(field)
                except WidgetCompatibilityError as exc:
                    self._warn_widget_fallback("RESULT_WIDGETS", hint, str(exc))
        return self._make_default_result_widget(field)

    @staticmethod
    def _make_default_result_widget(field: dict[str, Any]) -> QWidget:
        field_type = field.get("type")
        if field_type == "string":
            output = QLabel()
            output.setTextInteractionFlags(Qt.TextSelectableByMouse)
            output.setWordWrap(True)
            output.setAutoFillBackground(False)
            output.setStyleSheet(
                "QLabel { border: none; background: transparent; }"
            )
            return output
        if field_type == "integer":
            output = ResultIntLabel()
            output.setTextInteractionFlags(Qt.TextSelectableByMouse)
            output.setWordWrap(True)
            output.setAutoFillBackground(False)
            output.setStyleSheet(
                "QLabel { border: none; background: transparent; }"
            )
            return output
        if field_type == "boolean":
            return ResultBoolLabel()
        if field_type == "enum":
            return ResultEnumLabel(field)
        output = QLineEdit()
        output.setReadOnly(True)
        return output

    def parameters(self) -> dict[str, Any]:
        values: dict[str, Any] = {}
        fields = {str(field["name"]): field for field in self.descriptor.get("params", [])}
        for name, widget in self.param_widgets.items():
            field = fields[name]
            if isinstance(widget, ParameterWidget):
                values[name] = widget.value()
            elif isinstance(widget, QSpinBox):
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

    def _submit(self, show_validation_error: bool) -> int | None:
        try:
            params = self.parameters()
        except ValueError as exc:
            if show_validation_error:
                QMessageBox.warning(self, "Параметры", str(exc))
            return None
        self._preserve_scroll_position()
        self.in_flight = True
        self._update_execute_button()
        transaction = self.sender(self.command, params, self.handle_response)
        if transaction is None:
            self.in_flight = False
            self._update_execute_button()
        return transaction

    def execute(self) -> None:
        self._submit(show_validation_error=True)

    def execute_for_autopoll(self) -> int | None:
        """Submit current form values without recurring validation dialogs."""

        return self._submit(show_validation_error=False)

    def handle_response(self, message: dict[str, Any]) -> None:
        self._preserve_scroll_position()
        self.in_flight = False
        self._update_execute_button()
        if not message.get("success"):
            return
        result = message.get("result", {})
        for name, widget in self.result_widgets.items():
            value = result.get(name, "")

            if isinstance(widget, ResultWidget):
                widget.setValue(value)
                continue

            if isinstance(widget, ResultBoolLabel):
                widget.setValue(value)
                continue

            if isinstance(widget, ResultEnumLabel):
                widget.setValue(value)
                continue

            widget.setText(
                json.dumps(value, ensure_ascii=False)
                if isinstance(value, (dict, list))
                else str(value)
            )

    def _preserve_scroll_position(self) -> None:
        parent = self.parentWidget()
        while parent is not None and not isinstance(parent, QScrollArea):
            parent = parent.parentWidget()
        if not isinstance(parent, QScrollArea):
            return
        scroll_bar = parent.verticalScrollBar()
        position = scroll_bar.value()
        QTimer.singleShot(
            0,
            lambda bar=scroll_bar, value=position: bar.setValue(
                min(value, bar.maximum())
            ),
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
    response_received = Signal(str, dict)

    def __init__(
        self,
        worker_factory: Callable[..., SerialWorker] = SerialWorker,
        profile_store: DeviceProfileStore | None = None,
    ):
        super().__init__()
        self.setWindowTitle("StarSet - Control Panel")
        self.resize(1450, 850)
        self.worker: SerialWorker | None = None
        self._worker_factory = worker_factory
        self.next_id = 1
        self.pending: dict[int, tuple[str, Callable | None, float]] = {}
        self._heartbeat_transaction: int | None = None
        self._heartbeat_misses = 0
        self.descriptors: dict[str, dict[str, Any]] = {}
        self.forms: list[CommandForm] = []
        self.command_widgets: list[CommandWidget] = []
        self.command_widget_by_command: dict[str, CommandWidget] = {}
        self.io_panel: IOPanel | None = None
        self.io_panels: list[IOPanel] = []
        self.graph_window: GrathPlotWindow | None = None
        self.profile_store = profile_store or DeviceProfileStore()
        self._adc_profile_widgets: dict[
            str, list[SpecialAdcResultWidget | SpecialAdcGroupResultWidget]
        ] = defaultdict(list)
        self._applying_profile = False
        self.raw_history: list[str] = []
        self.raw_history_index = 0
        self._session_tx_bytes = 0
        self._session_rx_bytes = 0
        self._connected_at: float | None = None
        self._disconnected_elapsed = 0.0
        self._build_ui()
        application = QApplication.instance()
        if application is not None:
            application.installEventFilter(self)
        self._refresh_ports()

        self.timeout_timer = QTimer(self)
        self.timeout_timer.timeout.connect(self._expire_requests)
        self.timeout_timer.start(250)
        self.poll_timer = QTimer(self)
        self.poll_timer.timeout.connect(self._poll)
        self.poll_timer.start(50)
        self.heartbeat_timer = QTimer(self)
        self.heartbeat_timer.setInterval(HEARTBEAT_INTERVAL_MS)
        self.heartbeat_timer.timeout.connect(self._heartbeat_tick)
        self.connection_stats_timer = QTimer(self)
        self.connection_stats_timer.setInterval(1000)
        self.connection_stats_timer.timeout.connect(self._update_connection_stats)
        self.profile_save_timer = QTimer(self)
        self.profile_save_timer.setSingleShot(True)
        self.profile_save_timer.setInterval(350)
        self.profile_save_timer.timeout.connect(self.profile_store.flush)

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
        self.refresh_ports_button = QToolButton()
        self.refresh_ports_button.setObjectName("utilityButton")
        self.refresh_ports_button.setIcon(
            self.style().standardIcon(QStyle.SP_BrowserReload)
        )
        self.refresh_ports_button.setToolTip("Обновить список портов")
        self.refresh_ports_button.clicked.connect(self._refresh_ports)
        connection.addWidget(self.refresh_ports_button)
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
        self.graph_button = QPushButton("Графики")
        self.graph_button.setEnabled(False)
        self.graph_button.clicked.connect(self._show_graph_plot)
        connection.addWidget(self.graph_button)
        self.theme_button = QPushButton()
        self.theme_button.clicked.connect(self._toggle_theme)
        connection.addWidget(self.theme_button)
        self._update_theme_button()
        self.device_label = QLabel("МК: -")
        connection.addWidget(self.device_label, 1)
        root.addLayout(connection)

        splitter = QSplitter(Qt.Horizontal)
        self.tabs = QTabWidget()
        self.tabs.tabBar().setMouseTracking(True)
        splitter.addWidget(self.tabs)
        splitter.addWidget(self._build_terminal())
        splitter.setSizes([900, 550])
        root.addWidget(splitter, 1)
        self.setCentralWidget(central)
        self._reset_tabs()
        self._apply_button_cursors(central)

    def _build_terminal(self) -> QWidget:
        panel = QWidget()
        layout = QVBoxLayout(panel)
        header = QHBoxLayout()
        title = QLabel("Терминал")
        title.setStyleSheet("font-weight:bold")
        header.addWidget(title)
        header.addStretch()
        self.clear_terminal_button = QToolButton()
        self.clear_terminal_button.setObjectName("utilityButton")
        self.clear_terminal_button.setIcon(
            self.style().standardIcon(QStyle.SP_DialogResetButton)
        )
        self.clear_terminal_button.setToolTip("Очистить терминал")
        self.clear_terminal_button.clicked.connect(lambda: self.terminal.clear())
        header.addWidget(self.clear_terminal_button)
        self.developer_check = QCheckBox("Режим разработчика")
        self.developer_check.toggled.connect(self._toggle_developer)
        header.addWidget(self.developer_check)
        layout.addLayout(header)

        self.developer_stats_label = QLabel()
        self.developer_stats_label.setObjectName("developerStats")
        self.developer_stats_label.setTextInteractionFlags(Qt.TextSelectableByMouse)
        self.developer_stats_label.hide()
        layout.addWidget(self.developer_stats_label)
        self._update_connection_stats()

        self.terminal = QPlainTextEdit()
        self.terminal.setObjectName("terminal")
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
        if isinstance(watched, QAbstractButton) and event.type() == QEvent.Polish:
            watched.setCursor(Qt.PointingHandCursor)
        if (
            isinstance(watched, (QAbstractSpinBox, QComboBox, QSlider))
            and event.type() == QEvent.Wheel
        ):
            parent = watched.parentWidget()
            while parent is not None and not isinstance(parent, QScrollArea):
                parent = parent.parentWidget()
            if isinstance(parent, QScrollArea):
                scroll_bar = parent.verticalScrollBar()
                pixel_delta = event.pixelDelta().y()
                if pixel_delta:
                    amount = pixel_delta
                else:
                    steps = event.angleDelta().y() / 120.0
                    amount = round(
                        steps
                        * QApplication.wheelScrollLines()
                        * scroll_bar.singleStep()
                    )
                if event.inverted():
                    amount = -amount
                scroll_bar.setValue(scroll_bar.value() - amount)
            # Колесо навигирует по странице и никогда не редактирует поле.
            return True
        if watched is self.tabs.tabBar():
            if event.type() == QEvent.MouseMove:
                tab_bar = self.tabs.tabBar()
                index = tab_bar.tabAt(event.position().toPoint())
                is_inactive = (
                    index >= 0
                    and index != tab_bar.currentIndex()
                    and tab_bar.isTabEnabled(index)
                )
                tab_bar.setCursor(
                    Qt.PointingHandCursor if is_inactive else Qt.ArrowCursor
                )
            elif event.type() == QEvent.Leave:
                self.tabs.tabBar().setCursor(Qt.ArrowCursor)
            return False
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

    @staticmethod
    def _apply_button_cursors(root: QWidget) -> None:
        for button in root.findChildren(QAbstractButton):
            button.setCursor(Qt.PointingHandCursor)

    def _update_theme_button(self) -> None:
        light_is_next = _active_theme == "dark"
        self.theme_button.setText(
            "☀ Светлая тема" if light_is_next else "☾ Тёмная тема"
        )
        self.theme_button.setToolTip(
            "Переключить на светлую тему"
            if light_is_next
            else "Переключить на тёмную тему"
        )

    def _toggle_theme(self) -> None:
        set_theme("light" if _active_theme == "dark" else "dark")
        application = QApplication.instance()
        if application is not None:
            application.setPalette(application_palette())
            application.setStyleSheet(application_stylesheet())
        self._update_theme_button()
        self._update_connect_button_style()
        for panel in self.io_panels:
            for card in panel.cards.values():
                card.set_state(card.state)
        for result in self.findChildren(ResultBoolLabel):
            if result.text() == "SUCCESS":
                result.setValue(True)
            elif result.text() == "FAIL":
                result.setValue(False)
        if self.graph_window is not None:
            self.graph_window.update()
        self.update()

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
        if hasattr(self.worker, "bytes_received"):
            self.worker.bytes_received.connect(self._record_received_bytes)
        if hasattr(self.worker, "bytes_sent"):
            self.worker.bytes_sent.connect(self._record_sent_bytes)
        if hasattr(self.worker, "frame_error"):
            self.worker.frame_error.connect(
                lambda error: self._append_terminal(
                    f"Krul transport frame: {error}", "warning"))
        self.connect_button.setText("Подключение…")
        self.connect_button.setEnabled(False)
        self.connect_button.setStyleSheet("")
        self.format_combo.setEnabled(False)
        self._session_tx_bytes = 0
        self._session_rx_bytes = 0
        self._connected_at = None
        self._disconnected_elapsed = 0.0
        self._update_connection_stats()
        self.worker.start()

    def _serial_opened(self) -> None:
        self._connected_at = time.monotonic()
        self._disconnected_elapsed = 0.0
        self.connection_stats_timer.start()
        self._update_connection_stats()
        self.connect_button.setText("Отключить")
        self.connect_button.setEnabled(True)
        self._update_connect_button_style()
        self.rebuild_button.setEnabled(True)
        self._append_terminal("Последовательный порт открыт", "info")
        QTimer.singleShot(CONNECT_SETTLE_MS, self._discover)

    def _update_connect_button_style(self) -> None:
        if self.connect_button.text() != "Отключить":
            self.connect_button.setStyleSheet("")
            return
        self.connect_button.setStyleSheet(f"""
            QPushButton {{
                background-color: {theme_color("danger")};
                color: {theme_color("accent_text")};
            }}
            QPushButton:hover {{
                background-color: {theme_color("danger_hover")};
            }}
            QPushButton:pressed {{
                background-color: {theme_color("danger_pressed")};
            }}
        """)

    def _open_failed(self, error: str) -> None:
        QMessageBox.critical(self, "Ошибка порта", error)
        self._disconnect(from_worker=True)

    def _transport_failed(self, error: str) -> None:
        QMessageBox.critical(self, "Ошибка связи", error)
        self._disconnect(from_worker=True)

    def _disconnect(self, from_worker: bool = False) -> None:
        if self._connected_at is not None:
            self._disconnected_elapsed = max(
                0.0, time.monotonic() - self._connected_at)
        self._connected_at = None
        self.connection_stats_timer.stop()
        self._update_connection_stats()
        self.heartbeat_timer.stop()
        self._heartbeat_transaction = None
        self._heartbeat_misses = 0
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
        self.command_widgets.clear()
        self.command_widget_by_command.clear()
        self.io_panel = None
        self.io_panels.clear()
        self.device_label.setText("МК: -")
        self.connect_button.setText("Подключить")
        self.connect_button.setEnabled(True)
        self.connect_button.setStyleSheet("")
        self.rebuild_button.setEnabled(False)
        self.graph_button.setEnabled(False)
        self.format_combo.setEnabled(True)
        if self.graph_window is not None:
            self.graph_window.set_descriptors({})
        self._reset_tabs()

    def closeEvent(self, event) -> None:  # noqa: N802
        if self.graph_window is not None:
            self.graph_window.close()
        self.profile_save_timer.stop()
        self.profile_store.flush()
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
        command = pending[0] if pending else None
        if not message.get("success", False):
            error = message.get("error", {})
            self._append_terminal(
                f"Ошибка {error.get('code', '?')}: {error.get('message', 'Без описания')}",
                "error",
            )
        else:
            descriptor = self.descriptors.get(command or "", {})
            result = message.get("result", {})
            for field in descriptor.get("result", []):
                if field.get("type") == "console_string" and field.get("name") in result:
                    constraints = field.get("constraints", {})
                    self._append_terminal(str(result[field["name"]]),
                                          str(constraints.get("severity", "info")))
        if command is not None:
            self._mark_command_activity(command)
            self.response_received.emit(command, message)
            self._deliver_command_response(command, message, pending[1])

    def _deliver_command_response(
            self, command: str, message: dict[str, Any],
            callback: Callable | None) -> None:
        """Deliver one correlated response to every visible command copy."""

        callback_form: CommandForm | None = None
        if (
            callback is not None
            and isinstance(getattr(callback, "__self__", None), CommandForm)
            and getattr(callback, "__func__", None) is CommandForm.handle_response
        ):
            callback_form = callback.__self__

        if callback is not None:
            callback(message)
        for form in self.forms:
            if form.command == command and form is not callback_form:
                form.handle_response(message)

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
            self._mark_command_activity(command)
            self.response_received.emit(command, message)
            self._deliver_command_response(command, message, callback)

    def _start_heartbeat(self) -> None:
        self._heartbeat_transaction = None
        self._heartbeat_misses = 0
        self.heartbeat_timer.start()

    def _heartbeat_tick(self) -> None:
        if self.worker is None:
            self.heartbeat_timer.stop()
            return
        if self._heartbeat_transaction is not None:
            self.pending.pop(self._heartbeat_transaction, None)
            self._heartbeat_transaction = None
            self._heartbeat_misses += 1
            self._append_terminal(
                f"Связь потеряна, попытка переподключиться ({self._heartbeat_misses}/"
                f"{HEARTBEAT_MISS_LIMIT})",
                "warning",
            )
            if self._heartbeat_misses >= HEARTBEAT_MISS_LIMIT:
                self._heartbeat_connection_lost()
                return

        self._heartbeat_transaction = self.send_request(
            "PING",
            callback=self._heartbeat_received,
            timeout=2.0 * HEARTBEAT_INTERVAL_MS / 1000.0,
        )

    def _heartbeat_received(self, message: dict[str, Any]) -> None:
        if message.get("id") != self._heartbeat_transaction:
            return
        self._heartbeat_transaction = None
        if message.get("success"):
            self._heartbeat_misses = 0
            return

        self._heartbeat_misses += 1
        if self._heartbeat_misses >= HEARTBEAT_MISS_LIMIT:
            self._heartbeat_connection_lost()

    def _heartbeat_connection_lost(self) -> None:
        self._append_terminal(
            "Связь потеряна: три запроса подряд остались без ответа",
            "error",
        )
        self._disconnect()
        self.device_label.setText("МК: связь потеряна")

    def _discover(self) -> None:
        if self.worker is None:
            return
        self.heartbeat_timer.stop()
        self._heartbeat_transaction = None
        self._heartbeat_misses = 0
        self.pending.clear()
        self.descriptors.clear()
        self.forms.clear()
        self.command_widgets.clear()
        self.command_widget_by_command.clear()
        self.io_panel = None
        self.io_panels.clear()
        self.graph_button.setEnabled(False)
        if self.graph_window is not None:
            self.graph_window.set_descriptors({})
        self._reset_tabs()
        self.device_label.setText("МК: опрос…")
        self.send_request("WHOAMI", callback=self._whoami_received,
                          timeout=DISCOVERY_TIMEOUT_S)

    def _whoami_received(self, message: dict[str, Any]) -> None:
        if not message.get("success"):
            self.device_label.setText("МК: ошибка WHOAMI")
            return
        result = message.get("result", {})
        if not isinstance(result, dict):
            self.device_label.setText("МК: некорректный WHOAMI")
            return
        version = result.get("protocol_version")
        if version != PROTOCOL_VERSION:
            QMessageBox.critical(
                self, "Версия протокола",
                f"GUI поддерживает версию {PROTOCOL_VERSION}, МК сообщил {version}.",
            )
            return
        self.profile_store.select_device(result)
        self.device_label.setText(
            f"МК: {result.get('device_name', '?')} · FW {result.get('firmware', '?')} · JSON v{version}"
        )
        self._start_heartbeat()
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
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        welcome = QWidget()
        welcome.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Preferred)
        layout = QVBoxLayout(welcome)
        rabbit = QLabel("🌠🌌StarSet🌠")
        rabbit.setObjectName("welcomeTitle")
        rabbit.setAlignment(Qt.AlignCenter)
        rabbit.setStyleSheet(
            """font-size:28px; letter-spacing: 2px;
                margin-top: 10px; font-weight: bold;"""
        )
        layout.addWidget(rabbit)
        self.welcome_hint = QLabel(
            "Подключитесь к контроллеру - остальные вкладки будут построены "
            "по его DESCRIBE-метаданным."
        )
        layout.addWidget(self.welcome_hint, 0, Qt.AlignCenter)
        self.default_commands_layout = QVBoxLayout()
        layout.addLayout(self.default_commands_layout)
        layout.addStretch()
        scroll.setWidget(welcome)
        self.tabs.addTab(scroll, "Главная")

    def _populate_command_groups(
            self, body_layout: QVBoxLayout, descriptors: list[dict[str, Any]]
    ) -> None:
        page_grid = ResponsiveCardGrid(
            COMMAND_GRID_MIN_GROUP_WIDTH,
            COMMAND_GRID_MAX_COLUMNS,
        )
        page_grid.setObjectName("commandGroupsGrid")
        page_grid.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Fixed)
        body_layout.addWidget(page_grid)
        groups: dict[str, tuple[QGroupBox, ResponsiveCardGrid]] = {}

        def add_group_card(group_name: str, card: QWidget) -> None:
            box, command_grid = groups[group_name]
            command_grid.add_card(card)
            preferred_span = min(
                COMMAND_GRID_MAX_COLUMNS,
                max(1, math.ceil(math.sqrt(len(command_grid.cards)))),
            )
            box.setProperty("gridPreferredSpan", preferred_span)
            if card.property("gridSpanMode") == "full":
                box.setProperty("gridSpanMode", "full")
            page_grid.request_relayout()

        for descriptor in sorted(
                descriptors,
                key=lambda item: (int(item.get("order", 0)), str(item.get("cmd", ""))),
        ):
            group_name = str(descriptor.get("group") or "Команды")
            if group_name not in groups:
                box = QGroupBox(group_name)
                box.setObjectName("commandGroup")

                box_layout = QVBoxLayout(box)
                box_layout.setSpacing(COMMAND_GROUP_SPACING)
                command_grid = ResponsiveCardGrid(
                    COMMAND_GRID_MIN_CARD_WIDTH,
                    COMMAND_GRID_MAX_COLUMNS,
                )
                command_grid.setObjectName("commandCardsGrid")
                box_layout.addWidget(command_grid)
                groups[group_name] = (box, command_grid)
                page_grid.add_card(box)
            command = str(descriptor.get("cmd", ""))
            command_widget = self.command_widget_by_command.get(command)
            custom = (
                command_widget.create_widget(descriptor)
                if command_widget is not None
                else None
            )
            if custom is not None:
                self._apply_button_cursors(custom)
                add_group_card(group_name, custom)
                continue

            form = CommandForm(
                descriptor,
                self.send_request,
                lambda message: self._append_terminal(message, "warning"),
            )
            form.set_global_autopoll(self.autopoll_check.isChecked())
            self._apply_button_cursors(form)
            add_group_card(group_name, form)
            if form.auto_enabled is not None:
                peer = next(
                    (
                        existing for existing in self.forms
                        if existing.command == form.command
                        and existing.auto_enabled is not None
                    ),
                    None,
                )
                if peer is not None:
                    form.auto_enabled.setChecked(peer.auto_enabled.isChecked())
                form.auto_enabled.toggled.connect(
                    lambda enabled, source=form: self._sync_command_auto_checks(
                        source, enabled
                    )
                )
            self.forms.append(form)

    def _sync_command_auto_checks(
            self, source: CommandForm, enabled: bool) -> None:
        for form in self.forms:
            if (
                form is not source
                and form.command == source.command
                and form.auto_enabled is not None
            ):
                form.auto_enabled.setChecked(enabled)

    def _build_dynamic_tabs(self) -> None:
        self.welcome_hint.setText(
            "Все доступные команды и панели собраны ниже; тематические "
            "вкладки остаются доступны отдельно."
        )
        hidden_commands = self._prepare_command_widgets()
        normal = [
            descriptor
            for descriptor in self.descriptors.values()
            if not descriptor.get("builtin")
            and not descriptor.get("nogui")
            and str(descriptor.get("cmd")) not in hidden_commands
        ]

        overview: list[dict[str, Any]] = []
        for descriptor in normal:
            item = dict(descriptor)
            tab_name = str(item.get("tab") or "Кролик вампир")
            group_name = str(item.get("group") or "Команды")
            if tab_name != "Кролик вампир":
                item["group"] = f"{tab_name} / {group_name}"
            overview.append(item)
        self._populate_command_groups(self.default_commands_layout, overview)

        by_tab: dict[str, list[dict[str, Any]]] = defaultdict(list)
        for descriptor in normal:
            by_tab[str(descriptor.get("tab") or "Кролик вампир")].append(descriptor)
        by_tab.pop("Кролик вампир", None)

        for tab_name, descriptors in by_tab.items():
            scroll = QScrollArea()
            scroll.setWidgetResizable(True)
            scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
            body = QWidget()
            body.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Preferred)
            body_layout = QVBoxLayout(body)
            self._populate_command_groups(body_layout, descriptors)
            body_layout.addStretch()
            scroll.setWidget(body)
            self.tabs.addTab(scroll, tab_name)

        for widget in self.command_widgets:
            widget.build()
        self._bind_adc_profile_widgets()
        self._sync_graph_plot()

    def _bind_adc_profile_widgets(self) -> None:
        self._adc_profile_widgets.clear()
        stored = self.profile_store.section("widgets")
        for form in self.forms:
            for field_name, widget in form.result_widgets.items():
                if not isinstance(
                    widget,
                    (SpecialAdcResultWidget, SpecialAdcGroupResultWidget),
                ):
                    continue
                key = f"{form.command}:{field_name}"
                self._adc_profile_widgets[key].append(widget)

        self._applying_profile = True
        try:
            for key, widgets in self._adc_profile_widgets.items():
                config = stored.get(key)
                if isinstance(config, dict):
                    for widget in widgets:
                        widget.apply_profile_configuration(config)
        finally:
            self._applying_profile = False

        for key, widgets in self._adc_profile_widgets.items():
            for widget in widgets:
                for control in (
                    widget.reference_voltage,
                    widget.scale_factor,
                    widget.base_voltage,
                    widget.resolution_bits,
                ):
                    control.valueChanged.connect(
                        lambda _value, profile_key=key, source=widget:
                        self._adc_profile_changed(profile_key, source)
                    )

    def _adc_profile_changed(
        self,
        key: str,
        source: SpecialAdcResultWidget | SpecialAdcGroupResultWidget,
    ) -> None:
        if self._applying_profile:
            return
        config = source.profile_configuration()
        self._applying_profile = True
        try:
            for widget in self._adc_profile_widgets.get(key, []):
                if widget is not source:
                    widget.apply_profile_configuration(config)
        finally:
            self._applying_profile = False
        stored = self.profile_store.section("widgets")
        stored[key] = config
        self.profile_store.set_section("widgets", stored)
        self._schedule_profile_save()

    def _schedule_profile_save(self) -> None:
        if self.profile_store.active_key is not None:
            self.profile_save_timer.start()

    def _sync_graph_plot(self) -> None:
        if self.graph_window is not None:
            self.graph_window.set_descriptors(self.descriptors)
        self._sync_graph_gpio_inputs()
        if self.graph_window is not None:
            self.graph_window.apply_profile_configuration(
                self.profile_store.section("graphs")
            )

    @staticmethod
    def _gpio_source_id(panel: IOPanel) -> str:
        target = panel.target if panel.target is not None else "local"
        return f"{panel.get_command}:{target}"

    def _gpio_definitions(self) -> list[dict[str, Any]]:
        definitions: list[dict[str, Any]] = []
        seen: set[tuple[str, str]] = set()
        for panel in self.io_panels:
            source_id = self._gpio_source_id(panel)
            descriptor = self.descriptors.get(panel.get_command, {})
            source_title = str(
                panel.target
                or descriptor.get("title")
                or panel.get_command
            )
            for card in panel.cards.values():
                identity = (source_id, card.name)
                if identity in seen:
                    continue
                seen.add(identity)
                definitions.append({
                    "key": f"GPIO:{source_id}:{card.name}",
                    "source_id": source_id,
                    "pin_name": card.name,
                    "tab": str(descriptor.get("tab") or "GPIO"),
                    "group": str(
                        descriptor.get("group")
                        or ("Входы GPIO" if card.pin_type == "IN" else "Выходы GPIO")
                    ),
                    "source_title": source_title,
                    "label": card.name,
                    "state": card.state,
                })
        return definitions

    def _sync_graph_gpio_inputs(self) -> None:
        definitions = self._gpio_definitions()
        available = bool(definitions or discover_measurements(self.descriptors))
        self.graph_button.setEnabled(available)
        if self.graph_window is not None:
            self.graph_window.register_gpio_inputs(definitions)

    def _request_graph_gpio(
            self, requests: dict[str, set[str]]) -> None:
        for panel in self.io_panels:
            source_id = self._gpio_source_id(panel)
            names = requests.get(source_id, set())
            if not names:
                continue
            if self.autopoll_check.isChecked():
                # Ordinary GPIO auto-poll already supplies every input. Only
                # selected outputs need an additional read in this mode.
                names = {
                    name for name in names
                    if name in panel.cards
                    and panel.cards[name].pin_type == "OUT"
                }
            panel.poll_pins(names)

    def _gpio_panel_updated(
            self, source: IOPanel, message: dict[str, Any]
    ) -> None:
        if self.graph_window is None or not message.get("success"):
            return
        result = message.get("result", {})
        if not isinstance(result, dict):
            return
        pins = result.get("pins", [])
        if not pins and result.get("name"):
            pins = [result]
        states: dict[str, int] = {}
        for pin in pins if isinstance(pins, list) else []:
            if not isinstance(pin, dict) or "state" not in pin:
                continue
            name = str(pin.get("name", ""))
            card = source.cards.get(name)
            if card is None:
                continue
            try:
                states[name] = int(pin["state"])
            except (TypeError, ValueError):
                continue
        if states:
            self.graph_window.ingest_gpio_values(
                self._gpio_source_id(source), states
            )

    def _request_graph_sample(
            self, command: str, params: dict[str, Any] | None
    ) -> int | None:
        if self.worker is None:
            return None
        if self._command_is_pending(command):
            return None
        if self.autopoll_check.isChecked():
            for form in self.forms:
                if form.command != command:
                    continue
                if form.auto_enabled is not None and form.auto_enabled.isChecked():
                    return form.execute_for_autopoll()
                break
        return self.send_request(command, params)

    def _command_is_pending(self, command: str) -> bool:
        return any(
            pending_command == command
            for pending_command, _, _ in self.pending.values()
        )

    def _mark_command_activity(self, command: str) -> None:
        now = time.monotonic()
        for form in self.forms:
            if form.command == command:
                form.last_poll = now

    def _show_graph_plot(self) -> None:
        if self.graph_window is None:
            self.graph_window = GrathPlotWindow(
                self.descriptors,
                requester=self._request_graph_sample,
                gpio_requester=self._request_graph_gpio,
                parent=self,
            )
            self.response_received.connect(self.graph_window.ingest_response)
            self.graph_window.configuration_changed.connect(
                self._graph_profile_changed
            )
        elif self.graph_window.descriptors != self.descriptors:
            self.graph_window.set_descriptors(self.descriptors)
        self._sync_graph_gpio_inputs()
        self.graph_window.apply_profile_configuration(
            self.profile_store.section("graphs")
        )
        self.graph_window.show()
        self.graph_window.raise_()
        self.graph_window.activateWindow()

    def _graph_profile_changed(self, config: dict[str, Any]) -> None:
        self.profile_store.set_section("graphs", config)
        self._schedule_profile_save()

    def _prepare_command_widgets(self) -> set[str]:
        self.command_widgets.clear()
        self.command_widget_by_command.clear()
        grouped: dict[tuple[str, str | None], list[dict[str, Any]]] = defaultdict(list)
        for descriptor in self.descriptors.values():
            hint = descriptor.get("widget_hint")
            if (
                not descriptor.get("builtin")
                and not descriptor.get("nogui")
                and isinstance(hint, str)
                and hint
            ):
                # A GPIO group is also its explicit read/write pairing key.
                # Keep descriptors without a group in one legacy bucket so
                # existing devices with a single PIN_GET/PIN_SET pair continue
                # to work unchanged.  Other command widgets retain their
                # historical grouping by widget hint alone.
                pair_group = (
                    str(descriptor.get("group") or "")
                    if hint == "special_gpio"
                    else None
                )
                grouped[(hint, pair_group)].append(descriptor)

        hidden: set[str] = set()
        for (hint, pair_group), descriptors in grouped.items():
            widget_class = COMMAND_WIDGETS.get(hint)
            group_context = (
                f" (group={pair_group!r})"
                if hint == "special_gpio" and pair_group
                else ""
            )
            if widget_class is None:
                self._append_terminal(
                    f"COMMAND_WIDGETS '{hint}'{group_context} не зарегистрирован; команды "
                    "будут показаны стандартными формами.",
                    "warning",
                )
                continue
            if (
                not isinstance(widget_class, type)
                or not issubclass(widget_class, CommandWidget)
            ):
                self._append_terminal(
                    f"COMMAND_WIDGETS '{hint}' содержит несовместимый класс; "
                    "команды будут показаны стандартными формами.",
                    "warning",
                )
                continue
            try:
                widget_class.validate_descriptors(descriptors)
                widget = widget_class(self, descriptors)
            except WidgetCompatibilityError as exc:
                self._append_terminal(
                    f"COMMAND_WIDGETS '{hint}'{group_context} отклонил "
                    f"дескрипторы: {exc}. "
                    "Команды будут показаны стандартными формами.",
                    "warning",
                )
                continue
            self.command_widgets.append(widget)
            for command in widget.handled_command_names():
                self.command_widget_by_command[command] = widget
            hidden.update(widget.hidden_command_names())
        return hidden

    def _toggle_developer(self, enabled: bool) -> None:
        self.raw_row.setVisible(enabled)
        self.developer_stats_label.setVisible(enabled)
        if enabled:
            self._update_connection_stats()

    def _record_sent_bytes(self, count: int) -> None:
        if count > 0:
            self._session_tx_bytes += count
            self._update_connection_stats()

    def _record_received_bytes(self, count: int) -> None:
        if count > 0:
            self._session_rx_bytes += count
            self._update_connection_stats()

    @staticmethod
    def _format_connection_time(seconds: float) -> str:
        total = max(0, int(seconds))
        hours, remainder = divmod(total, 3600)
        minutes, seconds = divmod(remainder, 60)
        return f"{hours:02d}:{minutes:02d}:{seconds:02d}"

    def _update_connection_stats(self) -> None:
        elapsed = self._disconnected_elapsed
        connected = self._connected_at is not None
        if connected:
            elapsed = max(0.0, time.monotonic() - self._connected_at)
        status = "подключено" if connected else "отключено"
        tx = f"{self._session_tx_bytes:,}".replace(",", " ")
        rx = f"{self._session_rx_bytes:,}".replace(",", " ")
        self.developer_stats_label.setText(
            f"TX: {tx} B · RX: {rx} B · Время: "
            f"{self._format_connection_time(elapsed)} · {status}"
        )

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
            if self._command_is_pending(form.command):
                continue
            form.poll(period)
        for widget in self.command_widgets:
            widget.poll(period)

    def _autopoll_toggled(self, enabled: bool) -> None:
        for form in self.forms:
            form.set_global_autopoll(enabled)

    def _append_terminal(self, text: str, severity: str = "info") -> None:
        colors = {
            "debug": QColor(theme_color("terminal_debug")),
            "info": QColor(theme_color("terminal_info")),
            "warning": QColor(theme_color("terminal_warning")),
            "error": QColor(theme_color("terminal_error")),
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
    app.setApplicationName("ARK JSON Control Panel")
    set_theme(DEFAULT_THEME)
    app.setPalette(application_palette())
    app.setStyleSheet(application_stylesheet())
    window = MainWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
