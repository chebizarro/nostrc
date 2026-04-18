# GNOME Desktop Integration — NIP Mapping Cheatsheet

How Nostr primitives map to GNOME desktop services.

## NIP → Desktop Service Matrix

| NIP | Kind(s) | Desktop Target | Bridge | Status |
|-----|---------|---------------|--------|--------|
| NIP-52 (Calendar) | 31922, 31923 | GNOME Calendar via EDS | nostr-dav (CalDAV) | ✅ MVP |
| Contacts (draft) | 30085 | GNOME Contacts via EDS | nostr-dav (CardDAV) | ✅ MVP |
| NIP-94 + Blossom | 1063 | Nautilus / GNOME Files | nostr-dav (WebDAV) | ✅ MVP |
| NIP-46 (Remote Sign) | 24133 | gnostr-signer | D-Bus (org.nostr.Signer) | ✅ Shipped |
| NIP-65 (Relay List) | 10002 | Relay picker UI | signer settings | ✅ Shipped |
| NIP-01 (Metadata) | 0 | GNOME Online Accounts display name | GOA shim | 🚧 Planned |
| NIP-02 (Follow List) | 3 | Contact import hints | nostr-dav sync | 🚧 Planned |
| NIP-54 (Wiki) | 30818 | GNOME Text Editor / Yelp | — | 💡 Idea |

## Architecture

```
┌──────────────────────────────────────────────────┐
│                   GNOME Desktop                  │
│                                                  │
│  ┌─────────┐  ┌──────────┐  ┌─────────────────┐ │
│  │ Calendar │  │ Contacts │  │ Files (Nautilus) │ │
│  └────┬─────┘  └────┬─────┘  └───────┬─────────┘ │
│       │              │                │           │
│       │    EDS (Evolution Data Server) │           │
│       │              │                │           │
│  ┌────▼──────────────▼────────────────▼─────────┐ │
│  │              nostr-dav bridge                 │ │
│  │  :8080/calendars/  :8080/contacts/  :8080/f/  │ │
│  │  (CalDAV)          (CardDAV)        (WebDAV)  │ │
│  └──────────────┬────────────────────────────────┘ │
│                 │                                  │
│  ┌──────────────▼──────────────────┐              │
│  │    gnostr-signer (D-Bus)        │              │
│  │    org.nostr.Signer             │              │
│  │    NIP-46 bunker + key store    │              │
│  └──────────────┬──────────────────┘              │
└─────────────────┼────────────────────────────────┘
                  │
          ┌───────▼───────┐
          │  Nostr Relays  │
          │  + Blossom CDN │
          └───────────────┘
```

## Why Not a Custom GOA Provider?

GNOME Online Accounts (GOA) does not support out-of-tree providers.
Third-party GOA providers were removed in GNOME 3.34 and the plugin API
was never stabilised.

Our workaround: the **onboarding wizard** in gnostr-signer walks users
through adding a "WebDAV" or "Nextcloud" account in GNOME Settings,
pointing it at the local nostr-dav bridge. This gives full Calendar +
Contacts integration without any GOA patches.

See: `apps/gnostr-signer/data/ui/sheets/sheet-online-accounts.blp`

## D-Bus Interface: org.nostr.Signer

The signer exposes a session-bus service for key operations:

| Method | Signature | Description |
|--------|-----------|-------------|
| `GetPublicKey` | `() → s` | Returns the active npub |
| `SignEvent` | `(s) → s` | Signs a JSON event, returns signed JSON |
| `Encrypt` | `(ss) → s` | NIP-04 encrypt (pubkey, plaintext) |
| `Decrypt` | `(ss) → s` | NIP-04 decrypt (pubkey, ciphertext) |
| `Ping` | `() → b` | Health check |

Bus name: `org.nostr.Signer`
Object path: `/org/nostr/Signer`

## Component Directory

| Component | Path | Purpose |
|-----------|------|---------|
| nostr-dav bridge | `gnome/nostr-dav/` | CalDAV/CardDAV/WebDAV server |
| gnostr-signer | `apps/gnostr-signer/` | NIP-46 signer + onboarding UI |
| ICS converter | `gnome/nostr-dav/src/nd-ical.c` | iCalendar ↔ NIP-52 |
| vCard converter | `gnome/nostr-dav/src/nd-vcard.c` | vCard 4.0 ↔ kind 30085 |
| Calendar store | `gnome/nostr-dav/src/nd-calendar-store.c` | In-memory event cache |
| Contact store | `gnome/nostr-dav/src/nd-contact-store.c` | In-memory contact cache |
| File entry | `gnome/nostr-dav/src/nd-file-entry.c` | NIP-94 metadata + SHA-256 |
| File store | `gnome/nostr-dav/src/nd-file-store.c` | In-memory file cache |
| DAV server | `gnome/nostr-dav/src/nd-dav-server.c` | SoupServer request routing |
| Systemd unit | `gnome/nostr-dav/systemd/nostr-dav.service` | User service definition |
| Onboarding UI | `apps/gnostr-signer/data/ui/sheets/sheet-online-accounts.blp` | Setup wizard |

## nostr-dav Endpoints

| Path | Protocol | Methods | Description |
|------|----------|---------|-------------|
| `/.well-known/caldav` | HTTP | GET | → 301 to `/calendars/` |
| `/.well-known/carddav` | HTTP | GET | → 301 to `/contacts/` |
| `/` | WebDAV | PROPFIND | Server root, lists collections |
| `/principals/nostr/` | WebDAV | PROPFIND | Principal resource |
| `/calendars/` | CalDAV | PROPFIND, REPORT | Calendar collection |
| `/calendars/nostr/<uid>.ics` | CalDAV | GET, PUT, DELETE | Individual event |
| `/contacts/` | CardDAV | PROPFIND, REPORT | Address book collection |
| `/contacts/nostr/<uid>.vcf` | CardDAV | GET, PUT, DELETE | Individual contact |
| `/files/` | WebDAV | PROPFIND | Files home collection |
| `/files/nostr/<path>` | WebDAV | GET, PUT, DELETE, PROPFIND, REPORT | Individual file |

## Authentication

nostr-dav uses HTTP Basic auth with a locally-generated bearer token.
The token is stored in the GNOME Keyring via libsecret and displayed
to the user during onboarding.

- **Username**: `nostr`
- **Password**: 32-byte random token, base64-encoded
- **Storage**: `org.nostr.dav.token` in the default keyring

## Systemd Integration

```ini
# ~/.config/systemd/user/nostr-dav.service
[Unit]
Description=Nostr DAV Bridge
After=graphical-session.target gnostr-signer-daemon.service
Wants=gnostr-signer-daemon.service

[Service]
ExecStart=/usr/libexec/nostr-dav
Restart=on-failure

[Install]
WantedBy=default.target
```

Enable with: `systemctl --user enable --now nostr-dav.service`

## Related Documents

- [NIP-52: Calendar Events](https://github.com/nostr-protocol/nips/blob/master/52.md)
- [Contacts Draft NIP](../docs/proposals/nip-contacts-draft.md)
- [nostr-dav Quickstart](../gnome/nostr-dav/docs/QUICKSTART.md)
