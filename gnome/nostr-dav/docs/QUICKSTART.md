# nostr-dav Quickstart

## What is nostr-dav?

A localhost CalDAV/CardDAV/WebDAV bridge that maps GNOME desktop protocols
to Nostr events. It lets GNOME Calendar and GNOME Contacts work with your
Nostr identity through the stock **WebDAV** account in GNOME Settings
(GNOME 46+) — no custom UI and no cloud account.

nostr-dav always listens on **`http://127.0.0.1:7680/`**. The address and
port are fixed at compile time; there is no environment variable or flag
to change them.

## Setup

### 1. Start the service

```bash
systemctl --user enable --now nostr-dav.service
```

On first start nostr-dav mints a random 256-bit bearer token and saves it
to `~/.config/nostr-dav/token` (mode 0600). It never prints the token to
the journal.

### 2. Show your credentials

```bash
nostr-dav --show-credentials
```

```
Server:   http://127.0.0.1:7680/
Username: nostr
Password: <your bearer token>
```

`--show-credentials` creates the token if the service has not run yet, so
you can also do this step first. Treat the password like any other
secret; anyone who has it can read and change your calendar and contacts.

### 3. Add the WebDAV account in GNOME Settings (GNOME 46+)

1. Open **Settings → Online Accounts**.
2. Under **Add Account**, choose **WebDAV**.
3. Fill in the dialog:
   - **Server Address**: `http://127.0.0.1:7680/`
   - **Username**: `nostr` (the username is not checked; any value works)
   - **Password**: the `Password:` line from step 2
4. Click **Sign In**. GNOME finds the calendar and address book itself
   through `/.well-known/caldav` and `/.well-known/carddav`.
5. In the new account, leave **Calendar** and **Contacts** switched on.

### 4. Verify

- **GNOME Calendar** shows a calendar called "Nostr Events".
- **GNOME Contacts** shows an address book called "Nostr Contacts".

To check the server directly:

```bash
TOKEN=$(nostr-dav --show-credentials | sed -n 's/^Password: //p')
curl -s -o /dev/null -w '%{http_code}\n' -u "nostr:$TOKEN" \
     -X PROPFIND -H 'Depth: 1' http://127.0.0.1:7680/calendars/nostr/
# 207
```

Without the token you get `401`. `OPTIONS` and the `.well-known`
redirects need no token and reveal nothing about your data.

> **GNostr Signer's "Add to Online Accounts" wizard** currently shows a
> wrong port and password, so it does not work with this version
> (tracked as `nostrc-0e7k`). Use the steps above instead.

## Files

| Path | Contents | Mode |
|------|----------|------|
| `~/.config/nostr-dav/token` | Bearer token (the WebDAV password) | 0600 |
| `~/.config/nostr-dav/nostr-dav.conf` | Optional settings (see below) | — |
| `~/.local/share/nostr-dav/store.sqlite` | Calendar, contacts, files, pending publishes | 0600 |
| `$XDG_RUNTIME_DIR/nostr-dav/instance.lock` | Single-instance lock | 0600 |

If `XDG_CONFIG_HOME` or `XDG_DATA_HOME` is set, those replace
`~/.config` and `~/.local/share`. Everything survives a restart, and the
calendar and address-book ctags keep counting up across restarts, so
clients only re-sync what changed.

With libsecret available, a newly minted token is also copied into your
keyring. The token file stays the source of truth.

## Configuration

Copy the sample (installed as `share/doc/nostr-dav/nostr-dav.conf.sample`)
to `~/.config/nostr-dav/nostr-dav.conf`. It has one setting today:

```ini
[nostr-dav]
# session_relay_only | session_relay_or_direct (default) | direct_only
nostr_dav_upstream_mode=session_relay_or_direct
```

This chooses which relays nostr-dav uses once relay sync is enabled. An
unknown value stops the service from starting. That way a typo can never
quietly fall back to a less private mode.

## Endpoints

| Path | Protocol | Description |
|------|----------|-------------|
| `/.well-known/caldav` | CalDAV | Redirects to `/calendars/` (no auth) |
| `/.well-known/carddav` | CardDAV | Redirects to `/contacts/` (no auth) |
| `/calendars/nostr/` | CalDAV | NIP-52 calendar events |
| `/contacts/nostr/` | CardDAV | Kind-30085 contacts |
| `/files/nostr/` | WebDAV | NIP-94 files |
| `/principals/me/` | DAV | User principal discovery |

## Uninstall

```bash
systemctl --user disable --now nostr-dav.service
# Remove the WebDAV account in Settings → Online Accounts, then:
rm -rf ~/.config/nostr-dav ~/.local/share/nostr-dav
# If libsecret mirrored the token:
secret-tool clear application nostr-dav account_id default
```

## Troubleshooting

### Service won't start

```bash
journalctl --user -u nostr-dav.service -e
```

nostr-dav refuses to start rather than run insecurely. The log line
begins with `refusing to start:` and names the problem:

- **Token file permissions** — the token must be a regular file you own,
  mode `0600`, in a directory that only you can write to. nostr-dav never
  fixes permissions for you. Correct them with
  `chmod 700 ~/.config/nostr-dav && chmod 600 ~/.config/nostr-dav/token`,
  or delete the token and restart to get a new one (then update the
  password in Online Accounts).
- **Malformed token** — delete `~/.config/nostr-dav/token` and restart.
- **Invalid `nostr_dav_upstream_mode`** — fix the value in
  `nostr-dav.conf`.
- **Another instance** — only one nostr-dav runs per user. Stop the other
  one (`systemctl --user stop nostr-dav.service`, or end the process you
  started by hand).

### Port 7680 already in use

The port cannot be changed. Find what is using it with
`ss -ltnp 'sport = :7680'` and stop that program.

### Corrupted store

If `store.sqlite` fails its integrity check at startup, nostr-dav renames
it to `store.sqlite.corrupt-<timestamp>` and starts with an empty store.
Relays hold the long-term copy of your events.

### Calendar/Contacts not showing

Open Settings → Online Accounts → your WebDAV account and check that
Calendar and Contacts are switched on. If you deleted and re-created the
token, enter the new password in the account.

## Access from other machines

nostr-dav serves only loopback on purpose. To reach it from another
device, put a TLS reverse proxy in front on the same host. The proxy
forwards the `Authorization` header unchanged. For example, with nginx:

```nginx
server {
    listen 443 ssl;
    server_name dav.example.lan;
    ssl_certificate     /etc/ssl/dav.example.lan.crt;
    ssl_certificate_key /etc/ssl/dav.example.lan.key;
    location / {
        proxy_pass http://127.0.0.1:7680;
        proxy_set_header Host $host;
    }
}
```

Only do this on a network you trust. Anyone who can reach the proxy and
has the token gets full read-write access.
