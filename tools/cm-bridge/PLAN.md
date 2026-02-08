# cm-bridge plan

Goal: translate legacy Cm/Fd/Fbs traffic into zCm.

## Phase 1: Connectivity
- Read `ZCMDOMAIN` and legacy `ZCmDomains` to find NameServer endpoint.
- Connect to Cm NameServer and query/monitor registrations.
- Connect to zcm-broker and register bridged endpoints.

## Phase 2: Protocol decoding
- Implement legacy `CmMessage` decoder (magic, header, item stream, XDR/Cvt).
- Map legacy message types to zcm-msg `type` strings.
- Map legacy typed items to zcm-msg typed items.

## Phase 3: Fd/Fbs mappings
- `FdFrame` -> zcm-msg with `BYTES` payload for frame buffer.
- `FdAddFrame`, `FdRemoveFrame`, `FdGetChannelsList`, etc.
- `FbSmsData`, `FbGetAllSmsData`, `FbConfigData`, Ti* messages.

## Phase 4: Reliability
- Reconnect on NameServer restart.
- Cache name->endpoint mappings.
- Metrics + logging.
