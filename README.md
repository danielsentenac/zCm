# zCm

Lightweight messaging toolkit with a directory-service broker and direct peer-to-peer data paths. The broker maintains the registry (name → endpoint), while applications connect directly to exchange data after lookup. This matches the "broker as a directory service" architecture popularized in [ØMQ/ZeroMQ](https://zeromq.org/) patterns, where the broker handles discovery and peers handle transfer.

Published docs: https://danielsentenac.github.io/zCm/

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

## Scope
- Broker/registry (`zcm-broker`) for name -> endpoint lookup
- Client API (`zcm-node`) to register and lookup
- Minimal typed message envelope (`zcm-msg`)
- [ØMQ/ZeroMQ](https://zeromq.org/) transport support (`tcp`, `ipc`, `inproc`)
- Direct peer-to-peer data transfer after broker lookup

## Domain-aware Broker Startup
When `ZCMDOMAIN` is set, zCm resolves broker startup from `ZCmDomains`:
- CLI-style queries use the advertised endpoint currently stored for the domain.
- `zcm_broker` binds a local endpoint on the current host using the same port.
- After a successful local bind, `zcm_broker` rewrites `ZCmDomains` to publish the new active host.
- If another broker is already serving the domain, `zcm_broker` exits cleanly instead of creating a duplicate instance.

This behavior is covered by the focused regression tests:
- `zcm_broker_bind_conflict`
- `zcm_domain_resolution`
- `zcm_broker_tool_singleton`

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

Custom ZeroMQ location (when `libzmq` is not in system paths):
```bash
# Option A: point CMake to a ZeroMQ prefix
cmake -S . -B build \
  -DZCM_ZMQ_ROOT=/opt/zeromq

# Option B: explicit include/library paths
cmake -S . -B build \
  -DZCM_ZMQ_INCLUDE_DIR=/opt/zeromq/include \
  -DZCM_ZMQ_LIBRARY=/opt/zeromq/lib/libzmq.so
```

If your ZeroMQ install provides `libzmq.pc`, `pkg-config` also works:
```bash
export PKG_CONFIG_PATH=/opt/zeromq/lib/pkgconfig:/opt/zeromq/lib64/pkgconfig:$PKG_CONFIG_PATH
```
