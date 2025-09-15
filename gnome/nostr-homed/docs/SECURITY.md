# Security and Hardening (nostr-homed)

This document summarizes the security model, threat considerations, and systemd hardening applied to nostr-homed components.

## Components

- `nostr-homectl` (DBus control service): manages cache warm-up, secrets decrypt (via local signer), and session lifecycle.
- `nostrfs` (FUSE3 filesystem): materializes the home directory from the manifest and handles write-back to CAS + Blossom.
- `libnss_nostr.so.2` (NSS): resolves accounts and groups from a local SQLite cache only.
- `pam_nostr.so` (PAM): authenticates via the local signer (DBus), checks the cache, and opens/closes sessions.

## Threat model (high level)

- Local adversary cannot read decrypted secrets from disk: secrets are written to a tmpfs path (`/run/nostr-homed/secrets/secrets.json`) with mode 0600.
- FUSE mount isolated with a tight systemd sandbox; only `/dev/fuse` device is allowed.
- NSS and PAM modules do not perform network access in the hot path; all lookups come from a local SQLite cache.
- Publishing to relays is best-effort and rate-limited/debounced.

## Systemd hardening

### `systemd/nostr-homectl.service`

- `Type=simple`
- `NoNewPrivileges=true`
- `PrivateTmp=true`
- `PrivateDevices=true`
- `ProtectClock=true`
- `ProtectHostname=true`
- `ProtectKernelLogs=true`
- `ProtectKernelModules=true`
- `ProtectKernelTunables=true`
- `ProtectSystem=strict`
- `ProtectHome=read-only`
- `LockPersonality=true`
- `MemoryDenyWriteExecute=true`
- `RestrictSUIDSGID=true`
- `SystemCallArchitectures=native`
- `UMask=0077`

### `systemd/nostrfs@.service`

Environment defaults:
- `HOMED_NAMESPACE=personal`
- `SECRETS_NAMESPACE=personal`
- `BLOSSOM_BASE_URL=https://blossom.example.org`
- `RELAYS_DEFAULT=wss://relay.damus.io,wss://nostr.wine`

Exec and sandboxing:
- `ExecStart=/usr/bin/nostrfs --writeback --namespace=${HOMED_NAMESPACE} /home/%i`
- `NoNewPrivileges=true`
- `PrivateTmp=true`
- `PrivateDevices=true`
- `ProtectClock=true`
- `ProtectHostname=true`
- `ProtectKernelLogs=true`
- `ProtectKernelModules=true`
- `ProtectKernelTunables=true`
- `ProtectSystem=strict`
- `ProtectHome=read-only`
- `ProtectControlGroups=true`
- `LockPersonality=true`
- `MemoryDenyWriteExecute=true`
- `RestrictSUIDSGID=true`
- `RestrictNamespaces=true`
- `SystemCallArchitectures=native`
- `SystemCallFilter=@system-service`
- `UMask=0077`
- `DeviceAllow=/dev/fuse rwm`
- `CapabilityBoundingSet=CAP_SYS_ADMIN`
- `AmbientCapabilities=CAP_SYS_ADMIN`

Rationale:
- `CAP_SYS_ADMIN` is required for FUSE mounts; bounding set and ambient capabilities keep privilege minimal.
- `DeviceAllow=/dev/fuse rwm` restricts device access to `fuse` only.
- `ProtectSystem=strict`, `ProtectHome=read-only`, and other `Protect*` directives reduce filesystem and kernel surface.
- `SystemCallFilter=@system-service` limits available syscalls to a curated set suitable for system daemons.

## Secrets handling

- Secrets are fetched as encrypted envelopes (kind 30079) and decrypted via the local DBus signer.
- Decrypted content is written to `/run/nostr-homed/secrets/secrets.json` (tmpfs) with mode `0600`.
- Applications should read from the tmpfs path and avoid persisting decrypted data to disk.

## Network I/O

- Relay access (reads/writes) is done by `nostr-homectl` (warm cache) and `nostrfs` (best-effort publish). Both obey profile-configured relays (kind 30078) with fallback to `RELAYS_DEFAULT`.
- Blossom uploads are best-effort and verified via SHA-256 `cid` checks on read-through.

## Cache

- SQLite cache uses WAL journaling for durability.
- The manifest is written with a safe-replace strategy (`manifest.<ns>.tmp` → `manifest.<ns>`) to avoid partial reads.
- CAS quota enforced by `nostrfs` using an mtime-based LRU evictor; configurable via environment variables.

## Recommendations

- Keep `nostrfs@.service` and `nostr-homectl.service` unit files intact; avoid relaxing hardening unless necessary for your environment.
- Ensure the DBus signer is local and trusted; access control should be enforced according to your OS policy.
- Avoid exposing decrypted secrets beyond tmpfs; if an application needs persistent secrets, use envelope re-encryption and publish back to relays.
