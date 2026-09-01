from __future__ import annotations

import time

from krul_simulator import KrulSimulator, SQUARE_WAVE_PIN


def request(simulator: KrulSimulator, command: str, transaction: int,
            params: dict | None = None) -> dict:
    payload = {"cmd": command, "id": transaction}
    if params is not None:
        payload["params"] = params
    return simulator.dispatch(payload).response


def test_discovery_and_stateful_gpio() -> None:
    simulator = KrulSimulator()

    identity = request(simulator, "WHOAMI", 1)
    assert identity["result"]["protocol_version"] == 3
    assert identity["result"]["device_id"] == "ARK-PC-SIM-01"

    commands = request(simulator, "CMD_LIST", 2)["result"]["cmd_name"]
    assert {"DESCRIBE", "PIN_GET", "PIN_SET", "ECHO"}.issubset(commands)

    pin_get = request(simulator, "DESCRIBE", 6, {"name": "PIN_GET"})["result"]
    pin_set = request(simulator, "DESCRIBE", 7, {"name": "PIN_SET"})["result"]
    assert "builtin" not in pin_get
    assert "builtin" not in pin_set
    assert pin_get["widget_hint"] == "special_gpio"
    assert pin_set["widget_hint"] == "special_gpio"

    described = request(simulator, "DESCRIBE", 3, {"name": "ECHO"})
    assert described["result"]["params"][1]["default"] == 1

    updated = request(
        simulator, "PIN_SET", 4,
        {"pins": [{"name": "LED_GREEN", "state": 1}]},
    )
    assert updated["success"]
    pins = request(simulator, "PIN_GET", 5, {"pins": ["LED_GREEN"]})
    assert pins["result"]["pins"][0]["state"] == 1

    all_off = request(
        simulator, "PIN_SET", 8,
        {"pins": [{"name": "ALL", "state": 0}]},
    )
    assert all_off["success"]
    assert len(all_off["result"]["pins"]) > 1
    assert all(pin["state"] == 0 for pin in all_off["result"]["pins"])


def test_validation_and_log_event() -> None:
    simulator = KrulSimulator()

    invalid = request(simulator, "ECHO", 1, {"count": 2})
    assert invalid["error"]["code"] == 1

    dispatched = simulator.dispatch({
        "cmd": "LOG_EMIT",
        "id": 2,
        "params": {"severity": "warning", "message": "test event"},
    })
    assert dispatched.response["success"]
    assert dispatched.events == [{
        "event": "log",
        "data": {"severity": "warning", "message": "test event"},
    }]


def test_square_wave_input_has_two_second_period() -> None:
    now = [100.0]
    simulator = KrulSimulator(clock=lambda: now[0])

    def state_at(timestamp: float, transaction: int) -> int:
        now[0] = timestamp
        response = request(
            simulator,
            "PIN_GET",
            transaction,
            {"pins": [SQUARE_WAVE_PIN]},
        )
        pin = response["result"]["pins"][0]
        assert pin["type"] == "IN"
        return pin["state"]

    assert state_at(100.0, 10) == 0
    assert state_at(100.999, 11) == 0
    assert state_at(101.0, 12) == 1
    assert state_at(101.999, 13) == 1
    assert state_at(102.0, 14) == 0


def test_simulator_exposes_every_starset_widget_family() -> None:
    simulator = KrulSimulator()
    commands = request(simulator, "CMD_LIST", 1)["result"]["cmd_name"]

    assert {"PWM_SET", "DAC_SET", "ADC_READ", "ADC_READ_BY_GROUP"} <= set(commands)
    assert {"DELAY", "DELAYED_ECHO", "SELF_TEST", "CALIBRATE"} <= set(commands)

    descriptors = {
        command: request(simulator, "DESCRIBE", index + 2, {"name": command})["result"]
        for index, command in enumerate(commands)
    }
    command_hints = {
        descriptor.get("widget_hint") for descriptor in descriptors.values()
    }
    parameter_hints = {
        field.get("widget_hint")
        for descriptor in descriptors.values()
        for field in descriptor.get("params", [])
    }
    result_hints = {
        field.get("widget_hint")
        for descriptor in descriptors.values()
        for field in descriptor.get("result", [])
    }

    assert {"special_gpio", "special_pwm"} <= command_hints
    assert "special_dac" in parameter_hints
    assert {"special_adc", "special_adc_group"} <= result_hints

    gallery = descriptors["WIDGET_GALLERY"]
    assert {field["type"] for field in gallery["params"]} == {
        "string", "integer", "float", "boolean", "enum",
    }
    assert {field["type"] for field in gallery["result"]} == {
        "string", "integer", "float", "boolean", "enum", "console_string",
    }


def test_large_demo_groups_contain_many_working_commands() -> None:
    sensor_commands = [name for name in request(
        KrulSimulator(), "CMD_LIST", 1
    )["result"]["cmd_name"] if name.startswith("DEMO_SENSOR_")]
    actuator_commands = [name for name in request(
        KrulSimulator(), "CMD_LIST", 2
    )["result"]["cmd_name"] if name.startswith("DEMO_ACTUATOR_")]
    assert len(sensor_commands) == 12
    assert len(actuator_commands) == 12

    simulator = KrulSimulator()
    sensor = request(simulator, "DEMO_SENSOR_12", 3)
    actuator = request(
        simulator, "DEMO_ACTUATOR_12", 4,
        {"enabled": True, "setpoint": 37.5, "mode": "automatic"},
    )
    assert sensor["success"] and sensor["result"]["state"] == "normal"
    assert actuator["result"] == {
        "applied": True, "actual": 37.5, "mode": "automatic",
    }


def test_delayed_echo_really_defers_its_response() -> None:
    simulator = KrulSimulator()
    started = time.monotonic()
    response = request(
        simulator, "DELAYED_ECHO", 1,
        {"text": "later", "milliseconds": 30},
    )
    elapsed = time.monotonic() - started

    assert elapsed >= 0.025
    assert response["result"] == {"text": "later", "milliseconds": 30}
