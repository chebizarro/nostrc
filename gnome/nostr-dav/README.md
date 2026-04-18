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

Requires: `libsoup-3.0`, `libxml-2.0`, `glib-2.0 ≥ 2.56`
Optional: `libsecret-1` (persistent bearer token storage)

## Running

```bash
# Direct
./nostr-dav

# Or via systemd
systemctl --user enable --now nostr-dav

# Then in GNOME Settings:
# Online Accounts → Add → WebDAV
# URL: http://127.0.0.1:7680
# Password: <token printed at startup>
```

## Current status

**v1 scaffold** — the server answers `OPTIONS`, `PROPFIND`, well-known
redirects, and `REPORT` with valid but empty DAV responses. No real
Nostr content yet. Subsequent beads wire:

- `nostrc-a2jx` — CalDAV ↔ NIP-52 (calendar events)
- `nostrc-iqxk` — CardDAV ↔ contacts kind
- `nostrc-418v` — WebDAV Files ↔ Blossom + NIP-94

## Testing

```bash
cmake --build _build --target test-nostr-dav
ctest -R nostr-dav --test-dir _build
```

## Security

See [SECURITY.md](SECURITY.md) for the transport and auth model.
