from __future__ import annotations

import struct

import pytest

from krul_wire import (
    FORMAT_BSON,
    FORMAT_CBOR,
    FORMAT_JSON,
    FrameParser,
    cbor_key_tag,
    crc16_ccitt_false,
    decode_cbor,
    encode_cbor,
    decode_payload,
    encode_frame,
)


@pytest.mark.parametrize("wire_format", [FORMAT_JSON, FORMAT_BSON, FORMAT_CBOR])
def test_round_trip_nested_krul_frame(wire_format: str) -> None:
    message = {
        "id": 4_000_000_000,
        "success": True,
        "result": {
            "number": -7,
            "ratio": 1.5,
            "text": "Привет",
            "empty": None,
            "items": [1, "two", False],
        },
    }
    encoded = encode_frame(message, wire_format)
    parser = FrameParser()
    frames = []
    errors = []
    for value in encoded:
        new_frames, new_errors = parser.feed(bytes((value,)))
        frames.extend(new_frames)
        errors.extend(new_errors)
    assert not errors
    assert len(frames) == 1
    assert frames[0].wire_format == wire_format
    assert decode_payload(frames[0].payload, wire_format) == message


def test_crc_reference_and_resynchronization() -> None:
    assert crc16_ccitt_false(b"123456789") == 0x29B1
    bad = bytearray(encode_frame({"id": 1}, FORMAT_JSON))
    bad[-1] ^= 0x80
    good = encode_frame({"id": 2}, FORMAT_BSON)
    parser = FrameParser()
    frames, errors = parser.feed(b"noise" + bad + good)
    assert len(errors) == 1
    assert len(frames) == 1
    assert decode_payload(frames[0].payload, frames[0].wire_format)["id"] == 2


def test_compact_cbor_uses_numeric_keys_and_indefinite_containers() -> None:
    payload = encode_cbor({"id": 7, "params": {"value": 3}})
    assert payload[0] == 0xbf and payload[-1] == 0xff
    assert b"id" not in payload and b"params" not in payload
    assert cbor_key_tag("id") != cbor_key_tag("params")
    assert decode_cbor(payload) == {"id": 7, "params": {"value": 3}}


def test_rejects_oversized_length_without_waiting_for_payload() -> None:
    parser = FrameParser(32)
    frames, errors = parser.feed(b"KRJ1" + struct.pack("<I", 33))
    assert not frames
    assert errors == ["invalid Krul payload length: 33"]
