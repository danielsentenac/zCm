\page tool_zcm zcm

List registered processes:
```bash
./build/tools/zcm names
```
Output columns include `ROLE`, `PUB_PORT`, `PUSH_PORT`, and payload-byte columns:
`PUB_BYTES`, `SUB_BYTES`, `PUSH_BYTES`, `PULL_BYTES`.

Kill (shutdown) a registered process:
```bash
./build/tools/zcm kill NAME
```
Ping a registered process (control REQ/REP):
```bash
./build/tools/zcm ping NAME
```
Send a typed message (explicit type required):
```bash
./build/tools/zcm send NAME -type ZCM_CMD -t "hello"
./build/tools/zcm send NAME -type ZCM_CMD -i 42
./build/tools/zcm send NAME -type ZCM_CMD -f 3.14
./build/tools/zcm send NAME -type ZCM_CMD -d 2.718281828
./build/tools/zcm send NAME -type ZCM_CMD -c A
./build/tools/zcm send NAME -type ZCM_CMD -s 123
./build/tools/zcm send NAME -type ZCM_CMD -l 1234567890123
./build/tools/zcm send NAME -type ZCM_CMD -b "raw-bytes"
./build/tools/zcm send NAME -type ZCM_CMD -a int:1,2,3
./build/tools/zcm send NAME -type CustomType -t "hello"
./build/tools/zcm send basic -type QUERY -d 5 -d 7 -t action -d 0
```

Broker resolution for `zcm` CLI and broker:
- `ZCMDOMAIN` selects the domain
- `ZCmDomains` is read from:
  - `$ZCMDOMAIN_DATABASE` or `$ZCMMGR`, else
  - `$ZCMROOT/mgr`
- Line format:
  - `<domain> <nameserver-host> <nameserver-port> <first-port> <range-size> <repository>`
