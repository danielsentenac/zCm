# Legacy Compatibility Notes (Cm/Fd/Fbs)

This document captures the minimum legacy surface we need to interoperate with the current Virgo DAQ stack. It is based on the Cm/Fd/Fbs sources provided in `/home/sentenac/DOCS`.

## Cm NameServer (Broker/Registry)
- The NameServer process is `CmNameServer`.
- It reads `ZCmDomains` from `$ZCMDOMAIN_DATABASE` or `$ZCMMGR` or `$ZCMROOT/mgr`.
- The selected domain is set by `ZCMDOMAIN` and the line format is:
  - `<domain> <nameserver-host> <nameserver-port> <first-port> <range-size> <repository>`
- NameServer message types handled:
  - `NSGetAddress`, `NSGetPort`, `NSGetNewPort`, `NSGetNames`, `NSGetConnects`,
    `NSGetComments`, `NSGetStatistics`, `NSReconnect`, `NSGetPorts`,
    `NSGetPeriod`, `NSSetPeriod`, `NSDoCleanup`, `NSStop`, `NSRestart`,
    `NSForceFault`, `NSSetBlind`, `NSGetVersion`.

## CmMessage Wire Format (Legacy)
- `MAGIC_WORD` = `0xDEADDEAD` in both header and tail.
- Header fields:
  - `swap` (endian swap info), `magic`, `length`, `tail` (offset), `type` (16 chars, truncated).
- Tail is a single magic marker.
- Item stream is self-describing:
  - Item type byte followed by an alignment byte (`synchro`) and padding to 8-byte alignment.
  - Supported item types: char, short, int, long (string), float, double, array, text, bytes, tail.
- Arrays store: `type`, `elements`, followed by aligned element data.
- Text is stored as bytes with length and NUL terminator.
- Encoding uses `Cvt` to manage endianness.

## Fd (Frame Distribution) Cm Message Types
Common message types seen in `FdUtil.c` and `FdCm.c`:
- `FdFrame`
  - Payload fields (order):
    1) frame name (text)
    2) old run number (int)
    3) old frame number (int)
    4) GTimeS (int)
    5) GTimeN (int)
    6) trigger number (int)
    7) nBytes (int)
    8) frame bytes (array)
    9) frame dt (double)
- `FdAddFrame`
  - destination (text), tag (text), queueSize (int), nRetry (int)
- `FdRemoveFrame`
  - serverName (text)
- `FdGetFrameSet`
  - destName (text), tag (text), gpsStart (int), nSeconds (int), queueSize (int)
- `FdGetChannelsList`
  - gps (int)
- `FdChannelsList`
  - gps (int), channel lists for ADC/SER/PROC/SIM (text + count per list)
- Other control types used in `FdCm.c`:
  - `FdAddCmOutput`, `FdRemoveCmOutput`
  - `FdAddFrameMergerSource`, `FdRemoveFrameMergerSource`

## Fbs (Slow Frame Builder) Cm Message Types
From `Fbs.h` and `Fbs.c`:
- Config/registration:
  - `FbAddConfig`, `FbRemoveConfig`, `FbAddSms`, `FbRemoveSms`
- Sms data exchange:
  - `FbSms`, `FbSmsData`, `FbGetAllSmsData`
- Config queries:
  - `FbConfigData`, `FbGetConfig`
- Misc:
  - `CmRequestPrint`, `CmPrint`
  - `TiIrq`, `TiAskForIrq`, `reloadConfig`
  - `FbsAskForIrq`, `FbsAskForIrqOnce`, `FbsIrq`

