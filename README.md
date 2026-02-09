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

## Build
```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build
```
Verbose tests:
```bash
cmake --build build --target test-verbose
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
Unified process example:
```bash
./build/examples/zcm_proc pub-msg procpub 5
```

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
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_broker_main
```
Stop the broker with `Ctrl+C`, or remotely:
```bash
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_broker_ctl stop
```
Check broker status:
```bash
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_broker_ctl ping
```
List registered names:
```bash
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_broker_ctl list
```
List via API:
```bash
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_list
```

Unified `zcm_proc` modes:
```bash
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_proc pub-msg procpub 5
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_proc sub-msg procpub procsub 5
```

Raw bytes with the same unified process:
```bash
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_proc pub-bytes procbytes 5 raw-bytes-proc
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_proc sub-bytes procbytes procbytesub 5
```

Request/Reply with the same unified process:
```bash
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_proc rep echoservice -1
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_proc req echoservice echoclient 1
```

## Tests
See `docs/tests.md` for test coverage and intent.

## Status
Scaffolded with a working broker/registry + typed message envelope.
See `docs/zcm-proc.md`, `docs/zcm-msg.md`, and `docs/tests.md`.
