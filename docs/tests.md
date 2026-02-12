# Tests

This document describes the intent and coverage of the current test suite.

## Running tests
- Configure and build (once):
  ```bash
  cmake -S . -B build -DZCM_BUILD_TESTS=ON
  cmake --build build
  ```
- List all registered CTest entries:
  ```bash
  ctest --test-dir build -N
  ```
- Run all tests:
  ```bash
  ctest --test-dir build --output-on-failure
  ```
- Run one test by name pattern:
  ```bash
  ctest --test-dir build --output-on-failure -R zcm_cli_workflow
  ```
- Save verbose run output to `build/test_results/ctest.log`:
  ```bash
  cmake --build build --target test_results
  ```
- Run individual test binaries directly:
  ```bash
  ./build/tests/zcm_smoke
  ./build/tests/zcm_msg_roundtrip
  ./build/tests/zcm_msg_fuzz
  ./build/tests/zcm_msg_vectors
  ./build/tests/zcm_node_list
  ./build/tests/zcm_node_unique_name
  ./build/tests/zcm_node_prune_dead
  ./build/tests/zcm_proc_reannounce
  ./build/tests/zcm_cli_workflow
  ```

## Updating the list of tests
1. Add a test source file under `tests/` (for example `tests/node/my_test.c`).
2. Register it in `CMakeLists.txt` by adding:
   - `add_executable(...)`
   - `target_link_libraries(... PRIVATE zcm_lib)`
   - `add_test(NAME ... COMMAND ...)`
   - `set_target_properties(... RUNTIME_OUTPUT_DIRECTORY ${ZCM_TEST_OUTPUT_DIR})`
3. Reconfigure and check registration:
   ```bash
   cmake -S . -B build -DZCM_BUILD_TESTS=ON
   ctest --test-dir build -N
   ```
4. Add/update the corresponding section in this file.

## Test descriptions

### `zcm_smoke`
**Purpose:** basic lifecycle sanity check for context + broker + node.
- Creates a `zcm_context`.
- Starts a broker (`inproc://zcm-broker`).
- Registers a name and then looks it up.
- Verifies the resolved endpoint matches.

**Files:** `tests/smoke/zcm_smoke.c`

### `zcm_msg_roundtrip`
**Purpose:** end-to-end serialize/deserialize coverage for typed messages.
- Builds a `zcm_msg` with multiple item types:
  - char, short, int, long, float, double, text, bytes, array
- Serializes, parses back, and validates field values.

**Files:** `tests/msg/zcm_msg_roundtrip.c`

### `zcm_msg_fuzz`
**Purpose:** robustness under random input to the decoder.
- Feeds random byte buffers into `zcm_msg_from_bytes`.
- Ensures the decoder and validator do not crash.

**Files:** `tests/msg/zcm_msg_fuzz.c`

### `zcm_msg_vectors`
**Purpose:** deterministic compatibility check for a small fixed message.
- Creates a message with type `Test`, int `42`, text `ok`.
- Serializes, parses back, validates fields.

**Files:** `tests/msg/zcm_msg_vectors.c`

### `zcm_node_list`
**Purpose:** registry listing API coverage.
- Starts broker and registers two names.
- Calls `zcm_node_list()` and asserts both are present.

**Files:** `tests/node/zcm_node_list.c`

### `zcm_node_unique_name`
**Purpose:** enforce unique process names across different owners.
- Registers a name with one PID and allows same-owner re-register.
- Verifies a different owner PID with same name is rejected.
- Confirms original endpoint remains unchanged.

**Files:** `tests/node/zcm_node_unique_name.c`

### `zcm_node_prune_dead`
**Purpose:** prune stale registrations owned by dead PIDs.
- Registers a ghost node with a non-existing PID.
- Verifies list/lookup operations remove stale entry.

**Files:** `tests/node/zcm_node_prune_dead.c`

### `zcm_proc_reannounce`
**Purpose:** verify automatic proc re-registration after broker restart.
- Starts a broker and a `zcm_proc` test instance.
- Verifies the proc name is initially resolvable.
- Stops and restarts broker on the same endpoint.
- Polls lookup until the proc name appears again.

**Files:** `tests/node/zcm_proc_reannounce.c`

### `zcm_cli_workflow`
**Purpose:** end-to-end CLI workflow on real daemons and config files.
- Starts broker and `zcm_proc` publisher/basic/subscriber processes.
- Waits for `zcm names` registration and validates QUERY/QUERY_RPL exchange.
- Kills publisher and checks it disappears from names.
- Stops broker and verifies offline names behavior.
- Restarts broker, relaunches publisher, and verifies workflow recovers.

**Files:** `tests/node/zcm_cli_workflow.c`
