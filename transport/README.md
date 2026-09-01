# Transport

Besides `KRJ1` JSON and `KRB1` BSON, this library accepts `KRC1` compact CBOR.
Transport treats the payload as opaque bytes; the numeric-key CBOR mapping is
documented in `../docs/cbor_protocol_v3.md`.

Независимая от Krul command-interface библиотека потокового framing для JSON
и BSON/CBOR payload. Она отвечает только за `magic`, длину, CRC и восстановление
границ кадров; интерпретация payload остаётся на стороне вызывающего кода.

Wire-формат и API описаны в `../docs/transport_v1.md` и
`inc/krul_transport.h`.
