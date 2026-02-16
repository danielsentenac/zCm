\page tool_zcm_broker zcm_broker

\section tool_zcm_broker_cli zcm broker commands

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

\section tool_zcm_broker_daemon zcm_broker daemon

Run broker main loop:
```bash
./build/tools/zcm_broker
```

Broker resolution for `zcm_broker`:
- Optional explicit override:
  - `ZCMBROKER` or `ZCMBROKER_ENDPOINT` (takes priority)
- `ZCMDOMAIN` selects the domain
- `ZCmDomains` is read from:
  - `$ZCMDOMAIN_DATABASE` or `$ZCMMGR`, else
  - `$ZCMROOT/mgr`
- `ZCmDomains` stands for the zCm domain registry file:
  - one line per domain
  - each line defines broker endpoint and dynamic data-port allocation range
- Why this is convenient on non-broadcast networks:
  - discovery is file-based (no multicast/broadcast dependency)
  - all nodes read the same broker/port-range mapping
  - with a shared network file server, configuration stays consistent across hosts
  - in segmented/restricted networks this gives deterministic startup and lookup
- Who uses it:
  - `zcm_broker` (startup endpoint/range)
  - `zcm broker ping|stop|list` (broker lookup)
  - all `zcm` commands (`names`, `ping`, `send`, `kill`, ...)
  - `zcm_proc` startup/registration and runtime data-worker port allocation
- Line format:
  - `<domain> <nameserver-host> <nameserver-port> <first-port> <range-size> <repository>`
  - Example:
    - `myplace 127.0.0.1 5555 7000 100 repo`

Broker startup behavior:
- `zcm_broker` binds on the selected port using the local machine address.
- After successful startup, it updates the current `ZCMDOMAIN` row in `ZCmDomains`:
  - nameserver host -> detected local IPv4/hostname
  - nameserver port -> active broker port
