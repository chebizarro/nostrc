# nostr-dav Quickstart

## What is nostr-dav?

A localhost CalDAV/CardDAV/WebDAV bridge that maps GNOME desktop protocols
to Nostr events. It lets GNOME Calendar, Contacts, and Files work with
your Nostr identity — no cloud account needed.

## Quick Setup (GUI)

1. **Launch GNostr Signer** and ensure you have at least one identity.
2. Open **Settings → Online Accounts → Add Account** (or click
   "Add to Online Accounts" in the signer menu).
3. Follow the on-screen wizard:
   - The wizard starts `nostr-dav.service` if not already running.
   - A bearer token is provisioned and copied to your clipboard.
   - GNOME Settings opens the WebDAV account dialog.
   - Paste the token as the password.

4. Your Nostr calendar events and contacts now appear in GNOME Calendar
   and GNOME Contacts.

## Manual Setup

### 1. Start the service

```bash
systemctl --user enable --now nostr-dav.service
```

### 2. Get the bearer token

The service generates a random token on first run and stores it in your
system keyring (GNOME Keyring / KDE Wallet). Retrieve it with:

```bash
secret-tool lookup service nostr-dav account default
```

Or generate a new one:

```bash
nostr-dav --print-token
```

### 3. Add a WebDAV account in GNOME Settings

1. Open **Settings → Online Accounts**.
2. Click **Add Account → Nextcloud** (or generic WebDAV if available).
3. Enter:
   - **Server**: `http://127.0.0.1:7654`
   - **Username**: `nostr` (any value works)
   - **Password**: *(paste the bearer token from step 2)*

### 4. Verify

- **GNOME Calendar** should show the "Nostr Events" calendar.
- **GNOME Contacts** should show the "Nostr Contacts" address book.

## Endpoints

| Path | Protocol | Description |
|------|----------|-------------|
| `/.well-known/caldav` | CalDAV | Redirects to `/calendars/` |
| `/.well-known/carddav` | CardDAV | Redirects to `/contacts/` |
| `/calendars/nostr/` | CalDAV | NIP-52 calendar events |
| `/contacts/nostr/` | CardDAV | Kind-30085 contacts |
| `/principals/me/` | DAV | User principal discovery |

## Uninstall

```bash
# Stop and disable the service
systemctl --user disable --now nostr-dav.service

# Remove the GOA account from Settings → Online Accounts

# Clear the bearer token from the keyring
secret-tool clear service nostr-dav account default
```

## Troubleshooting

### Service won't start
```bash
journalctl --user -u nostr-dav.service -f
```

### Port already in use
The default port is 7654. If it conflicts, set `NOSTR_DAV_PORT` in the
environment or pass `--port=NNNN` to the binary.

### Calendar/Contacts not showing
Check that the GOA account is enabled for CalDAV and CardDAV in
Settings → Online Accounts → (your WebDAV account).
