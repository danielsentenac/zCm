\page tool_zcm zcm

List registered processes:
```bash
./build/tools/zcm names
```
`zcm names` prints a normalized table with these columns:
- `NAME`: registered node name
- `ENDPOINT`: data endpoint registered in broker
- `HOST`: node host (from broker metadata when available, else parsed from endpoint)
  - if `HOST` is an IP, CLI tries reverse-DNS resolution and prints hostname when PTR exists
- `ROLE`: inferred data role (`BROKER`, `PUB`, `SUB`, `PUSH`, `PULL`, combinations, `EXTERNAL`)
- `PUB_PORT`: first publisher data port exposed by node (`-` when unavailable)
- `PUSH_PORT`: first pusher data port exposed by node (`-` when unavailable)
- `PUB_BYTES`: publisher payload byte count
- `SUB_BYTES`: subscriber last received payload byte count
- `PUSH_BYTES`: pusher payload byte count
- `PULL_BYTES`: puller last received payload byte count

`zcm names` behavior details:
- If broker is not reachable, the command prints only:
  - `zcm: broker not reachable`
- Nodes registered without control metadata (plain `REGISTER`) are shown as `EXTERNAL`
  without probing `DATA_*` commands (prevents names timeout on non-`zcm_proc` nodes).
- Nodes exposing control metadata (`REGISTER_EX` + `DATA_*` commands) can show full
  `ROLE`, `*_PORT`, and `*_BYTES` values.
- Broker-side metric probing is local-host only; stale remote registrations do not
  block `LIST_EX` responses (remote nodes should report metrics via `METRICS`).
- For remote entries with control metadata (`REGISTER_EX` + PID), broker performs a
  quick control liveness check during `LIST`/`LIST_EX`/`LOOKUP`/`INFO`; unreachable
  stale entries are pruned automatically.

Kill (shutdown) a registered process:
```bash
./build/tools/zcm kill NAME
```
Control-command behavior:
- `zcm ping NAME` sends control `PING` and expects `REPLY/PONG`.
- `zcm kill NAME` sends control `KILL` and expects `REPLY/OK` before node exit.
- `zcm broker stop` sends broker control `SHUTDOWN` and expects `OK`.

Control endpoint resolution:
- `zcm kill`/`zcm ping` first use broker `ctrl_endpoint` metadata.
- When `ctrl_endpoint` is missing for a legacy TCP registration, the client applies
  fallback `tcp://host:(data_port+1)`.
- If control does not respond, the CLI retries on the registered data endpoint.
- `zcm_proc` publishes control metadata by default (`REGISTER_EX`), so kill/ping
  are directly routable without fallback in normal deployments.

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
  - `DATA_ROLE`
  - `DATA_PORT_PUB` (and optional legacy `DATA_PORT`)
  - `DATA_PAYLOAD_BYTES_PUB`
- If a bridge registers only with plain `REGISTER`, it is shown as `EXTERNAL`
  and byte/port columns stay `-`.

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
  - `<domain> <nameserver-host> <nameserver-port> <first-port> <range-size> <repository>`
  - Example:
    - `myplace 127.0.0.1 5555 7000 100 repo`

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

`zcm_proc` specific:

| Variable | Meaning |
| --- | --- |
| `ZCM_PROC_CONFIG_FILE` | XML config file override. |
| `ZCM_PROC_CONFIG_DIR` | Base directory used to resolve relative config file names. |
| `ZCM_PROC_CONFIG_SCHEMA` | XSD schema path override. |
| `ZCM_PROC_REANNOUNCE_MS` | Broker re-announce period in ms (default `1000`). |

Build-time variables (`cmake`/toolchain):

| Variable | Meaning |
| --- | --- |
| `ZCM_ZMQ_ROOT` | ZeroMQ prefix containing `include/` and `lib*/`. |
| `ZCM_ZMQ_INCLUDE_DIR` | Explicit directory containing `zmq.h`. |
| `ZCM_ZMQ_LIBRARY` | Explicit full path to `libzmq.so`/`libzmq.a`. |
| `PKG_CONFIG_PATH` | Optional path so `pkg-config` can resolve `libzmq`. |

Example exports (runtime):

```bash
export ZCMDOMAIN=Virgo
export ZCMROOT=/virgoDev/zCm/v0r1
export ZCMMGR=$ZCMROOT/mgr
```

Example exports (build):

```bash
export ZCM_ZMQ_ROOT=/virgoDev/zmq/v4r35/zeromq-4.3.5
export ZCM_ZMQ_INCLUDE_DIR=$ZCM_ZMQ_ROOT/include
export ZCM_ZMQ_LIBRARY=$ZCM_ZMQ_ROOT/src/.libs/libzmq.so
```
