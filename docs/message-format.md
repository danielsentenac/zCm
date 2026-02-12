# Message Format

zcm-msg is a simple typed-value envelope independent of the transport and data format.

## Envelope
- `magic` (u32): `0x5A434D31` (`ZCM1`)
- `version` (u16): `1`
- `type_len` (u16): length of type string
- `payload_len` (u32)
- `type` (bytes): UTF-8, no NUL terminator
- `payload` (bytes): sequence of typed items

## Typed items
Each item begins with a one-byte `item_type` followed by item data.

Scalar items
- `CHAR`  (1): 1 byte
- `SHORT` (2): 2 bytes
- `INT`   (3): 4 bytes
- `LONG`  (4): 8 bytes
- `FLOAT` (5): 4 bytes
- `DOUBLE`(6): 8 bytes

Array item
- `ARRAY` (7):
  - `array_type` (u8): CHAR/SHORT/INT/FLOAT/DOUBLE
  - `elements` (u32)
  - `data` (elements * element_size)

Text item
- `TEXT` (8):
  - `len` (u32)
  - `data` (len bytes)

Bytes item
- `BYTES` (9):
  - `len` (u32)
  - `data` (len bytes)

## Value property
For command-line interoperability and generic process handling, zCm defines a
standardized scalar value property encoded as three typed items in order:

1. `TEXT` marker: `__zcm_value__`
2. `TEXT` scalar kind: `text` | `double` | `float` | `int`
3. Value item matching kind:
   - `TEXT` for `text`
   - `DOUBLE` for `double`
   - `FLOAT` for `float`
   - `INT` for `int`

Library helpers:
- `zcm_msg_put_value_text()`
- `zcm_msg_put_value_double()`
- `zcm_msg_put_value_float()`
- `zcm_msg_put_value_int()`
- `zcm_msg_get_value()`

## Endianness
All numeric fields are stored in **little-endian** encoding, regardless of host endianness.
