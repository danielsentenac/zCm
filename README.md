# zCm

Lightweight messaging toolkit with a directory-service broker and direct peer-to-peer data paths. The broker maintains the registry (name → endpoint), while applications connect directly to exchange data after lookup. This matches the "broker as a directory service" architecture popularized in ØMQ/ZeroMQ patterns, where the broker handles discovery and peers handle transfer.

## Architecture
zCm separates discovery from data transfer:
- **Broker (directory service)**: maintains a registry of `name → endpoint` and answers lookup queries.
- **Applications (peers)**: register themselves, then connect directly to each other for data exchange.

This keeps the broker lightweight and avoids routing all payloads through a central point. The general flow is:
1. App A registers its name and endpoint with the broker.
2. App B queries the broker for App A’s endpoint.
3. App B connects directly to App A and sends data.

This pattern gives centralized manageability (single place to discover who is running) while keeping high-performance data paths.

Diagram (ASCII):
```
   +---------+            +---------+
   |  App B  |  lookup    | Broker  |
   +---------+ ---------->+---------+
        |                     |
        | endpoint for App A  |
        <---------------------+
        |
        | direct connect + send
        v
   +---------+
   |  App A  |
   +---------+
```

## v0.1 Scope
- Broker/registry (`zcm-broker`) for name -> endpoint lookup
- Client API (`zcm-node`) to register and lookup
- Minimal typed message envelope (`zcm-msg`)
- ZeroMQ transport support (`tcp`, `ipc`, `inproc`)
- Direct peer-to-peer data transfer after broker lookup

## API Layers
- High-level service/process lifecycle APIs: broker + proc (`include/zcm/zcm.h`, `include/zcm/zcm_proc.h`, `src/high-level/`).
- Low-level building blocks: node registry, sockets, and message envelope (`include/zcm/zcm_node.h`, `include/zcm/zcm_msg.h`, `src/low-level/`).

## Build
```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build
```
Verbose tests:
```bash
cmake --build build --target test_results
```

## Documentation
Published docs: https://danielsentenac.github.io/zCm/
Local docs:
- `docs/zcm-proc.md` for unified process example modes.
- `docs/zcm-msg.md` for message encoding format.
- `docs/tests.md` for test coverage.

## Tools
List registered processes:
```bash
./build/tools/zcm names
```
Kill (shutdown) a registered process:
```bash
./build/tools/zcm kill NAME
```
Ping a registered process (control REQ/REP):
```bash
./build/tools/zcm ping NAME
```
Send a typed core message:
```bash
./build/tools/zcm send NAME -t "hello"
./build/tools/zcm send NAME -i 42
./build/tools/zcm send NAME -f 3.14
./build/tools/zcm send NAME -d 2.718281828
./build/tools/zcm send NAME -type CustomType -t "hello"
```
Run broker main loop:
```bash
./build/tools/zcm_broker
```
Check broker status:
```bash
./build/tools/zcm broker ping
```
Stop broker:
```bash
./build/tools/zcm broker stop
```
List broker registry:
```bash
./build/tools/zcm broker list
```
Unified process daemon:
```bash
./build/examples/zcm_proc daemon zcmproc
```

Process config at init (required):
- zcm_proc loads `NAME.cfg` as XML from:
  - `$ZCM_PROC_CONFIG_DIR`, else current directory (`.`)
- XML is validated against:
  - `$ZCM_PROC_CONFIG_SCHEMA`, else `docs/config/proc-config.xsd`
- Examples: `data/basic.cfg`, `docs/config/coco.cfg`, `docs/config/zcmproc.cfg`

Broker resolution for `zcm` CLI and broker:
- `ZCMDOMAIN` selects the domain
- `ZCmDomains` is read from:
  - `$ZCMDOMAIN_DATABASE` or `$ZCMMGR`, else
  - `$ZCMROOT/mgr`
- Line format:
  - `<domain> <nameserver-host> <nameserver-port> <first-port> <range-size> <repository>`

## Examples
Start a broker (endpoint via `ZCmDomains`):
```bash
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/tools/zcm_broker
```
Stop the broker with `Ctrl+C`, or remotely:
```bash
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/tools/zcm broker stop
```
Check broker status:
```bash
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/tools/zcm broker ping
```
List registered names:
```bash
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/tools/zcm broker list
```

Default daemon request/reply (`PING` -> `PONG`):
```bash
cp docs/config/zcmproc.cfg ./zcmproc.cfg
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_proc daemon zcmproc
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/tools/zcm ping zcmproc
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_proc req zcmproc echoclient 1 PING
```

Optional message pub/sub modes:
```bash
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_proc pub-msg procpub 5
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_proc sub-msg procpub procsub 5
```

Optional bytes pub/sub modes:
```bash
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_proc pub-bytes procbytes 5 raw-bytes-proc
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_proc sub-bytes procbytes procbytesub 5
```

## Tests
See `docs/tests.md` for test coverage and intent.

## Status
Scaffolded with a working broker/registry + typed message envelope.
See `docs/zcm-proc.md`, `docs/zcm-msg.md`, and `docs/tests.md`.
