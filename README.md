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
Generate API documentation locally with Doxygen:
```bash
doxygen Doxyfile
```
Generated HTML:
```bash
docs/_build/html/index.html
```

GitHub Pages deployment is automated by `.github/workflows/doxygen-gh-pages.yml`.
On every push to `master`/`main` (or manual workflow dispatch), docs are regenerated and published to the `gh-pages` branch.
Set repository Pages source to branch `gh-pages` (root) to serve the generated site.

## Tools
List registered processes:
```bash
./build/tools/zcm names
```
Kill (shutdown) a registered process:
```bash
./build/tools/zcm kill --name NAME
```
Process initialization (register + control endpoint):
```bash
./build/examples/zcm_proc_example proc.example
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

Typed message pub/sub:
```bash
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_pub vacuum.pub
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_sub vacuum.pub vacuum.sub
```

Raw bytes pub/sub:
```bash
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_bytes_pub vacuum.bytes
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_bytes_sub vacuum.bytes vacuum.bytes.sub
```

Process init API:
```bash
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_proc_example proc.example
```

Request/Reply example:
```bash
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_rep echo.service
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_req echo.service echo.client
```

## Tests
See `docs/tests.md` for test coverage and intent.

## Status
Scaffolded with a working broker/registry + typed message envelope.
See `docs/legacy-compat.md` and `docs/zcm-msg.md`.
