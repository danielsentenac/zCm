# zcm-proc

`zcm_proc` is a single unified example process executable.

Launch format:
```bash
./build/examples/zcm_proc <proc-config.cfg>
```

`zcm_proc` reads runtime behavior from the XML config file. Command-line mode
arguments are no longer used.

## Runtime model
- Every process has one name: `<process name="...">`.
- In `daemon` mode it is an infinite process using direct ZeroMQ `REQ/REP`
  semantics.
- Default control request/reply is `PING -> PONG`.
- Optional roles (publish/subscribe bytes or messages, request client) are
  selected by config.

## Config file
Validation:
- config schema: `$ZCM_PROC_CONFIG_SCHEMA` or `config/schema/proc-config.xsd`

Required nodes:
- `<process @name>`
- `<runtime @mode>`
- `<dataSocket @type @bind>`

Optional node:
- `<control @timeoutMs>`
- `<handlers>` (daemon/rep modes)

Runtime attributes:
- `mode`: `daemon`, `rep`, `pub-msg`, `sub-msg`, `pub-bytes`, `sub-bytes`, `req`
- `target`: required for `sub-msg`, `sub-bytes`, `req`
- `count`: optional (`-1` means infinite where supported)
- `payload`: optional for `pub-bytes` (default `raw-bytes-proc`)
- `request`: optional for `req` (default `PING`)

Handlers:
- `<handlers><core .../></handlers>` configures CORE request handling.
- `<handlers><type name="..." reply="..."><arg kind="..."/>...</type></handlers>` defines
  a strict ordered payload format for that TYPE.
- Supported arg kinds: `text`, `double`, `float`, `int`.
- `zcm send` keeps value flag order exactly as written (`-t/-d/-f/-i` sequence).
- Reply selection order in daemon/rep:
  1. matching TYPE handler
  2. CORE ping handler (`pingRequest -> pingReply`)
  3. CORE default reply (`defaultReply`)
- If a TYPE request payload does not match its declared arg sequence, the proc
  replies with `ERROR` and includes the expected format (example:
  `ERR malformed QUERY expected QUERY(double,double,text,double)`).

## Examples
Daemon (`PING -> PONG`):
```xml
<?xml version="1.0" encoding="UTF-8"?>
<procConfig>
  <process name="coco">
    <runtime mode="daemon"/>
    <dataSocket type="REP" bind="true"/>
    <control timeoutMs="200"/>
    <handlers>
      <core pingRequest="PING" pingReply="PONG" defaultReply="OK"/>
      <type name="ALARM" reply="ALARM_ACK">
        <arg kind="double"/>
      </type>
    </handlers>
  </process>
</procConfig>
```

Run:
```bash
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/examples/zcm_proc docs/config/coco.cfg
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/tools/zcm ping coco
ZCMDOMAIN=myplace ZCMROOT=/path/to/zcmroot ./build/tools/zcm send coco -t "hello"
```

Ordered TYPE payload example:
```bash
./build/tools/zcm send basic -type QUERY -d 5 -d 7 -t action -d 0
```

Message publisher:
```xml
<?xml version="1.0" encoding="UTF-8"?>
<procConfig>
  <process name="procpub">
    <runtime mode="pub-msg" count="5"/>
    <dataSocket type="PUB" bind="true"/>
  </process>
</procConfig>
```

Bytes subscriber:
```xml
<?xml version="1.0" encoding="UTF-8"?>
<procConfig>
  <process name="procbytesub">
    <runtime mode="sub-bytes" target="procbytes" count="-1"/>
    <dataSocket type="SUB" bind="false"/>
  </process>
</procConfig>
```

Request client:
```xml
<?xml version="1.0" encoding="UTF-8"?>
<procConfig>
  <process name="echoclient">
    <runtime mode="req" target="coco" count="1" request="PING"/>
    <dataSocket type="REQ" bind="false"/>
  </process>
</procConfig>
```
