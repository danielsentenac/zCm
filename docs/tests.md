# zCm Tests

This document describes the intent and coverage of the current test suite.

## Running tests
- Standard:
  ```bash
  ctest --test-dir build
  ```
- Verbose:
  ```bash
  cmake --build build --target test-verbose
  ```
- Individual test binaries:
  ```bash
  ./build/tests/zcm_smoke
  ./build/tests/zcm_msg_roundtrip
  ./build/tests/zcm_msg_fuzz
  ./build/tests/zcm_msg_vectors
  ./build/tests/zcm_node_list
  ```

## Test descriptions

### `zcm_smoke`
**Purpose:** basic lifecycle sanity check for context + broker + node.
- Creates a `zcm_context`.
- Starts a broker (`inproc://zcm-broker`).
- Registers a name and then looks it up.
- Verifies the resolved endpoint matches.

**Files:** `tests/zcm_smoke.c`

### `zcm_msg_roundtrip`
**Purpose:** end-to-end serialize/deserialize coverage for typed messages.
- Builds a `zcm_msg` with multiple item types:
  - char, short, int, long, float, double, text, bytes, array
- Serializes, parses back, and validates field values.

**Files:** `tests/zcm_msg_roundtrip.c`

### `zcm_msg_fuzz`
**Purpose:** robustness under random input to the decoder.
- Feeds random byte buffers into `zcm_msg_from_bytes`.
- Ensures the decoder and validator do not crash.

**Files:** `tests/zcm_msg_fuzz.c`

### `zcm_msg_vectors`
**Purpose:** deterministic compatibility check for a small fixed message.
- Creates a message with type `Test`, int `42`, text `ok`.
- Serializes, parses back, validates fields.

**Files:** `tests/zcm_msg_vectors.c`

### `zcm_node_list`
**Purpose:** registry listing API coverage.
- Starts broker and registers two names.
- Calls `zcm_node_list()` and asserts both are present.

**Files:** `tests/zcm_node_list.c`
