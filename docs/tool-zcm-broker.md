\page tool_zcm_broker zcm_broker

Run broker main loop:
```bash
./build/tools/zcm_broker
```

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

Broker resolution for `zcm_broker` and `zcm broker` commands:
- `ZCMDOMAIN` selects the domain
- `ZCmDomains` is read from:
  - `$ZCMDOMAIN_DATABASE` or `$ZCMMGR`, else
  - `$ZCMROOT/mgr`
- Line format:
  - `<domain> <nameserver-host> <nameserver-port> <first-port> <range-size> <repository>`
