# zcm-proc unified example

`zcm_proc` is a single example executable. By default it runs as an infinite
daemon using direct ZeroMQ `REQ/REP` semantics for requests.

Default request/response:
- `PING` -> `PONG`

The same executable can also be configured for optional message pub/sub and
bytes pub/sub roles.

## Naming convention
Use plain process identifiers (no dotted suffixes).

Examples:
- `procpub`
- `procsub`
- `procbytes`
- `procbytesub`
- `zcmproc`
- `echoclient`

## Usage
```bash
./build/examples/zcm_proc daemon    [name]
./build/examples/zcm_proc pub-msg   [name] [count|-1]
./build/examples/zcm_proc sub-msg   [target] [self_name] [count|-1]
./build/examples/zcm_proc pub-bytes [name] [count|-1] [payload]
./build/examples/zcm_proc sub-bytes [target] [self_name] [count|-1]
./build/examples/zcm_proc req       [service] [self_name] [count] [request]
./build/examples/zcm_proc rep       [name]   # alias for daemon
```

`count = -1` means infinite loop for modes that support it.

## Common flows
Daemon request/reply:
```bash
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_proc daemon zcmproc
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/tools/zcm ping zcmproc
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_proc req zcmproc echoclient 1 PING
```

Message pub/sub (optional):
```bash
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_proc pub-msg procpub 5
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_proc sub-msg procpub procsub 5
```

Bytes pub/sub (optional):
```bash
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_proc pub-bytes procbytes 5 raw-bytes-proc
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_proc sub-bytes procbytes procbytesub 5
```
