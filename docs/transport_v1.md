# Krul transport framing v1 {#transport_v1}

`KRC1` is the compact-CBOR transport variant. It uses the same length, CRC,
10 KiB limit, response-codec selection, and stream resynchronization rules as
`KRJ1` and `KRB1`. Its payload mapping is defined in @ref cbor_protocol_v4;
stream receivers search for all three magic values.

Этот документ задаёт двоичное обрамление одного готового payload Krul для
потоковых транспортов: UART, TCP, USB CDC и stdin/stdout. Сериализация payload
выбирается magic; команды и envelope Krul от неё не зависят.

## Формат кадра

Все многобайтовые целые передаются в little-endian.

| Смещение | Размер | Поле |
| --- | ---: | --- |
| 0 | 4 | magic: ASCII `KRJ1` для JSON, `KRB1` для BSON или `KRC1` для compact CBOR |
| 4 | 4 | `payload_length`, беззнаковый `uint32` |
| 8 | `payload_length` | ровно один request, response или event Krul |
| `8 + payload_length` | 2 | CRC-16, `uint16` little-endian |

`payload_length` считает только байты payload. Заголовок и CRC в него не входят.
Полный размер кадра равен `payload_length + 10`. Допустимый payload Krul имеет
длину от 1 до 10 240 байт.

`KRJ1` содержит один UTF-8 JSON object без `NUL`, CR или LF, добавляемых
транспортом. `KRB1` содержит один полный BSON document; внешняя длина payload
должна совпадать с BSON `int32` document length.

## CRC

Используется CRC-16/CCITT-FALSE:

- polynomial: `0x1021`;
- initial value: `0xFFFF`;
- refin/refout: false;
- xorout: `0x0000`;
- эталон: CRC ASCII `123456789` равен `0x29B1`.

CRC считается над четырьмя wire-байтами `payload_length` и всеми байтами
payload в указанном порядке. Magic и само поле CRC в расчёт не входят.

## Потоковый приём

Получатель ищет `KRJ1`, `KRB1` или `KRC1` скользящим окном, читает длину, проверяет её
до накопления payload, принимает указанное число байтов и затем два байта CRC.
Чтение может вернуть часть кадра или несколько кадров одновременно. Magic
внутри payload не имеет специального значения.

После неверной длины или CRC текущий кандидат отбрасывается, и поиск magic
возобновляется. Это обеспечивает восстановление после мусора или потери байтов.
CRC обнаруживает случайные ошибки линии, но не обеспечивает аутентификацию.

Ответ на request кодируется тем же codec, что указан magic запроса. События,
не привязанные к request, используют последний корректно принятый формат; до
первого запроса используется JSON. CVM gateway принимает только `KRJ1`, но
использует тот же transport v1 framing.

Реализация C находится в независимой библиотеке `transport` и её
`krul_transport.h`; Python-реализация GUI и симулятора
находится в `Python GUI/krul_wire.py`.
