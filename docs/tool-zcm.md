\page tool_zcm zcm

List registered processes:
```bash
./build/tools/zcm names
```
`zcm names` prints a normalized table with these columns:
- `NAME`: registered node name
- `ENDPOINT`: data endpoint registered in broker
  - for `sub://host:port` registrations, CLI shows normalized `tcp://host:port`
- `HOST`: node host (from broker metadata when available, else parsed from endpoint)
  - if `HOST` is an IP, CLI tries reverse-DNS resolution and prints hostname when PTR exists
- `ROLE`: inferred data role (`BROKER`, `PUB`, `SUB`, `PUSH`, `PULL`, combinations, `EXTERNAL`)
  - subscriber rows can be annotated as `SUB:<name:port[,name:port...]>` when endpoint matching resolves targets
- `PUB_PORT`: first publisher data port exposed by node (`-` when unavailable)
- `PUSH_PORT`: first pusher data port exposed by node (`-` when unavailable)
- `PUB_BYTES`: publisher payload byte count
- `SUB_BYTES`: subscriber last received payload byte count
  - when subscriber metric is unavailable, CLI can infer it from matched publisher `PUB_BYTES`
- `PUSH_BYTES`: pusher payload byte count
- `PULL_BYTES`: puller last received payload byte count

`zcm names` behavior details:
- If broker is not reachable, the command prints only:
  - `zcm: broker not reachable`
- `zcm names` retries transient broker failures before reporting offline.
- Nodes are expected to register with `REGISTER_EX` metadata.
- Nodes exposing control metadata and `DATA_*` commands can show full
  `ROLE`, `*_PORT`, and `*_BYTES` values.
- `zcm names` queries each node with a single typed control command:
  - `DATA_METRICS`
  - expected reply format:
    - `ROLE=<...>;PUB_PORT=<...>;PUSH_PORT=<...>;PUB_BYTES=<...>;SUB_BYTES=<...>;PUSH_BYTES=<...>;PULL_BYTES=<...>;SUB_TARGETS=<...>;SUB_TARGET_BYTES=<...>`
  - for unsupported fields, nodes should return `-` (not silence/timeouts).
- Broker-side metric probing is local-host only; stale remote registrations do not
  block `LIST_EX` responses (remote nodes should report metrics via `METRICS`).
- For remote entries with control metadata (`REGISTER_EX` + PID), broker performs a
  quick control liveness check during `INFO`; unreachable stale entries are pruned.
- `LIST`/`LIST_EX` are kept read-only and avoid stale remote prune side effects.
- For `sub://host:port` registrations, CLI cross-references matching `tcp://host:port`
  entries to display subscriber target names in `ROLE` and a normalized `ENDPOINT`.
- For `tcp://host:port` rows inferred as subscriber-side, CLI can also
  resolve `ROLE` target annotation by endpoint matching against publisher entries.

Kill (shutdown) a registered process:
```bash
./build/tools/zcm kill NAME
```
Control-command behavior:
- `zcm ping NAME` sends control `PING` and expects `REPLY/PONG`.
- `zcm kill NAME` sends control `KILL` and expects `REPLY/OK` before node exit.
- `zcm broker stop` sends broker control `STOP` and expects `zcm_broker: stopped`.

Control endpoint resolution:
- `zcm kill`/`zcm ping` first use broker `ctrl_endpoint` metadata.
- If control does not respond, the CLI retries on the registered data endpoint.
- `zcm_proc` publishes control metadata by default (`REGISTER_EX`).

Discovery/re-registration:
- `zcm names` uses broker registry state.
- After broker restart, `zcm_proc` nodes re-register automatically
  (`ZCM_PROC_REANNOUNCE_MS`, default `1000` ms), and names/ping/kill become
  available again once discovery converges.

Ping a registered process (control REQ/REP):
```bash
./build/tools/zcm ping NAME
```
Send a typed message (explicit type required):
```bash
./build/tools/zcm send NAME -type ZCM_CMD -t "hello"
./build/tools/zcm send NAME -type ZCM_CMD -i 42
./build/tools/zcm send NAME -type ZCM_CMD -f 3.14
./build/tools/zcm send NAME -type ZCM_CMD -d 2.718281828
./build/tools/zcm send NAME -type ZCM_CMD -c A
./build/tools/zcm send NAME -type ZCM_CMD -s 123
./build/tools/zcm send NAME -type ZCM_CMD -l 1234567890123
./build/tools/zcm send NAME -type ZCM_CMD -b "raw-bytes"
./build/tools/zcm send NAME -type ZCM_CMD -a int:1,2,3
./build/tools/zcm send NAME -type CustomType -t "hello"
./build/tools/zcm send basic -type QUERY -d 5 -d 7 -t action -d 0
```

Example `names` output:
```text
NAME       ENDPOINT                  HOST                       ROLE  PUB_PORT  PUSH_PORT  PUB_BYTES  SUB_BYTES  PUSH_BYTES  PULL_BYTES
---------  ------------------------  -------------------------  ----  --------  ---------  ---------  ---------  ----------  ----------
publisher  tcp://90.147.137.35:7002  olserver135.virgo.infn.it  PUB   7002      -          128        -          -           -
zcmbroker  tcp://90.147.137.35:5555  olserver135.virgo.infn.it  BROKER -         -          -          -          -           -
```

External nodes:
- To appear as `PUB` with `PUB_PORT`/`PUB_BYTES`, register with `REGISTER_EX`
  and expose a control endpoint that replies to:
  - `DATA_METRICS`
- If a bridge does not implement `DATA_*` replies, byte/port columns stay `-`.

Broker resolution for `zcm` CLI and broker:
- Optional explicit override:
  - `ZCMBROKER` or `ZCMBROKER_ENDPOINT` (takes priority)
- `ZCMDOMAIN` selects the domain
- `ZCmDomains` is read from:
  - `$ZCMDOMAIN_DATABASE` or `$ZCMMGR`, else
  - `$ZCMROOT/mgr`
- `ZCmDomains` is the zCm domain registry file (broker endpoint + port range per domain).
- Used by `zcm`, `zcm_broker`, and `zcm_proc`.
- Line format:
  - `<domain-name> <broker-host> <broker-port> <port-range-start> <port-range-size>`
  - Example:
    - `myplace 127.0.0.1 5555 7000 100`
- Field reference (all consumers):

| Field | Required | Used by | Meaning |
| --- | --- | --- | --- |
| `domain-name` | yes | all | Lookup key matched against `ZCMDOMAIN`. |
| `broker-host` | yes | `zcm`, `zcm_broker`, `zcm_proc`, `zFd`, `zFbs`, `ZcmPubBridge`, `jchv/zJchv` | Hostname or IP used to build broker endpoint `tcp://<broker-host>:<broker-port>`. |
| `broker-port` | yes | `zcm`, `zcm_broker`, `zcm_proc`, `zFd`, `zFbs`, `ZcmPubBridge`, `jchv/zJchv` | Broker port (expected TCP `1..65535`). |
| `port-range-start` | recommended | `zcm_proc` (bind allocation) | First candidate port for local data/control binding. If missing or invalid, defaults to `7000`. |
| `port-range-size` | recommended | `zcm_proc` (bind allocation) | Number of sequential ports scanned from `port-range-start`. If missing or invalid, defaults to `100`. |

No trailing `repository`/`repo` field is used by runtime components.

## Environment Variables (export reference)

Shared runtime variables (`zcm`, `zcm_broker`, `zcm_proc`):

| Variable | Meaning |
| --- | --- |
| `ZCMBROKER` | Explicit broker endpoint override (for example `tcp://host:5555`), highest priority. |
| `ZCMBROKER_ENDPOINT` | Alias of `ZCMBROKER`. |
| `ZCMDOMAIN` | Domain name to select the row in `ZCmDomains` (required). |
| `ZCMDOMAIN_DATABASE` | Directory containing `ZCmDomains` (highest priority). |
| `ZCMMGR` | Alternative directory containing `ZCmDomains`. |
| `ZCMROOT` | Fallback root; `ZCmDomains` read from `$ZCMROOT/mgr/ZCmDomains`. |

`zcm` CLI specific:

| Variable | Meaning |
| --- | --- |
| `ZCM_NAMES_QUERY_TIMEOUT_MS` | Per-query timeout used by `zcm names` when probing nodes for metrics (default `1000`, valid `10..5000`). |
| `ZCM_NAMES_QUERY_ATTEMPTS` | Retry attempts used by `zcm names` for broker/node query steps (default `3`, valid `1..10`). |

`zcm_proc` specific:

| Variable | Meaning |
| --- | --- |
| `ZCM_PROC_CONFIG_FILE` | XML config file override. |
| `ZCM_PROC_CONFIG_DIR` | Base directory used to resolve relative config file names. |
| `ZCM_PROC_CONFIG_SCHEMA` | XSD schema path override. |
| `ZCM_PROC_REANNOUNCE_MS` | Broker re-announce base period in ms (default `1000`, valid `100..60000`). |
| `ZCM_PROC_REANNOUNCE_BACKOFF_MAX_MS` | Maximum exponential backoff delay for re-announce retries (default `30000`, valid `1000..300000`). |
| `ZCM_PROC_ADVERTISED_HOST` | Host/IP advertised in broker registration endpoint metadata. |
| `ZCM_ADVERTISED_HOST` | Compatibility alias used when `ZCM_PROC_ADVERTISED_HOST` is not set. |
| `ZCM_PROC_RX_STALE_MS` | Staleness window for `SUB/PULL` receive-byte metrics before reporting `0` (default `5000`, valid `0..600000`; `0` disables staleness aging). |

`zcm_broker` specific:

| Variable | Meaning |
| --- | --- |
| `ZCM_BROKER_REMOTE_PROBE_INTERVAL_MS` | Interval for remote registration liveness probes (default `3000`, valid `250..120000`). |
| `ZCM_BROKER_REMOTE_PROBE_FAILS` | Consecutive failed probes before dropping a stale remote entry (default `3`, valid `1..20`). |
| `ZCM_BROKER_TRACE_REG` | When truthy, enables broker register/unregister trace logs (`0`/`false`/`no` disables). |

Build-time variables (`cmake`/toolchain):

| Variable | Meaning |
| --- | --- |
| `ZCM_ZMQ_ROOT` | ZeroMQ prefix containing `include/` and `lib*/`. |
| `ZCM_ZMQ_INCLUDE_DIR` | Explicit directory containing `zmq.h`. |
| `ZCM_ZMQ_LIBRARY` | Explicit full path to `libzmq.so`/`libzmq.a`. |
| `PKG_CONFIG_PATH` | Optional path so `pkg-config` can resolve `libzmq`. |

Example exports (runtime):

```bash
export ZCMDOMAIN=myplace
export ZCMROOT=/path/to/zcmroot
export ZCMMGR=$ZCMROOT/mgr
```

Example exports (build):

```bash
export ZCM_ZMQ_ROOT=/path/to/zeromq
export ZCM_ZMQ_INCLUDE_DIR=$ZCM_ZMQ_ROOT/include
export ZCM_ZMQ_LIBRARY=$ZCM_ZMQ_ROOT/lib/libzmq.so
```
