# nostr-dav Security Model

## Transport

nostr-dav listens on `127.0.0.1:7680` over plain HTTP. **TLS is not
used for v1.** This is acceptable because:

1. **Localhost only** — the server binds to `127.0.0.1:7680`, fixed at
   compile time. There is no environment or command-line override, and
   `nd_dav_server_start()` refuses any non-loopback address. No packets
   leave the loopback interface. LAN access requires a TLS reverse proxy
   (see docs/QUICKSTART.md).
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

- A random 256-bit bearer token (base64url-encoded, from
  `getentropy()`; there is no non-CSPRNG fallback) is minted on first
  run.
- It is persisted to `$XDG_CONFIG_HOME/nostr-dav/token`, mode 0600, in
  a 0700 directory. The file is written to a temporary file, fsync'd,
  and published with `link(2)`, which gives `O_EXCL` no-clobber
  semantics without ever exposing a partially written token.
- On load, nostr-dav refuses to start if the token file is not a
  regular file owned by the user with no group/other permission bits,
  is a symlink, is malformed, or sits in a group/world-writable
  directory. It never chmods and continues.
- With libsecret, a freshly minted token is also mirrored into the
  keyring (best effort). Validation uses only the token file.
- **Fail closed**: the server will not bind until an account is
  configured and its token is loaded, and every request other than
  `OPTIONS` and the `.well-known` redirects requires HTTP Basic auth
  whose password equals the token. These unauthenticated responses
  carry no user data.
- Token validation uses constant-time comparison to prevent timing
  attacks.
- The daemon never logs the token. `nostr-dav --show-credentials`
  prints it to stdout on request.

## Threat model

| Threat | Mitigation |
|--------|------------|
| Remote network access | Binds to 127.0.0.1 only |
| Local user privilege escalation | systemd hardening (NoNewPrivileges, ProtectSystem=strict, ProtectHome=read-only with only the token and store dirs writable, PrivateTmp) |
| Token theft from disk by other users | Token file 0600 in a 0700 dir; startup refused on looser permissions |
| Requests before auth is configured | Bind happens only after the token is loaded and the account is set |
| Store readable by other users | `store.sqlite` (and its WAL/SHM) created 0600 in a 0700 dir |
| Timing side-channel on auth | Constant-time token comparison |
| Malicious DAV XML payloads | libxml2 with XML_PARSE_NONET (no network entity resolution) |

## Future improvements

- **TLS with auto-generated localhost cert** — when GNOME supports
  `https://localhost` WebDAV accounts natively.
- **Per-request nonce** — for replay protection (currently unnecessary
  on loopback).
