# zcm-proc

`zcm_proc` is a single unified process daemon.

Launch:
```bash
./build/examples/zcm_proc <proc-config.cfg>
```

## Behavior
- Every `zcm_proc` is an infinite daemon.
- It always answers requests over direct ZeroMQ `REQ/REP` semantics.
- Default command behavior is `PING -> PONG`.
- It periodically re-registers in broker so names recover after broker restart.
  - tune interval with `ZCM_PROC_REANNOUNCE_MS` (default `1000`)
- Optional repeated `dataSocket` entries configure bytes `PUB/SUB/PUSH/PULL` roles.

## Config
Validation schema:
- `$ZCM_PROC_CONFIG_SCHEMA` or `config/schema/proc-config.xsd`

Required:
- `<process @name>`

Optional:
- repeated `<dataSocket .../>` for `PUB/SUB/PUSH/PULL`
- `<control @timeoutMs>`
- `<handlers>`

`dataSocket` attributes:
- `type`: `PUB`, `SUB`, `PUSH`, or `PULL`
- `PUB`/`PUSH` port is auto-allocated from the current domain range
- `payload`: optional `PUB`/`PUSH` payload (default `tick`)
- `intervalMs`: optional `PUB`/`PUSH` period (default `1000`)
- `targets`: comma-separated source names for `SUB`/`PULL` (multi-target)
- `target`: optional single-target compatibility alias for `SUB`/`PULL`
- `topics`: optional `SUB`-only comma-separated topic prefixes
  (example: `topics="prefix1,prefix2"`). If omitted, `SUB` subscribes to all.

Handlers:
- builtin command behavior is fixed: `PING -> PONG` (default reply `OK`)
- `<type name="..."> <arg kind="..."/> ... </type>`
- `arg kind`: `text`, `double`, `float`, `int`
- TYPE payload order is strict.
- TYPE handler reply is built in user code and sent as typed message
  `"<REQ_TYPE>_RPL"` with any payload fields your handler writes.
- Malformed TYPE payload reply: `ERROR` with expected format.
- each `SUB` target discovers publisher port with command `DATA_PORT_PUB`
  (fallback alias: `DATA_PORT`).
- each `PULL` target discovers pusher port with command `DATA_PORT_PUSH`.

## Example
```xml
<?xml version="1.0" encoding="UTF-8"?>
<procConfig>
  <process name="basic">
    <dataSocket type="PUB" payload="basic-pub" intervalMs="1000"/>
    <dataSocket type="SUB" targets="publisher" topics="publisher"/>
    <control timeoutMs="200"/>
    <handlers>
      <type name="QUERY">
        <arg kind="double"/>
        <arg kind="double"/>
        <arg kind="text"/>
        <arg kind="double"/>
      </type>
    </handlers>
  </process>
</procConfig>
```

Run:
```bash
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_proc data/basic.cfg
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/tools/zcm ping basic
./build/tools/zcm send basic -type QUERY -d 5 -d 7 -t action -d 0
```

PUSH/PULL quick run:
```bash
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_proc data/pusher.cfg
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_proc data/puller.cfg
```
Expected logs:
- `pusher`: `[PUSH pusher] started: ...`
- `puller`: `[PULL puller] connected ...` then `[PULL puller] received payload ...`

Additional generic config examples:
- `data/publisher.cfg` (`publisher` with `PUB`)
- `data/subscriber.cfg` (`subscriber` with `SUB` to `basic`)
- `data/pusher.cfg` (`pusher` with `PUSH`)
- `data/puller.cfg` (`puller` with `PULL` to `pusher`)
