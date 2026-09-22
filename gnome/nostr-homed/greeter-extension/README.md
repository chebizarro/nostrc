# nostr-login-qr — GDM greeter QR renderer

A small gnome-shell extension that displays the pending NIP-46 QR-login
pairing on the GDM greeter (and, optionally, on the lock screen).  It is
consumed at the login dialog while `pam_nostr` / `nostr-authd` is waiting
for a signer to complete a `nostrconnect://` pairing.

The extension is **display-only**.  It never writes anywhere, never makes a
network request, and never touches PAM.  All of the auth logic lives in
`nostr-authd`; the extension simply mirrors what the broker chooses to
publish for the seat.

Design context: `docs/designs/nip46-qr-login-greeter.md` §3.5 (Phase-2
richer rendering) and §5.3 (broker `display` object), plus the go/no-go
`docs/reviews/qr-greeter-render-spike-2026-09-22.md` — a QR sent as PAM
text is unscannable in the gnome-shell login label, so it must be rendered
as a real image by an extension running in the greeter.

## UUID

`nostr-login-qr@nostrc`

## Session modes

`metadata.json` declares:

```json
"session-modes": ["gdm", "unlock-dialog"]
```

- `gdm` — the extension loads in the greeter (the primary v1 target).
- `unlock-dialog` — the extension also loads on the lock screen so a
  future re-auth flow can reuse it.

The extension **does not** declare `user`, so it never runs in a normal
desktop session — there is no reason for a signed-in user to see a
pending-login QR panel on their own screen.

Because the extension is not for `user` sessions, it must be installed
**system-wide** under `${datadir}/gnome-shell/extensions/` (typically
`/usr/share/gnome-shell/extensions/`).  A user-scope install under
`~/.local/share/gnome-shell/extensions/` will not be picked up by GDM.

## Producer contract (broker → extension)

The `nostr-authd` broker (owned by the auth agent, not this extension)
publishes the pending pairing under `/run/nostr-auth/greeter/`.  This
directory layout is the contract:

```
/run/nostr-auth/           mode 0711, root:root
└── greeter/               mode 0755, root:root
    ├── current.json       mode 0644, root:root
    └── current.png        mode 0644, root:root
```

- `/run/nostr-auth` is `0711` so the `gdm` user (uid 124 on the reference
  Ubuntu 24.04 build) can `x`-traverse to `greeter/` without being able
  to list the parent, which may hold sockets or per-seat state.
- `/run/nostr-auth/greeter` is `0755` so `gdm-x-session` /
  `gnome-shell --gdm-mode` can `read` + `x` — required for
  `Gio.FileMonitor` on the directory and for `load_contents` on the
  manifest.
- Both files are `0644`, `root:root`.  Nothing under `greeter/` is a
  secret in the strong sense: the `nostrconnect://` URI encoded in the
  QR contains a one-time pairing secret whose only purpose is to prove
  that a signer saw *this* screen (see the design §8.1); if a local
  unprivileged user reads it the worst they can do is race the phone,
  and the outcome is still gated by the account-pubkey binding checks
  (§8.3).  Do not add any other files to this directory.

### `current.json`

```json
{
    "tx_id": "a1b2c3d4e5f60718",
    "png": "current.png",
    "uri": "nostrconnect://<64-hex>?relay=wss%3A%2F%2Fbunker.sharegap.net&secret=<32-hex>&perms=sign_event%3A1&name=GNOME",
    "pairing_code": "6131-6232",
    "expires_at": 1758560000,
    "hint": "Scan with your Nostr signer"
}
```

Field semantics:

| Field | Type | Required | Notes |
|---|---|:---:|---|
| `tx_id` | string | yes | Broker transaction id.  Logged for correlation; not shown to the user. |
| `png` | string | yes | Basename **only** of the QR image inside `/run/nostr-auth/greeter/`.  Slashes, `..`, and empty strings are rejected; the extension falls back to `current.png`. |
| `uri` | string | no | The full `nostrconnect://` URI.  Not displayed by v1 (the QR is the display surface) but published for future tooling / accessibility. |
| `pairing_code` | string | yes | Short human cross-check the signer app also shows.  Rendered under the QR as `Pairing code: <value>`.  Truncated to 64 chars. |
| `expires_at` | number | yes | Unix seconds.  When `now >= expires_at`, the extension hides the panel and schedules a re-check.  Non-numeric / non-positive values are treated as "no expiry". |
| `hint` | string | no | One line of guidance shown under the pairing code.  Default: `"Scan with your Nostr signer"`.  Truncated to 256 chars. |

Any extra fields are tolerated and ignored.  The manifest is capped at
8 KiB by the extension; anything larger is rejected.

### `current.png`

- Any PNG the broker chooses; `Gio.FileMonitor` picks it up alongside the
  manifest.
- Rendered at ~320px in the greeter using nearest-neighbour scaling
  (`Clutter.ScalingFilter.NEAREST`) so QR modules stay crisp when the
  broker's native module grid is small.  The broker is free to pre-scale
  the PNG; the extension will still display it at ~320px.
- Recommended broker output: encode with `qrencode -t PNG -l L -o
  current.png <uri>`, which yields a 41×41-ish module grid the extension
  upscales cleanly.

### Publish / retire protocol

Atomic publish (broker side):

```sh
umask 022
install -d -m 0711 /run/nostr-auth
install -d -m 0755 /run/nostr-auth/greeter
qrencode -t PNG -l L -o /run/nostr-auth/greeter/.current.png.new "$URI"
mv /run/nostr-auth/greeter/.current.png.new /run/nostr-auth/greeter/current.png
printf '%s' "$MANIFEST_JSON" > /run/nostr-auth/greeter/.current.json.new
mv /run/nostr-auth/greeter/.current.json.new /run/nostr-auth/greeter/current.json
```

Retire (broker side):

```sh
rm -f /run/nostr-auth/greeter/current.json /run/nostr-auth/greeter/current.png
```

Removing `current.json` is the primary hide signal; the extension will
also hide if `expires_at` passes with the manifest still on disk (a
crashed broker leaving stale state).

## Extension behaviour

- Watches `/run/nostr-auth/greeter/` with `Gio.FileMonitor` (both a
  directory monitor and a file monitor on `current.json`, to catch both
  the initial `CREATED` and subsequent `CHANGED` events).
- Debounces bursts of monitor events (100 ms) so one publish → one
  refresh.
- Renders the QR image + pairing code + hint in a floating panel
  positioned top-centre of the primary monitor's greeter.
- Hides when:
  - `current.json` is missing;
  - `current.json` fails to parse (any JSON error, oversize file,
    non-object payload);
  - `expires_at` has passed;
  - `current.png` is missing or fails to decode as a pixbuf.
- Never mutates the login dialog itself.  If any of the extension's
  own state is missing or throws, the panel simply stays hidden — the
  underlying GDM/PAM flow is unaffected.

## Enabling for GDM

The gnome-shell greeter reads a **separate** dconf profile (`gdm`) from
the logged-in user's session.  To enable the extension for the greeter:

```sh
sudo tee /etc/dconf/profile/gdm >/dev/null <<'EOF'
user-db:user
system-db:gdm
file-db:/usr/share/gdm/greeter-dconf-defaults
EOF

sudo install -d -m 0755 /etc/dconf/db/gdm.d
sudo tee /etc/dconf/db/gdm.d/10-nostr-login-qr >/dev/null <<'EOF'
[org/gnome/shell]
enabled-extensions=['nostr-login-qr@nostrc']
EOF

sudo dconf update
sudo systemctl restart gdm
```

To disable without uninstalling, either drop the enabled-extensions key
back to `[]` in `/etc/dconf/db/gdm.d/10-nostr-login-qr` (then
`dconf update && systemctl restart gdm`) or delete that keyfile
entirely.

To fully uninstall, `rm -rf
/usr/share/gnome-shell/extensions/nostr-login-qr@nostrc` and revert the
dconf keyfile.

## Install (from this repo)

The extension is installed by the parent `nostr-homed` CMake build when
`-DNOSTR_HOMED_ENABLE_GREETER_EXTENSION=ON`:

```sh
cmake -S . -B build -DNOSTR_HOMED_ENABLE_GREETER_EXTENSION=ON \
    -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
```

The three files (`metadata.json`, `extension.js`, `stylesheet.css`) land
in `${datadir}/gnome-shell/extensions/nostr-login-qr@nostrc/`.

For a manual test install (no CMake), copy the directory in place:

```sh
sudo cp -R gnome/nostr-homed/greeter-extension/nostr-login-qr@nostrc \
    /usr/share/gnome-shell/extensions/
```

## Manual smoke test with a static QR

```sh
# 1. Install the extension + enable it for GDM (steps above).
# 2. Restart gdm and confirm the greeter comes back.
sudo systemctl restart gdm

# 3. Publish a static test manifest + PNG.
URI='nostrconnect://a1b2c3d4e5f60718a1b2c3d4e5f60718a1b2c3d4e5f60718a1b2c3d4e5f60718?relay=wss%3A%2F%2Fbunker.sharegap.net&secret=deadbeefcafef00ddeadbeefcafef00d&name=GNOME'
EXPIRES=$(( $(date +%s) + 300 ))
sudo install -d -m 0711 /run/nostr-auth
sudo install -d -m 0755 /run/nostr-auth/greeter
sudo qrencode -t PNG -l L -o /run/nostr-auth/greeter/current.png "$URI"
sudo tee /run/nostr-auth/greeter/current.json >/dev/null <<EOF
{"tx_id":"static-smoketest","png":"current.png","uri":"$URI","pairing_code":"A1B2-C3D4","expires_at":$EXPIRES,"hint":"Scan with your Nostr signer"}
EOF

# 4. From the hypervisor, wake and screenshot the greeter, then decode.
# 5. Cleanup:
sudo rm -f /run/nostr-auth/greeter/current.{json,png}
```

The panel must disappear as soon as `current.json` is removed, and must
also disappear if `expires_at` is set to a past unix time.
