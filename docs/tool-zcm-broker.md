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
  - `<domain-name> <broker-host> <broker-port> <port-range-start> <port-range-size>`
  - Example:
    - `myplace 127.0.0.1 5555 7000 100`
- Field meanings:
  - `domain-name`: lookup key selected by `ZCMDOMAIN`.
  - `broker-host`: host/IP used to build `tcp://<broker-host>:<broker-port>`.
  - `broker-port`: broker bind/lookup port.
  - `port-range-start`: first port in the dynamic bind window used by `zcm_proc`.
  - `port-range-size`: number of ports in that dynamic bind window.
  - defaults for `zcm_proc` bind window when missing/invalid:
    - `port-range-start=7000`
    - `port-range-size=100`

No trailing `repository`/`repo` field is used by runtime components.

Broker startup behavior:
- `zcm_broker` binds on the selected port using the local machine address.
- After successful startup, it updates the current `ZCMDOMAIN` row in `ZCmDomains`:
  - `broker-host` -> detected local IPv4/hostname
  - `broker-port` -> active broker port
  - row is normalized to 5 fields:
    - `<domain-name> <broker-host> <broker-port> <port-range-start> <port-range-size>`

Broker runtime environment variables:

| Variable | Meaning |
| --- | --- |
| `ZCM_BROKER_REMOTE_PROBE_INTERVAL_MS` | Interval for remote registration liveness probes (default `3000`, valid `250..120000`). |
| `ZCM_BROKER_REMOTE_PROBE_FAILS` | Consecutive failed probes before dropping a stale remote entry (default `3`, valid `1..20`). |
| `ZCM_BROKER_TRACE_REG` | When truthy, enables register/unregister trace logs (`0`/`false`/`no` disables). |
