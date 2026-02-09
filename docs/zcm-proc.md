# zcm-proc

`zcm_proc` is a single unified process daemon.

Launch:
```bash
./build/examples/zcm_proc <proc-config.cfg>
```

## Behavior
- Every `zcm_proc` is an infinite daemon.
- It always answers requests over direct ZeroMQ `REQ/REP` semantics.
- Default CORE behavior is `PING -> PONG`.
- Optional repeated `dataSocket` entries configure bytes `PUB/SUB` roles.

## Config
Validation schema:
- `$ZCM_PROC_CONFIG_SCHEMA` or `config/schema/proc-config.xsd`

Required:
- `<process @name>`

Optional:
- repeated `<dataSocket .../>` for `PUB/SUB`
- `<control @timeoutMs>`
- `<handlers>`

`dataSocket` attributes:
- `type`: `PUB` or `SUB`
- `port`: required for `PUB`
- `payload`: optional `PUB` payload (default `raw-bytes-proc`)
- `intervalMs`: optional `PUB` period (default `1000`)
- `targets`: comma-separated publisher names for `SUB` (multi-target)
- `target`: optional single-target compatibility alias for `SUB`

Handlers:
- `<core pingRequest="..." pingReply="..." defaultReply="..."/>`
- `<type name="..." reply="..."> <arg kind="..."/> ... </type>`
- `arg kind`: `text`, `double`, `float`, `int`
- TYPE payload order is strict.
- Malformed TYPE payload reply: `ERROR` with expected format.
- each `SUB` target discovers publisher port by sending default request `DATA_PORT`.

## Example
```xml
<?xml version="1.0" encoding="UTF-8"?>
<procConfig>
  <process name="basic">
    <dataSocket type="PUB" port="7301" payload="basic-pub" intervalMs="1000"/>
    <dataSocket type="SUB" targets="coco, sensorA, sensorB"/>
    <control timeoutMs="200"/>
    <handlers>
      <core pingRequest="PING" pingReply="PONG" defaultReply="OK"/>
      <type name="QUERY" reply="OK">
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
