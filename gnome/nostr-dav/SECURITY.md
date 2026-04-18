# nostr-dav Security Model

## Transport

nostr-dav listens on `127.0.0.1:7680` over plain HTTP. **TLS is not
used for v1.** This is acceptable because:

1. **Localhost only** — the server binds to `127.0.0.1`, not `0.0.0.0`.
   No packets leave the loopback interface.
2. **No untrusted data in transit** — both the DAV client (GNOME
   Online Accounts / gnome-calendar / evolution) and the DAV server
   run on the same machine under the same user account.
3. **Precedent** — this is the same model used by Nextcloud Desktop,
   GNOME Calendar's CalDAV test harness, and evolution-data-server's
   internal IMAP proxy.
4. **TLS would add complexity** — generating and managing a localhost
   certificate (self-signed or via a local CA) introduces key
   management concerns that outweigh the benefit for loopback traffic.

## Authentication

- A random 32-byte bearer token (base64url-encoded) is generated per
  account at first activation.
- The token is stored in the user's secret service (GNOME Keyring /
  KDE Wallet) via libsecret, **never persisted to disk**.
- WebDAV clients send the token as the password in HTTP Basic auth.
- Token validation uses constant-time comparison to prevent timing
  attacks.

## Threat model

| Threat | Mitigation |
|--------|------------|
| Remote network access | Binds to 127.0.0.1 only |
| Local user privilege escalation | systemd hardening (NoNewPrivileges, ProtectSystem, PrivateTmp) |
| Token theft from disk | Token lives only in secret service, never in config files |
| Timing side-channel on auth | Constant-time token comparison |
| Malicious DAV XML payloads | libxml2 with XML_PARSE_NONET (no network entity resolution) |

## Future improvements

- **TLS with auto-generated localhost cert** — when GNOME supports
  `https://localhost` WebDAV accounts natively.
- **Per-request nonce** — for replay protection (currently unnecessary
  on loopback).
