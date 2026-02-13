# zCm

Lightweight messaging toolkit with a directory-service broker and direct peer-to-peer data paths. The broker maintains the registry (name → endpoint), while applications connect directly to exchange data after lookup. This matches the "broker as a directory service" architecture popularized in ØMQ/ZeroMQ patterns, where the broker handles discovery and peers handle transfer.

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

## API Docs Publish
Build and publish API docs to `gh-pages`:
```bash
./scripts/publish_api_docs.sh
```

Dry-run without pushing:
```bash
./scripts/publish_api_docs.sh --dry-run
```
