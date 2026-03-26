\page tool_zcm_proc zcm-proc

`zcm_proc` is a single unified process daemon.

Unified process executable:
```bash
./build/examples/zcm_proc <proc-config-file>
```
Config content must be XML (`.cfg` samples are XML files).

## Behavior
- Every `zcm_proc` is an infinite daemon.
- It always answers requests over direct [ØMQ/ZeroMQ](https://zeromq.org/) `REQ/REP` semantics.
- Default command behavior includes:
  - `PING -> PONG`
  - `DATA_METRICS -> ROLE=NONE;PUB_PORT=-1;PUSH_PORT=-1;PUB_BYTES=-1;SUB_BYTES=-1;PUSH_BYTES=-1;PULL_BYTES=-1;SUB_TARGETS=-;SUB_TARGET_BYTES=-`
- It periodically re-registers in broker so names recover after broker restart.
  - tune interval with `ZCM_PROC_REANNOUNCE_MS` (default `1000`)
- Re-announce retries use exponential backoff capped by
  `ZCM_PROC_REANNOUNCE_BACKOFF_MAX_MS` (default `30000`).
- Registration host metadata can be overridden with
  `ZCM_PROC_ADVERTISED_HOST` (fallback alias: `ZCM_ADVERTISED_HOST`).
- `SUB/PULL` receive-byte metrics are aged with `ZCM_PROC_RX_STALE_MS`
  (default `5000` ms): stale values are reported as `0`.
- Optional repeated `dataSocket` entries configure bytes `PUB/SUB/PUSH/PULL` roles.

## Environment Overrides

| Variable | Meaning |
| --- | --- |
| `ZCM_PROC_CONFIG_FILE` | XML config file override. |
| `ZCM_PROC_CONFIG_DIR` | Base directory used to resolve relative config file names. |
| `ZCM_PROC_CONFIG_SCHEMA` | XSD schema path override. |
| `ZCM_PROC_REANNOUNCE_MS` | Broker re-announce base period in ms (default `1000`, valid `100..60000`). |
| `ZCM_PROC_REANNOUNCE_BACKOFF_MAX_MS` | Max exponential backoff for re-announce retries (default `30000`, valid `1000..300000`). |
| `ZCM_PROC_ADVERTISED_HOST` | Host/IP advertised in broker registration endpoint metadata. |
| `ZCM_ADVERTISED_HOST` | Compatibility alias used when `ZCM_PROC_ADVERTISED_HOST` is not set. |
| `ZCM_PROC_RX_STALE_MS` | Staleness window for `SUB/PULL` receive-byte metrics before reporting `0` (default `5000`, valid `0..600000`; `0` disables aging). |

## XML Config Reference
Validation schema:
- `$ZCM_PROC_CONFIG_SCHEMA`, else `config/schema/proc-config.xsd`

`zcm_proc` reads one XML file passed on the command line and validates it before startup.
The file must contain exactly one `<process>` inside `<procConfig>`.

Minimal skeleton:
```xml
<?xml version="1.0" encoding="UTF-8"?>
<procConfig>
  <process name="myproc">
    <dataSocket type="PUB" payload="tick" intervalMs="1000"/>
    <control timeoutMs="200"/>
    <handlers>
      <type name="QUERY">
        <arg kind="double"/>
        <arg kind="text"/>
      </type>
    </handlers>
  </process>
</procConfig>
```

Element order matters because the XSD uses a strict sequence:
1. zero or more `<dataSocket/>`
2. optional `<control/>`
3. optional `<handlers>...</handlers>`

### Element Structure

| Element | Required | Count | Notes |
| --- | --- | --- | --- |
| `<procConfig>` | yes | exactly 1 | Root element. |
| `<process name="...">` | yes | exactly 1 | Declares the broker registration name and all runtime behavior. |
| `<dataSocket .../>` | no | `0..16` | Data path role entries for `PUB`, `SUB`, `PUSH`, or `PULL`. |
| `<control timeoutMs="..."/>` | no | `0..1` | Tunes REP control socket send/recv timeout. |
| `<handlers>` | no | `0..1` | Container for typed request signatures. |
| `<handlers><type name="...">` | no | `0..32` | One typed request signature. |
| `<handlers><type><arg kind="..."/>` | no | `0..32` per type | Ordered payload fields expected for that request type. |

### `<process>` Attributes

| Attribute | Required | Meaning |
| --- | --- | --- |
| `name` | yes | Process name used for broker registration, `zcm names`, `zcm ping`, `zcm kill`, and `zcm send NAME ...`. |

### `<dataSocket>` Attributes

| Attribute | Used By | Required | Default | Meaning |
| --- | --- | --- | --- | --- |
| `type` | all | yes | none | One of `PUB`, `SUB`, `PUSH`, `PULL`. |
| `payload` | `PUB`, `PUSH` | no | `tick` | Text payload sent periodically by the sender worker. |
| `intervalMs` | `PUB`, `PUSH` | no | `1000` | Positive send period in milliseconds. |
| `target` | `SUB`, `PULL` | no | empty | Legacy single-target form. |
| `targets` | `SUB`, `PULL` | no | empty | Comma-separated target names. Each non-empty item creates one runtime receiver entry. |
| `topics` | `SUB` only | no | subscribe all | Comma-separated topic prefixes passed to `zmq_setsockopt(ZMQ_SUBSCRIBE, ...)`. |

`<dataSocket>` rules and semantics:
- `PUB` and `PUSH` auto-allocate their TCP port from the current `ZCmDomains` port range.
- Do not define `@port` manually. Runtime rejects it for sender sockets.
- `payload` is treated as plain text bytes and `PUB_BYTES`/`PUSH_BYTES` are derived from its string length.
- `intervalMs` must be a positive integer.
- `SUB` and `PULL` require at least one non-empty target via `target`, `targets`, or both.
- If both `target` and `targets` are present, both are loaded. This is additive, not exclusive.
- `targets="a,b,c"` is trimmed token-by-token, so whitespace around commas is ignored.
- `topics` is valid only for `SUB`. Using it on `PULL` is an error.
- `topics` is split on commas, surrounding whitespace is trimmed, and empty items are ignored.
- If `topics` is omitted, the `SUB` socket subscribes to all incoming messages.
- One `<dataSocket type="SUB" targets="a,b"/>` expands internally to one subscriber worker per target, each sharing the same topic filter list.

Role-specific behavior:
- `PUB`
  - binds a local TCP port
  - sends `payload` every `intervalMs`
  - reports `PUB_PORT` and `PUB_BYTES`
- `SUB`
  - resolves each target publisher via control command `DATA_PORT_PUB` (fallback alias: `DATA_PORT`)
  - optionally filters by `topics`
  - reports `SUB_BYTES` from the last received payload
- `PUSH`
  - binds a local TCP port
  - sends `payload` every `intervalMs`
  - reports `PUSH_PORT` and `PUSH_BYTES`
- `PULL`
  - resolves each target pusher via control command `DATA_PORT_PUSH`
  - reports `PULL_BYTES` from the last received payload

### `<control>` Attributes

| Attribute | Required | Default | Meaning |
| --- | --- | --- | --- |
| `timeoutMs` | no | `200` | REP control socket send/recv timeout in milliseconds. |

### `<handlers>` and `<type>` Attributes

| Element / Attribute | Required | Meaning |
| --- | --- | --- |
| `<type name="...">` | yes | Request type name matched case-insensitively at runtime. |
| `<arg kind="text|double|float|int"/>` | yes | One ordered payload field in the expected request body. |
| `<type reply="...">` | no in schema | Currently ignored by runtime. Reply type is always built as `<REQ_TYPE>_RPL`. |

Typed handler rules:
- Argument order is strict. Incoming payloads must match the declared `<arg>` order exactly.
- Supported argument kinds are `text`, `double`, `float`, and `int`.
- The handler signature only describes request decoding. Reply payload content is produced by user code.
- Successful typed replies use message type `<REQ_TYPE>_RPL`.
- Malformed typed requests return `ERROR` with the expected signature summary.
- `zcm send` preserves the order of repeated payload flags, so the CLI can be used to match the handler signature exactly.

Builtin text/control commands available even without `<handlers>`:
- `PING -> PONG`
- `DATA_METRICS -> ROLE=...;PUB_PORT=...;PUSH_PORT=...;PUB_BYTES=...;SUB_BYTES=...;PUSH_BYTES=...;PULL_BYTES=...;SUB_TARGETS=...;SUB_TARGET_BYTES=...`
- unknown text command -> `OK`

Additional builtin payload-byte commands exposed by `zcm_proc`:
- `DATA_PAYLOAD_BYTES_PUB`
- `DATA_PAYLOAD_BYTES_SUB`
- `DATA_PAYLOAD_BYTES_PUSH`
- `DATA_PAYLOAD_BYTES_PULL`

Runtime limits:
- maximum `<dataSocket>` entries per config: `16`
- maximum `<type>` handlers per config: `32`
- maximum `<arg>` entries per type: `32`
- maximum topic prefixes per `SUB` socket: `16`

Sample config files:
- `data/basic.cfg`
- `data/publisher.cfg`
- `data/subscriber.cfg`
- `data/pusher.cfg`
- `data/puller.cfg`
- `docs/config/zcmproc.cfg`

## Example
```xml
<?xml version="1.0" encoding="UTF-8"?>
<procConfig>
  <process name="basic">
    <dataSocket type="PUB" payload="basic-pub" intervalMs="1000"/>
    <dataSocket type="SUB" targets="publisher" topics="publisher"/>
    <control timeoutMs="200"/>
    <handlers>
      <type name="QUERY">
        <arg kind="double"/>
        <arg kind="double"/>
        <arg kind="text"/>
        <arg kind="double"/>
      </type>
    </handlers>
  </process>
</procConfig>
```

Run:
```bash
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_proc data/basic.cfg
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/tools/zcm ping basic
./build/tools/zcm send basic -type QUERY -d 5 -d 7 -t action -d 0
```

PUSH/PULL quick run:
```bash
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_proc data/pusher.cfg
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_proc data/puller.cfg
```
Expected logs:
- `pusher`: `[PUSH pusher] started: ...`
- `puller`: `[PULL puller] connected ...` then `[PULL puller] received payload ...`

Additional generic config examples:
- `data/publisher.cfg` (`publisher` with `PUB`)
- `data/subscriber.cfg` (`subscriber` with `SUB` to `basic`)
- `data/pusher.cfg` (`pusher` with `PUSH`)
- `data/puller.cfg` (`puller` with `PULL` to `pusher`)
