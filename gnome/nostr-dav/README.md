# nostr-dav

Localhost CalDAV / CardDAV / WebDAV bridge that translates between the
DAV wire protocol and Nostr events.

## Why

GNOME Online Accounts supports **WebDAV** as a first-class account
type (GNOME 3.46+). By running a localhost DAV server, we can make
Nostr calendar events (NIP-52) and contacts appear natively in GNOME
Calendar, GNOME Contacts, and Nautilus Files — without modifying any
system component.

## Architecture

```
┌────────────────────┐     HTTP Basic     ┌──────────────┐
│ GNOME Calendar     │◄──────────────────►│              │
│ GNOME Contacts     │   localhost:7680   │  nostr-dav   │
│ Nautilus Files     │                    │              │
└────────────────────┘                    └──────┬───────┘
                                                 │
                                    ┌────────────┼────────────┐
                                    │ D-Bus      │ nostrdb    │
                                    ▼            ▼            ▼
                              org.nostr.    NIP-52/NIP-??   Blossom
                              Signer       events           blobs
```

## Building

```bash
cmake -B _build -DENABLE_NOSTR_DAV=ON
cmake --build _build --target nostr-dav
```

Requires: `glib-2.0 ≥ 2.60`, `libsoup-3.0`, `libxml-2.0`, `json-glib-1.0`,
`sqlite3 ≥ 3.24`
Optional: `libsecret-1` (mirrors a newly minted bearer token into the keyring)

## Running

```bash
systemctl --user enable --now nostr-dav
nostr-dav --show-credentials   # URL, username, and bearer token

# Then in GNOME Settings (46+):
# Online Accounts → Add Account → WebDAV
# Server Address: http://127.0.0.1:7680/
# Username: nostr    Password: <token from --show-credentials>
```

The systemd user unit is the single activation authority: the D-Bus
activation file for `org.nostr.Dav` delegates to it via `SystemdService=`.
The daemon listens only on `127.0.0.1:7680`, fixed at compile time. See
[docs/QUICKSTART.md](docs/QUICKSTART.md) for the full walkthrough.

## Current status

The server handles `OPTIONS`, `PROPFIND`, `REPORT`, `GET`, `PUT`,
`DELETE`, and the well-known redirects for calendars (NIP-52), contacts
(kind 30085), and files (NIP-94). Everything is stored in SQLite at
`$XDG_DATA_HOME/nostr-dav/store.sqlite` and survives restarts. Auth is
fail-closed; see [SECURITY.md](SECURITY.md).

The schema already includes the publish-outbox columns
(`publish_state`, `publish_attempts`, `publish_next_ts`,
`signed_event_json`) and the `publish_log` table. Relay subscribe
(plan items #8a/#8b) and publishing via `org.nostr.Signer` (item #9)
are not wired yet. Until they are, DAV writes stay local.

## Testing

```bash
cmake --build _build
ctest -R nostr-dav --test-dir _build
```

Suites: `test-nostr-dav-dav-propfind` (DAV protocol),
`test-nostr-dav-auth-required` (fail-closed start, 401s, token file
contract, loopback-only bind, config validation), and
`test-nostr-dav-store-sqlite` (persistence across restart, ctag
monotonicity, schema, permissions, corruption quarantine).

## Security

See [SECURITY.md](SECURITY.md) for the transport and auth model.
