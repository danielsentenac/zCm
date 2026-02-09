# zcm-proc unified example

`zcm_proc` is a single example executable that can act as publisher, subscriber,
requester, or replier, for both typed messages and raw bytes.

## Naming convention
Use plain process identifiers (no dotted suffixes).

Examples:
- `procpub`
- `procsub`
- `procbytes`
- `procbytesub`
- `echoservice`
- `echoclient`

## Usage
```bash
./build/examples/zcm_proc pub-msg   [name] [count]
./build/examples/zcm_proc sub-msg   [target] [self_name] [count]
./build/examples/zcm_proc pub-bytes [name] [count] [payload]
./build/examples/zcm_proc sub-bytes [target] [self_name] [count]
./build/examples/zcm_proc rep       [name] [count|-1]
./build/examples/zcm_proc req       [service] [self_name] [count]
```

`count = -1` means infinite loop for modes that support it.

## Common flows
Message pub/sub:
```bash
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_proc pub-msg procpub 5
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_proc sub-msg procpub procsub 5
```

Bytes pub/sub:
```bash
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_proc pub-bytes procbytes 5 raw-bytes-proc
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_proc sub-bytes procbytes procbytesub 5
```

Request/reply:
```bash
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_proc rep echoservice -1
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_proc req echoservice echoclient 1
```
