from __future__ import annotations

import csv
import json
import time

from PySide6.QtCore import Qt

import Starset as gui
from GrathPlot import (
    MAX_BUFFER_BYTES,
    MIN_BUFFER_SECONDS,
    GrathPlotWindow,
    discover_measurements,
)


DESCRIPTORS = {
    "MONITOR": {
        "cmd": "MONITOR",
        "title": "Monitor",
        "tab": "Telemetry",
        "group": "Power",
        "params": [],
        "result": [
            {"name": "voltage", "label": "Voltage", "type": "float"},
            {
                "name": "channels",
                "label": "Channels",
                "type": "object",
                "fields": [
                    {"name": "current", "label": "Current", "type": "integer"},
                    {"name": "caption", "type": "string"},
                ],
            },
        ],
        "autoupdate": {"min_period": 100, "max_period": 1000},
    },
    "MANUAL": {
        "cmd": "MANUAL",
        "group": "Manual",
        "params": [{"name": "channel", "type": "integer"}],
        "result": [{"name": "value", "type": "integer"}],
    },
    "HIDDEN": {
        "cmd": "HIDDEN",
        "nogui": True,
        "result": [{"name": "secret", "type": "integer"}],
    },
}


def test_measurements_are_discovered_recursively_and_grouped() -> None:
    measurements = discover_measurements(DESCRIPTORS)

    assert set(measurements) == {
        "MONITOR:voltage",
        "MONITOR:channels.current",
    }
    assert "MANUAL:value" not in measurements
    nested = measurements["MONITOR:channels.current"]
    assert nested.path == ("channels", "current")
    assert nested.tab == "Telemetry"
    assert nested.group == "Power"
    assert nested.label == "Channels / Current"


def test_time_and_xy_modes_collect_response_values(qtbot) -> None:
    window = GrathPlotWindow(DESCRIPTORS)
    qtbot.addWidget(window)
    voltage = "MONITOR:voltage"
    current = "MONITOR:channels.current"
    window._tree_items[voltage].setCheckState(0, Qt.Checked)
    window._tree_items[current].setCheckState(0, Qt.Checked)

    window.ingest_response(
        "MONITOR",
        {
            "success": True,
            "result": {"voltage": 12.5, "channels": {"current": 3}},
        },
    )
    assert window.canvas.series[voltage][-1][1] == 12.5
    assert window.canvas.series[current][-1][1] == 3.0

    window.mode_combo.setCurrentIndex(window.mode_combo.findData(window.XY_MODE))
    window.x_combo.setCurrentIndex(window.x_combo.findData(voltage))
    window.ingest_response(
        "MONITOR",
        {
            "success": True,
            "result": {"voltage": 13, "channels": {"current": 4}},
        },
    )
    assert window.canvas.series[current][-1] == (13.0, 4.0)
    window._tree_items[current].setCheckState(0, Qt.Unchecked)
    window._tree_items[current].setCheckState(0, Qt.Checked)
    assert window.canvas.series[current][-2:] == [(12.5, 3.0), (13.0, 4.0)]


def test_only_safe_autoupdate_commands_are_polled(qtbot) -> None:
    requests: list[tuple[str, dict | None]] = []

    def requester(command: str, params: dict | None) -> int:
        requests.append((command, params))
        return len(requests)

    window = GrathPlotWindow(DESCRIPTORS, requester=requester)
    qtbot.addWidget(window)
    window._tree_items["MONITOR:voltage"].setCheckState(0, Qt.Checked)

    window._poll_sources()

    assert requests == [("MONITOR", None)]
    assert "MONITOR" in window.in_flight


def test_points_and_line_style_can_be_changed_without_clearing_data(qtbot) -> None:
    window = GrathPlotWindow(DESCRIPTORS)
    qtbot.addWidget(window)
    key = "MONITOR:voltage"
    window._tree_items[key].setCheckState(0, Qt.Checked)
    window.ingest_response(
        "MONITOR", {"success": True, "result": {"voltage": 12.5}}
    )
    points_before = list(window.canvas.series[key])

    window.points_check.setChecked(True)
    window.line_style_combo.setCurrentIndex(
        window.line_style_combo.findData("dash_dot")
    )

    assert window.canvas.show_points is True
    assert window.canvas.line_style == "dash_dot"
    assert window.canvas.series[key] == points_before


def test_series_keep_color_when_other_measurements_are_toggled(qtbot) -> None:
    window = GrathPlotWindow(DESCRIPTORS)
    qtbot.addWidget(window)
    voltage = "MONITOR:voltage"
    current = "MONITOR:channels.current"

    window._tree_items[voltage].setCheckState(0, Qt.Checked)
    original_color = window.canvas.series_color(voltage).name()
    window._tree_items[current].setCheckState(0, Qt.Checked)
    assert window.canvas.series_color(voltage).name() == original_color
    window._tree_items[voltage].setCheckState(0, Qt.Unchecked)
    window._tree_items[voltage].setCheckState(0, Qt.Checked)
    assert window.canvas.series_color(voltage).name() == original_color

    header_style = window.tree.styleSheet()
    assert "QHeaderView::section" in header_style
    assert "background-color: #343840" in header_style


def test_per_measurement_multiplier_and_base_rebuild_buffered_graph(qtbot) -> None:
    window = GrathPlotWindow(DESCRIPTORS)
    qtbot.addWidget(window)
    voltage = "MONITOR:voltage"
    current = "MONITOR:channels.current"
    window._tree_items[voltage].setCheckState(0, Qt.Checked)
    window._tree_items[current].setCheckState(0, Qt.Checked)
    window.ingest_response(
        "MONITOR",
        {
            "success": True,
            "result": {"voltage": 2.0, "channels": {"current": 3}},
        },
    )

    voltage_multiplier, voltage_base = window._transform_widgets[voltage]
    current_multiplier, current_base = window._transform_widgets[current]
    assert (voltage_multiplier.value(), voltage_base.value()) == (1.0, 0.0)
    assert voltage_multiplier.width() == 62
    voltage_multiplier.setValue(10.0)
    voltage_base.setValue(1.0)
    current_multiplier.setValue(2.0)
    current_base.setValue(-1.0)

    assert window.canvas.series[voltage][-1][1] == 21.0
    assert window.canvas.series[current][-1][1] == 5.0
    assert window.sample_buffer[-1][1][voltage] == 2.0

    window.mode_combo.setCurrentIndex(window.mode_combo.findData(window.XY_MODE))
    window.x_combo.setCurrentIndex(window.x_combo.findData(voltage))
    assert window.canvas.series[current][-1] == (21.0, 5.0)


def test_unselected_measurement_is_restored_from_buffer(qtbot) -> None:
    window = GrathPlotWindow(DESCRIPTORS)
    qtbot.addWidget(window)
    key = "MONITOR:voltage"

    window.ingest_response(
        "MONITOR", {"success": True, "result": {"voltage": 10.0}}
    )
    window.ingest_response(
        "MONITOR", {"success": True, "result": {"voltage": 11.0}}
    )
    assert key not in window.canvas.series

    window._tree_items[key].setCheckState(0, Qt.Checked)
    assert [point[1] for point in window.canvas.series[key]] == [10.0, 11.0]

    window._tree_items[key].setCheckState(0, Qt.Unchecked)
    window._tree_items[key].setCheckState(0, Qt.Checked)
    assert [point[1] for point in window.canvas.series[key]] == [10.0, 11.0]


def test_buffer_retention_and_memory_limits(qtbot) -> None:
    window = GrathPlotWindow(DESCRIPTORS)
    qtbot.addWidget(window)
    window.history_spin.setValue(60)
    assert window._buffer_retention_seconds() == 60.0
    window.history_spin.setValue(5)
    assert window._buffer_retention_seconds() == MIN_BUFFER_SECONDS

    window._append_buffer_frame(100.0, {"MONITOR:voltage": 1.0})
    window._append_buffer_frame(129.0, {"MONITOR:voltage": 2.0})
    window._append_buffer_frame(131.0, {"MONITOR:voltage": 3.0})
    assert [frame[0] for frame in window.sample_buffer] == [129.0, 131.0]

    window._clear_buffer()
    one_frame = window._estimate_frame_bytes(1)
    window.buffer_limit_bytes = one_frame * 2
    for timestamp in (200.0, 201.0, 202.0):
        window._append_buffer_frame(timestamp, {"MONITOR:voltage": timestamp})
    assert len(window.sample_buffer) == 2
    assert window.buffer_bytes <= window.buffer_limit_bytes
    assert MAX_BUFFER_BYTES == 1024 ** 3


def test_plot_image_and_buffer_csv_can_be_saved(qtbot, tmp_path) -> None:
    window = GrathPlotWindow(DESCRIPTORS)
    qtbot.addWidget(window)
    voltage = "MONITOR:voltage"
    current = "MONITOR:channels.current"
    window._tree_items[voltage].setCheckState(0, Qt.Checked)
    window.ingest_response(
        "MONITOR",
        {
            "success": True,
            "result": {"voltage": 12.5, "channels": {"current": 3}},
        },
    )
    window.ingest_response(
        "MONITOR",
        {
            "success": True,
            "result": {"voltage": 13.0, "channels": {"current": 4}},
        },
    )

    voltage_multiplier, voltage_base = window._transform_widgets[voltage]
    voltage_multiplier.setValue(2.0)
    voltage_base.setValue(1.0)

    csv_path, row_count = window.export_buffer_csv(str(tmp_path / "values"))
    assert csv_path.suffix == ".csv"
    assert row_count == 2
    with csv_path.open("r", encoding="utf-8-sig", newline="") as stream:
        rows = list(csv.reader(stream))
    assert rows[0][:2] == ["timestamp", "time_s"]
    assert voltage in rows[0]
    assert current in rows[0]
    assert [row[rows[0].index(voltage)] for row in rows[1:]] == ["12.5", "13.0"]
    assert [row[rows[0].index(current)] for row in rows[1:]] == ["3.0", "4.0"]

    image_path = window.save_plot_image(str(tmp_path / "graph"))
    assert image_path.suffix == ".png"
    assert image_path.read_bytes().startswith(b"\x89PNG\r\n\x1a\n")


def test_csv_can_merge_sparse_frames_from_one_poll_cycle(qtbot, tmp_path) -> None:
    window = GrathPlotWindow(DESCRIPTORS)
    qtbot.addWidget(window)
    voltage = "MONITOR:voltage"
    manual = "MANUAL:value"
    now = time.monotonic()
    window._append_buffer_frame(now, {voltage: 12.5})
    window._append_buffer_frame(now + 0.01, {manual: 7.0})
    window._append_buffer_frame(now + 0.50, {voltage: 13.0})
    window._append_buffer_frame(now + 0.51, {manual: 8.0})
    window.merge_csv_cycles_check.setChecked(True)

    csv_path, row_count = window.export_buffer_csv(str(tmp_path / "merged"))

    assert row_count == 2
    with csv_path.open("r", encoding="utf-8-sig", newline="") as stream:
        rows = list(csv.reader(stream))
    voltage_column = rows[0].index(voltage)
    manual_column = rows[0].index(manual)
    assert [(row[voltage_column], row[manual_column]) for row in rows[1:]] == [
        ("12.5", "7.0"),
        ("13.0", "8.0"),
    ]


def test_csv_merge_waits_for_complete_cycle_when_fast_source_repeats(
        qtbot, tmp_path) -> None:
    window = GrathPlotWindow(DESCRIPTORS)
    qtbot.addWidget(window)
    voltage = "MONITOR:voltage"
    slow = "SLOW:value"
    now = time.monotonic()
    window._append_buffer_frame(now, {voltage: 12.5})
    window._append_buffer_frame(now + 0.1, {voltage: 13.0})
    window._append_buffer_frame(now + 0.2, {slow: 7.0})

    csv_path, row_count = window.export_buffer_csv(str(tmp_path / "complete"))

    assert window.merge_csv_cycles_check.isChecked()
    assert window.merge_csv_cycles_check.text() == "Разъеденить время измерений"
    assert row_count == 1
    with csv_path.open("r", encoding="utf-8-sig", newline="") as stream:
        rows = list(csv.reader(stream))
    assert rows[1][rows[0].index(voltage)] == "13.0"
    assert rows[1][rows[0].index(slow)] == "7.0"


class CaptureWorker(gui.QObject):
    def __init__(self) -> None:
        super().__init__()
        self.requests: list[dict] = []

    def send_line(self, line: str) -> None:
        self.requests.append(json.loads(line))

    def stop(self) -> None:
        pass

    def wait(self, _milliseconds: int) -> bool:
        return True


def test_normal_and_graph_autopoll_share_one_request(qtbot) -> None:
    descriptor = dict(DESCRIPTORS["MONITOR"])
    descriptor["params"] = [{
        "name": "channel",
        "type": "integer",
        "default": 1,
    }]
    main = gui.MainWindow()
    qtbot.addWidget(main)
    worker = CaptureWorker()
    main.worker = worker
    main.descriptors = {"MONITOR": descriptor}
    form = gui.CommandForm(descriptor, main.send_request)
    qtbot.addWidget(form)
    form.param_widgets["channel"].setValue(7)
    main.forms = [form]
    main.autopoll_check.setChecked(True)

    graph = GrathPlotWindow(main.descriptors, main._request_graph_sample)
    qtbot.addWidget(graph)
    main.response_received.connect(graph.ingest_response)
    graph._tree_items["MONITOR:voltage"].setCheckState(0, Qt.Checked)

    # Whichever scheduler runs first, only one transaction may be outstanding.
    graph._poll_sources()
    main._poll()
    assert len(worker.requests) == 1
    assert len(main.pending) == 1
    assert worker.requests[0]["params"] == {"channel": 7}

    transaction = worker.requests[0]["id"]
    main._receive_line(json.dumps({
        "id": transaction,
        "success": True,
        "result": {"voltage": 12.0},
    }))
    graph._poll_sources()
    main._poll()
    assert len(worker.requests) == 1

    # The opposite timer order is deduplicated as well.
    form.last_poll = 0.0
    graph.last_poll.clear()
    main._poll()
    graph._poll_sources()
    assert len(worker.requests) == 2
    second_transaction = worker.requests[1]["id"]
    main._receive_line(json.dumps({
        "id": second_transaction,
        "success": True,
        "result": {"voltage": 13.0},
    }))

    main.worker = None


def test_starset_button_opens_plot_and_routes_correlated_responses(qtbot) -> None:
    window = gui.MainWindow()
    qtbot.addWidget(window)
    window.descriptors = dict(DESCRIPTORS)
    window._build_dynamic_tabs()
    assert window.graph_button.isEnabled()

    window._show_graph_plot()
    assert window.graph_window is not None
    key = "MONITOR:voltage"
    window.graph_window._tree_items[key].setCheckState(0, Qt.Checked)
    window.pending[77] = ("MONITOR", None, time.monotonic() + 1)
    window._receive_line(json.dumps({
        "id": 77,
        "success": True,
        "result": {"voltage": 9.75, "channels": {"current": 2}},
    }))

    assert window.graph_window.canvas.series[key][-1][1] == 9.75


def test_gpio_input_widget_registers_polls_and_streams_states(qtbot) -> None:
    main = gui.MainWindow()
    qtbot.addWidget(main)
    main.descriptors = {
        "PIN_GET": {
            "cmd": "PIN_GET",
            "title": "Board pins",
            "tab": "GPIO",
            "group": "Digital inputs",
        }
    }
    sent: list[tuple[str, dict]] = []
    panel = gui.IOPanel(
        [
            {"name": "BUTTON", "type": "IN", "state": 0},
            {"name": "LED", "type": "OUT", "state": 0},
        ],
        lambda command, params, _callback: sent.append((command, params)),
        update_observer=main._gpio_panel_updated,
    )
    qtbot.addWidget(panel)
    main.io_panels = [panel]
    graph = GrathPlotWindow(
        main.descriptors,
        gpio_requester=main._request_graph_gpio,
    )
    qtbot.addWidget(graph)
    main.graph_window = graph
    main._sync_graph_gpio_inputs()

    key = "GPIO:PIN_GET:local:BUTTON"
    assert key in graph.measurements
    output_key = "GPIO:PIN_GET:local:LED"
    assert output_key in graph.measurements
    assert not graph.sample_buffer
    assert main.graph_button.isEnabled()
    graph._tree_items[key].setCheckState(0, Qt.Checked)
    graph._tree_items[output_key].setCheckState(0, Qt.Checked)
    assert key in graph.canvas.event_keys
    assert graph.canvas.series[key] == []

    graph._poll_sources()
    assert sent == [("PIN_GET", {"pins": ["BUTTON", "LED"]})]
    panel._updated({
        "success": True,
        "result": {"pins": [{"name": "BUTTON", "state": 1}]},
    })
    assert panel.cards["BUTTON"].state == 1
    assert graph.canvas.series[key][-1][1] == 1.0
    event_count = len(graph.canvas.series[key])
    panel._in_flight = False
    panel._updated({
        "success": True,
        "result": {"pins": [{"name": "BUTTON", "state": 1}]},
    })
    assert len(graph.canvas.series[key]) == event_count

    panel._in_flight = False
    panel._updated({
        "success": True,
        "result": {"pins": [{"name": "BUTTON", "state": 0}]},
    })
    assert graph.canvas.series[key][-1][1] == 0.0
    assert not graph.canvas.grab().toImage().isNull()

    # With ordinary auto-poll enabled, inputs reuse its updates while selected
    # outputs receive the additional read they need.
    panel._in_flight = False
    main.autopoll_check.setChecked(True)
    main._request_graph_gpio({"PIN_GET:local": {"BUTTON", "LED"}})
    assert sent[-1] == ("PIN_GET", {"pins": ["LED"]})


def test_gpio_events_do_not_expand_value_axis(qtbot) -> None:
    graph = GrathPlotWindow(DESCRIPTORS)
    qtbot.addWidget(graph)
    canvas = graph.canvas
    canvas.set_series(
        {"voltage", "gpio"},
        {"voltage": "Voltage", "gpio": "Input"},
    )
    canvas.set_event_series({"gpio"})
    canvas.append("voltage", 0.0, 1500.0)
    canvas.append("voltage", 1.0, 1600.0)
    canvas.append("gpio", 0.5, 0.0)

    ranges = canvas._data_ranges()
    assert ranges is not None
    _x_range, y_range = ranges
    assert y_range == (1495.0, 1605.0)
