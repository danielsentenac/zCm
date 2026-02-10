# zcm-msg format

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

## Endianness
All numeric fields are stored in **little-endian** encoding, regardless of host endianness.
