# Krul compact CBOR mapping v4 {#cbor_protocol_v4}

`KRC1` несёт ту же модель Krul v4, что JSON и BSON. Кодирование намеренно
неканоническое: writer использует indefinite-length maps/arrays (`0xbf` и
`0x9f`, завершённые `0xff`), decoder принимает также definite-length формы.

Map-ключи остаются без изменений относительно v3: это unsigned `uint16` tags,
полученные из UTF-8 имени DJB2-XOR-хешем modulo 65521 плюс один, либо явно
заданные ненулевым `krul_field_desc_t.tag`. Версия 4 не добавляет фиксированные
ключи команд или конверта.

| Krul value | CBOR representation |
| --- | --- |
| object | map |
| array | array |
| string | UTF-8 text string |
| enum / `i32` | major type 0 или 1 |
| `u32` | major type 0 |
| `f32` | IEEE-754 binary32 |
| boolean | `false` / `true` |

Главное отличие от @ref cbor_protocol_v3: enum кодируется целым `int32`, а не
text string. `DESCRIBE` также публикует числовые `value` и `default` enum.
Успешный `PING` содержит только map с `id` и `success`; map `result` отсутствует.

Ограничение payload transport v1 остаётся 10 КиБ.
