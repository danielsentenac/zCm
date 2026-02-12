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
- `docs/modules.md` for High-Level vs Low-Level API modules.
- `docs/zcm-proc.md` for unified process example modes.
- `docs/zcm-msg.md` for message encoding format.
- `docs/tests.md` for test coverage.

## Tools
List registered processes:
```bash
./build/tools/zcm names
```
Output columns include `ROLE`, `PUB_PORT`, and `PUSH_PORT`.
Kill (shutdown) a registered process:
```bash
./build/tools/zcm kill NAME
```
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
Unified process executable:
```bash
./build/examples/zcm_proc <proc-config.cfg>
```

Process config at init (required):
- zcm_proc reads the XML file path passed on the command line.
- XML is validated against:
  - `$ZCM_PROC_CONFIG_SCHEMA`, else `config/schema/proc-config.xsd`
- `<process @name>` is the process registration name.
- `zcm_proc` is always an infinite daemon (no runtime mode).
- `zcm_proc` re-announces its registration periodically so names are restored if broker restarts.
  - interval can be tuned with `ZCM_PROC_REANNOUNCE_MS` (default `1000`)
- Optional repeated `<dataSocket>` configures bytes `PUB/SUB/PUSH/PULL`:
  - `type=PUB|SUB|PUSH|PULL`
  - `PUB`/`PUSH` auto-allocate a port from the current domain range and use optional `payload`, `intervalMs`
  - `SUB`/`PULL` use `targets=<proc-a,proc-b,...>` (or legacy `target=<proc-name>`)
  - `SUB` can define `topics=<prefix1,prefix2,...>` for topic-prefix filtering (default is all topics)
  - each `SUB` target publisher port is discovered via `DATA_PORT_PUB` (fallback: `DATA_PORT`)
  - each `PULL` target pusher port is discovered via `DATA_PORT_PUSH`
- Optional `<handlers>` adds request reply rules:
  - builtin command behavior is fixed: `PING -> PONG` (default reply `OK`)
  - repeated `<type name=...><arg kind=.../>...</type>` with ordered payload args
  - TYPE replies are built in handler code and sent as typed messages with name `<REQ_TYPE>_RPL`
  - malformed TYPE requests are rejected with `ERROR` and expected TYPE format
  - `zcm send` preserves the exact order of repeated payload flags
  - payload flags: `-t/-d/-f/-i/-c/-s/-l/-b/-a`
  - `-a` uses `kind:v1,v2,...` with `kind=char|short|int|float|double`
- Examples: `data/basic.cfg`, `data/publisher.cfg`, `data/subscriber.cfg`, `data/pusher.cfg`, `data/puller.cfg`, `docs/config/zcmproc.cfg`

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
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_proc docs/config/zcmproc.cfg
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/tools/zcm ping zcmproc
```

Typed send to a proc (`-t`, `-d`, `-f`, `-i`):
```bash
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/tools/zcm send zcmproc -type ZCM_CMD -t "hello"
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/tools/zcm send zcmproc -type ZCM_CMD -i 42
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/tools/zcm send zcmproc -type ZCM_CMD -f 3.14
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/tools/zcm send zcmproc -type ZCM_CMD -d 2.718281828
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/tools/zcm send zcmproc -type ZCM_CMD -l 99
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/tools/zcm send zcmproc -type ZCM_CMD -a short:1,2,3
```

Ordered TYPE payload example:
```bash
./build/tools/zcm send basic -type QUERY -d 5 -d 7 -t action -d 0
```

Generic data-path sample procs:
```bash
./build/examples/zcm_proc data/publisher.cfg
./build/examples/zcm_proc data/basic.cfg
./build/examples/zcm_proc data/subscriber.cfg
./build/examples/zcm_proc data/pusher.cfg
./build/examples/zcm_proc data/puller.cfg
```

## Status
Scaffolded with a working broker/registry + typed message envelope.
- [Process guide (`zcm_proc`)](docs/zcm-proc.md)
- [Message format (`zcm_msg`)](docs/zcm-msg.md)
- [Test suite and execution](docs/tests.md)
