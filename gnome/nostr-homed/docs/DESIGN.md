# Design: nostr-homed

This document describes the architecture and flows for the Nostr-backed roaming home directory and login stack.

## Components

- libnss_nostr (NSS)
  - Resolves users/groups from a local SQLite cache only.
  - Provides getpwnam_r/getpwuid_r/getgrnam_r/getgrgid_r.
  - No network on hot path.

- pam_nostr (PAM)
  - Auth: requests a NIP-46-style proof via local DBus signer (org.nostr.Signer), verifies response.
  - Account: ensures the cache has a UID/GID mapping; provisions if WarmCache ran.
  - Session: calls Homed service (org.nostr.Homed1) OpenSession/CloseSession.

- nostr-homectl (control service)
  - WarmCache: fetches 30078 (profile relays), 30081 (manifest), and 30079 (secrets refs), decrypts secrets via signer to tmpfs (0600), persists settings.
  - Deterministic UID/GID mapping using configured base/range and npub siphash.
  - DBus API (org.nostr.Homed1): OpenSession(user), CloseSession(user), WarmCache(namespace), GetStatus(user).

- nostrfs (FUSE3)
  - Materializes home from the manifest (30081) with CAS stored at /var/cache/nostrfs/$uid.
  - Read-through from CAS; write-back pipeline:
    1) First write: temp file created in cache/tmp.
    2) On flush/fsync/close: upload to Blossom; promote file to CAS path; update manifest in memory.
    3) Persist new manifest via safe-replace to settings.manifest.<ns>; coalesce and publish latest 30081.
  - CAS quota enforced by mtime-based LRU eviction; cap via env.

## Flows

### Login / OpenSession

1. pam_nostr authenticates via local DBus signer.
2. pam_nostr calls Homed1.OpenSession(user).
3. homectl ensures WarmCache has been executed (or runs it quickly if needed), then starts nostrfs@user via systemd and waits for mount.

### WarmCache

1. Determine namespace (HOMED_NAMESPACE or default "personal").
2. Fetch profile relays (30078) and persist settings.relays.<ns> (fallback RELAYS_DEFAULT).
3. Fetch manifest (30081) and persist settings.manifest.<ns>.
4. Fetch secrets (30079), ask signer to decrypt; write to tmpfs 0600 path.
5. Provision deterministic UID/GID and ensure primary group.

### Write-back

- After upload and manifest update, nostrfs persists the manifest (safe replace) and triggers a debounced publish of the latest 30081 across configured relays.

## Security & Hardening

- Systemd units use strict sandboxes; nostrfs only allows CAP_SYS_ADMIN and /dev/fuse device.
- Secrets written only to tmpfs with mode 0600.
- See docs/SECURITY.md for details.

## Dependencies

- libnostr, libjson, libgo from repo.
- jansson, sqlite3, openssl, secp256k1, glib-2.0, gio-2.0, curl, fuse3.

## Error Handling & Logging

- All system calls checked; return errno-style codes.
- Throttled warnings in nostrfs to avoid spam in tight loops.

## Future Work

- Supplemental groups support in NSS.
- Optional seccomp profile.
- Negative-cache policies for relays during WarmCache.
