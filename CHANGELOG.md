# Changelog

## Unreleased

- `zcm names` now normalizes subscriber endpoints and annotates subscriber roles as
  `SUB:<publisher>:<port>` when endpoint matching resolves targets.
- `SUB_BYTES` for subscriber rows is now populated from direct subscriber metrics when
  available, with endpoint-matched publisher-byte inference as fallback.
- Broker `LIST`/`LIST_EX` behavior was hardened to avoid blocking probe paths and
  unstable pruning side effects during names listing.
- `zcm names` client path now retries transient broker request failures before
  reporting broker offline.
