# Examples

Common shell exports:
```bash
export ZCMDOMAIN=myplace
export ZCMROOT=/path/to/zcmroot
export ZCMMGR=$ZCMROOT/mgr
```
Full variable reference: \subpage tool_zcm "zcm" (`Environment Variables` section).

`ZCmDomains` entry format used by all examples:
```text
<domain-name> <broker-host> <broker-port> <port-range-start> <port-range-size>
myplace 127.0.0.1 5555 7000 100
```
Field-by-field description: \subpage tool_zcm "zcm" (`Broker resolution for zcm CLI and broker` section).

Start a broker (endpoint via `ZCmDomains`):
```bash
ZCMDOMAIN=myplace ZCMMGR=/path/to/zcmroot/mgr ./build/tools/zcm_broker
```
Stop the broker with `Ctrl+C`, or remotely:
```bash
ZCMDOMAIN=myplace ZCMMGR=/path/to/zcmroot/mgr ./build/tools/zcm broker stop
```
Check broker status:
```bash
ZCMDOMAIN=myplace ZCMMGR=/path/to/zcmroot/mgr ./build/tools/zcm broker ping
```
List registered names:
```bash
ZCMDOMAIN=myplace ZCMMGR=/path/to/zcmroot/mgr ./build/tools/zcm broker list
```

Detailed names report (role, ports, payload bytes, host):
```bash
ZCMDOMAIN=myplace ZCMMGR=/path/to/zcmroot/mgr ./build/tools/zcm names
```

Default daemon request/reply (`PING` -> `PONG`):
```bash
ZCMDOMAIN=myplace ZCMMGR=/path/to/zcmroot/mgr ./build/examples/zcm_proc docs/config/zcmproc.cfg
ZCMDOMAIN=myplace ZCMMGR=/path/to/zcmroot/mgr ./build/tools/zcm ping zcmproc
```

Typed send to a proc (`-t`, `-d`, `-f`, `-i`):
```bash
ZCMDOMAIN=myplace ZCMMGR=/path/to/zcmroot/mgr ./build/tools/zcm send zcmproc -type ZCM_CMD -t "hello"
ZCMDOMAIN=myplace ZCMMGR=/path/to/zcmroot/mgr ./build/tools/zcm send zcmproc -type ZCM_CMD -i 42
ZCMDOMAIN=myplace ZCMMGR=/path/to/zcmroot/mgr ./build/tools/zcm send zcmproc -type ZCM_CMD -f 3.14
ZCMDOMAIN=myplace ZCMMGR=/path/to/zcmroot/mgr ./build/tools/zcm send zcmproc -type ZCM_CMD -d 2.718281828
ZCMDOMAIN=myplace ZCMMGR=/path/to/zcmroot/mgr ./build/tools/zcm send zcmproc -type ZCM_CMD -l 99
ZCMDOMAIN=myplace ZCMMGR=/path/to/zcmroot/mgr ./build/tools/zcm send zcmproc -type ZCM_CMD -a short:1,2,3
```

Ordered TYPE payload example:
```bash
./build/tools/zcm send basic -type QUERY -d 5 -d 7 -t action -d 0
```

Generic data-path sample procs:
```bash
./build/examples/zcm_proc data/publisher.cfg
./build/examples/zcm_proc data/basic.cfg
./build/examples/zcm_proc data/subscriber.cfg
./build/examples/zcm_proc data/pusher.cfg
./build/examples/zcm_proc data/puller.cfg
```

`zcm_cli_workflow`-style run with real daemons:
```bash
# terminal 1
ZCMDOMAIN=myplace ZCMMGR=/path/to/zcmroot/mgr ./build/tools/zcm_broker

# terminal 2
ZCMDOMAIN=myplace ZCMMGR=/path/to/zcmroot/mgr ./build/examples/zcm_proc data/publisher.cfg

# terminal 3
ZCMDOMAIN=myplace ZCMMGR=/path/to/zcmroot/mgr ./build/examples/zcm_proc data/basic.cfg

# terminal 4
ZCMDOMAIN=myplace ZCMMGR=/path/to/zcmroot/mgr ./build/examples/zcm_proc data/subscriber.cfg
```
Then inspect the names table and exercise the control path:
```bash
ZCMDOMAIN=myplace ZCMMGR=/path/to/zcmroot/mgr ./build/tools/zcm names
ZCMDOMAIN=myplace ZCMMGR=/path/to/zcmroot/mgr ./build/tools/zcm ping publisher
ZCMDOMAIN=myplace ZCMMGR=/path/to/zcmroot/mgr ./build/tools/zcm ping basic
ZCMDOMAIN=myplace ZCMMGR=/path/to/zcmroot/mgr ./build/tools/zcm ping subscriber
ZCMDOMAIN=myplace ZCMMGR=/path/to/zcmroot/mgr ./build/tools/zcm send basic -type QUERY -d 5 -d 7 -t action -d 0
ZCMDOMAIN=myplace ZCMMGR=/path/to/zcmroot/mgr ./build/tools/zcm kill subscriber
ZCMDOMAIN=myplace ZCMMGR=/path/to/zcmroot/mgr ./build/examples/zcm_proc data/subscriber.cfg
ZCMDOMAIN=myplace ZCMMGR=/path/to/zcmroot/mgr ./build/tools/zcm kill publisher
ZCMDOMAIN=myplace ZCMMGR=/path/to/zcmroot/mgr ./build/tools/zcm broker stop
```
To complete the recovery part of `zcm_cli_workflow`, start the broker again in terminal 1, relaunch `publisher` in terminal 2, then run:
```bash
ZCMDOMAIN=myplace ZCMMGR=/path/to/zcmroot/mgr ./build/tools/zcm names
ZCMDOMAIN=myplace ZCMMGR=/path/to/zcmroot/mgr ./build/tools/zcm send basic -type QUERY -d 1 -d 2 -t again -d 3
```
This is the same topology covered by `zcm_cli_workflow`: `publisher` is `PUB`, `subscriber` is `SUB`, and `basic` is a combined `PUB`/`SUB` proc. After the daemons exchange traffic, `zcm names` should show the role/port columns plus `PUB_BYTES` and `SUB_BYTES` for the active data path, and after the broker restart the names/query workflow should recover once `publisher` comes back.

To configure one `zcm_proc` as both publisher and subscriber, declare both data sockets in the same `<process>` block, as in `data/basic.cfg`:
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
In this configuration, `basic` publishes `basic-pub` every second, subscribes to payloads coming from `publisher`, and still answers control requests such as `PING`, `KILL`, and typed `QUERY` messages.
