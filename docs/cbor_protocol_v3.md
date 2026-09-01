# Krul compact CBOR mapping v3 {#cbor_protocol_v3}

`KRC1` carries the same Krul v3 data model as JSON and BSON, encoded as CBOR.
It is intentionally non-canonical: writers emit indefinite-length maps and
arrays (`0xbf`/`0x9f` with `0xff` break). Decoders also accept definite-length
maps and arrays. A payload contains exactly one root map.

## Numeric map keys

Every map key on the wire is an unsigned 16-bit integer. For a key without an
explicit descriptor tag, both peers derive the tag from its UTF-8 name:

```text
hash = 5381
for byte in utf8(name):
    hash = ((hash * 33) XOR byte) modulo 65521
tag = hash + 1
```

Thus zero is never emitted. An explicit nonzero `krul_field_desc_t.tag`
overrides the derived value. `DESCRIBE` publishes the effective tag for every
named parameter/result field, allowing clients to learn application keys.
Tags must be unique among sibling fields; server initialization rejects a
collision.

The standard envelope and discovery keys use the same derivation. Clients must
know those names before discovery. JSON and BSON continue to use text keys.

## Value mapping

| Krul value | CBOR representation |
| --- | --- |
| object | map |
| array | array |
| string/enum | UTF-8 text string |
| `i32` | major type 0 or 1 |
| `u32` | major type 0 |
| `f32` | IEEE-754 binary32; decoders also accept binary16/binary64 |
| boolean | `false` / `true` |
| null | `null` |

Byte strings, semantic tags, non-finite floats, and non-integer/non-text map
keys are not part of the Krul CBOR profile. The transport-wide 10 KiB payload
limit still applies.
