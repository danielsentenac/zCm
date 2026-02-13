# Examples

Common shell exports:
```bash
export ZCMDOMAIN=myplace
export ZCMROOT=/path/to/zcmroot
export ZCMMGR=$ZCMROOT/mgr
```
Full variable reference: \subpage tool_zcm "zcm" (`Environment Variables` section).

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
