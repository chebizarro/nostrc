# Nostr GOA Provider

The Nostr provider integrates with GNOME Online Accounts to provision Calendar, Contacts, and Files services for a Nostr identity.

## Add account flow

1. Query local signer (DBus `org.nostr.Signer`) for available identities; offer to create a new key if none are present.
2. Ask the signer to approve desktop pairing (NIP-46 style challenge/response).
3. Store selected `npub` in the GOA secret storage (via `GoaPasswordBased` secret).
4. Run provisioning helper to:
   - Write EDS CalDAV/CardDAV sources pointing to `http://127.0.0.1:7680`.
   - Start user services: `nostr-router`, `nostr-dav`, `nostrfs`, `nostr-notify`.
   - Register `nostr:` URI handler.

## Services

- Calendar: CalDAV via localhost bridge (`/cal/<user>`)
- Contacts: CardDAV via localhost bridge (`/card/<user>`)
- Files: `nostrfs` mounted at `~/Nostr`
- Mail: disabled by default (bridge optional).

## Refresh & removal

- `refresh_account()`: ping signer to ensure access; if not approved, surface to UI.
- `remove_account()`: stop units and remove EDS sources; unregister `nostr:` handler.
