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
    "hint": "Scan with your Nostr signer",
    "account": {
        "username": "n_bizarro",
        "display_name": "Biz",
        "identifier": "chebizarro@coinos.io",
        "avatar": "avatar.png"
    }
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
| `account` | object | no | Present when the broker knows which account's login is pending. See below. |

Any extra fields are tolerated and ignored.  The manifest is capped at
8 KiB by the extension; anything larger is rejected.

### `account` (optional identity block)

When the broker has resolved the pending login to a specific enrolled
account — typically because the user entered a NIP-05 identifier at
`"Not listed?"` and `pam_nostr` canonicalised it via the broker before
gnome-shell rewrote the dialog's own user widget — it MAY include an
`account` object.  The extension renders this above the QR, overriding
the dialog's typed name + generic avatar for the duration of the
pairing and restoring both verbatim on retire.

```json
{
  ...
  "account": {
    "username":     "n_bizarro",
    "display_name": "Biz",
    "identifier":   "chebizarro@coinos.io",
    "avatar":       "avatar.png"
  }
}
```

| Field | Type | Required | Notes |
|---|---|:---:|---|
| `username` | string | no | Local canonical account name.  Used as a fallback label when `display_name` is missing.  Truncated to 64 chars. |
| `display_name` | string | no | Prominent label shown above the QR.  Truncated to 128 chars. |
| `identifier` | string | no | Smaller line under `display_name`.  Present only when the login started via NIP-05; the extension shows it verbatim.  Truncated to 128 chars. |
| `avatar` | string | no | Basename **only** of a PNG inside `/run/nostr-auth/greeter/` (same drop directory as `current.png`).  Slashes, `..`, empty strings, and any non-basename value are rejected — the extension falls back to the shell's own `avatar-default-symbolic`.  Never loads from outside the drop directory. |

Field semantics:

- Every field is optional.  A missing / empty / malformed `account`
  object is treated as "no account" (v2 behaviour: the dialog's own
  avatar + username stay visible, the QR + pairing code render below
  them at the original size).  This is enforced defensively — the
  extension NEVER breaks the login/unlock dialog on a bad account
  block.
- If any of `display_name`, `identifier`, or a loadable `avatar` is
  present, the extension:
  1. renders the account identity block above the QR (avatar ~100 px,
     `display_name` as the prominent label, `identifier` as a smaller
     muted line beneath — falling back to `username` for the label
     when `display_name` is empty);
  2. hides the dialog's own user widget (avatar + typed name) so the
     display reads as one identity;
  3. shrinks the QR from 260 px → 220 px so the whole stack still
     clears the greeter's Ubuntu branding logo at 1280×800 (verified
     scannable with `zbarimg` on the framebuffer capture).
- On retire (`current.json` unlinked, `expires_at` elapsed, host
  dialog rebuilt) the hidden user widget is restored to its previous
  `.visible` state and the QR bin is reset to 260 px so a subsequent
  publish without an `account` block behaves byte-identically to v2.

### `avatar.png` (optional, sibling to `current.png`)

```
/run/nostr-auth/greeter/avatar.png    mode 0644, root:root
```

- Sits next to `current.png` under `/run/nostr-auth/greeter/`, same
  ownership + mode.  The `account.avatar` field is a basename-only
  reference into this directory (not an arbitrary path); the extension
  refuses to load images from anywhere else on disk.
- Any PNG the producer chooses.  For the circular mask to read cleanly
  the producer should supply a square crop with an alpha channel
  (or a pre-cropped circle) — the extension applies `border-radius`
  to the avatar bin but does not re-mask the raw pixels.
- Loaded via `GdkPixbuf.Pixbuf.new_from_file` and scaled with
  `Clutter.ScalingFilter.LINEAR` (photo/avatar), unlike the QR's
  nearest-neighbour upscale.  Same size cap as the QR image
  (implausible dimensions >4096 px are refused).
- If the file is missing or fails to decode the extension falls back
  to the shell's own `avatar-default-symbolic` at the same size, so
  the label + identifier still render.

### `account` block (B5-NIP-05, nostrc-bit0)

The broker attaches an `account` block whenever it can name the
account whose login is pending — either because the client typed the
canonical local username directly, or because a NIP-05 identifier
(e.g. `chebizarro@coinos.io`) canonicalised to one at BEGIN_LOGIN.

| Field | Type | Required | Notes |
|---|---|:---:|---|
| `username` | string | yes (when `account` is present) | Canonical local username (matches `passwd`). |
| `display_name` | string | no | Real name from the AccountsService profile cache (`/var/lib/nostr-auth/profile/<user>.json`, fields `display_name` → `name`).  Omitted if the cache is empty; the greeter should fall back to `username`. |
| `identifier` | string | no | The NIP-05 as typed at the greeter (normalized: local case preserved, domain lower-cased). Present **only** when the login was initiated via NIP-05 — omitted for canonical-username logins so the extension shows the plain user label. |
| `avatar` | string | no | Basename of the sibling PNG under `/run/nostr-auth/greeter/`.  Present only when the broker successfully copied `/var/lib/AccountsService/icons/<user>` into the greeter drop as `avatar.png` (mode `0644`, root-owned).  Absent when no cached icon exists (fresh account) or the copy failed; the greeter should fall back to the generic avatar. |

Sibling file when `avatar` is emitted:

```
/run/nostr-auth/greeter/
├── current.json       — as above, with "account" and "avatar":"avatar.png"
├── current.png        — the QR PNG
└── avatar.png         — copy of the AccountsService icon (root-owned, 0644)
```

The broker removes `avatar.png` together with `current.{json,png}` on
transaction retire (`nh_broker_greeter_artifact_remove`) so a subsequent
attempt for a different account never leaves the previous face on
screen.

The producer never mutates AccountsService itself — the icon is
already up to date by the time the account tile shows in GDM.  The
avatar sibling is a copy because the greeter's gnome-shell process
(running as `gdm`) cannot read `/var/lib/AccountsService/icons/`
directly on the shipped Ubuntu setup.

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
- **Preferred mode** ("inline centered"): while the artifact is live, the
  extension locates the current `LoginDialog` / `UnlockDialog` `AuthPrompt`
  (by St style class `login-dialog-prompt-layout` — shared between both
  dialogs since gnome-shell 3.36), hides the `login-dialog-prompt-entry`
  (password entry + eye toggle), and inserts the QR card in the entry's
  slot so the QR appears centred directly under the avatar / username.
  When the manifest carries an `account` object the extension ALSO
  hides the dialog's own `user-widget` (avatar + typed username, which
  gnome-shell captured before `pam_nostr` canonicalised the name) and
  renders its own avatar + `display_name` + `identifier` block above
  the QR — see the account contract above.  Message labels the shell
  renders that would duplicate the card's pairing code or hint
  (`login-dialog-message` / `login-dialog-message-hint` containing
  "Pairing code" or "Scan the QR") are also hidden so the card is the
  sole source of on-screen instructions; message-warning labels
  ("Sorry, that didn't work") are never touched.
- **Fallback mode** ("floating"): if the `AuthPrompt` cannot be located
  (unusual shell version, dialog still being built), the extension
  renders the same QR card as a floating widget off to the right of the
  primary monitor so the QR is still scannable — this matches the
  earlier v1 layout documented in
  `docs/reviews/phone-test-rig-2026-09-22.md`.  The extension retries
  the inline attach a few times over ~2.4 s in case the dialog is
  still being built when the artifact publishes.
- Hides when:
  - `current.json` is missing;
  - `current.json` fails to parse (any JSON error, oversize file,
    non-object payload);
  - `expires_at` has passed;
  - `current.png` is missing or fails to decode as a pixbuf.
- On hide the hidden `login-dialog-prompt-entry`, the dialog's own
  `user-widget` (if it was hidden for the `account` block), and any
  suppressed message labels are restored to their prior `.visible`
  state so the underlying dialog is byte-identical to what the shell
  rendered before we intervened.  Never mutates the login/unlock
  dialog outside the window during which the artifact is live.  If
  any of the extension's own state is missing or throws, the panel
  simply stays hidden — the underlying GDM/PAM flow is unaffected.
- If the host `AuthPrompt` is torn down while the card is attached
  (e.g. `LoginDialog` rebuild after Escape) the extension detects the
  `destroy` signal, drops references, and rebuilds its own container so
  a subsequent publish can reattach cleanly.

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

## Enabling for the lock screen (`unlock-dialog` mode)

The lock screen runs inside the **user's** gnome-shell process (not
GDM's), transitioned into `session-modes: ["unlock-dialog"]`.  The
extension is only loaded there if the user's `org.gnome.shell
enabled-extensions` list carries `nostr-login-qr@nostrc`.  The
greeter's dconf keyfile above (`/etc/dconf/db/gdm.d/…`) is scoped to
the `gdm` profile and does **not** cover user sessions — for the
lock-screen path you need a *user-scope* enablement.

System-wide default for every user session (recommended for deployments
where the whole herd is on nostr-homed):

```sh
sudo tee /etc/dconf/profile/user >/dev/null <<'EOF'
user-db:user
system-db:local
EOF

sudo install -d -m 0755 /etc/dconf/db/local.d
sudo tee /etc/dconf/db/local.d/10-nostr-login-qr >/dev/null <<'EOF'
[org/gnome/shell]
enabled-extensions=['nostr-login-qr@nostrc']
EOF

sudo dconf update
```

Existing user sessions pick this up on next shell restart (log out /
log back in, or `sudo systemctl restart gdm` if no live sessions).
New logins pick it up automatically.  The extension only draws in
`unlock-dialog` mode inside user sessions — normal desktop use is not
affected because the extension does not declare the `user` session
mode.

Per-user opt-in (useful for testing without touching site-wide dconf):

```sh
gsettings set org.gnome.shell enabled-extensions "['nostr-login-qr@nostrc']"
```

Run as the target user in an already-open session.  Verify with:

```sh
gsettings get org.gnome.shell enabled-extensions
# -> ['nostr-login-qr@nostrc']
```

The artifact directory `/run/nostr-auth/greeter/` must remain readable
to the user's shell process — the shipped broker publishes with dir
mode `0755` and file mode `0644`, which is what `unlock-dialog` needs.

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

### Smoke test with a resolved `account`

Same as above, but ALSO drop an `avatar.png` next to `current.png` and
include an `account` object in the manifest.  Uses an AccountsService
avatar as a stand-in for whatever the broker will produce; any PNG at
`0644` works.

```sh
URI='nostrconnect://a1b2c3d4e5f60718a1b2c3d4e5f60718a1b2c3d4e5f60718a1b2c3d4e5f60718?relay=wss%3A%2F%2Fnos.lol&secret=deadbeefcafef00ddeadbeefcafef00d&name=GNOME'
EXPIRES=$(( $(date +%s) + 300 ))
sudo install -d -m 0711 /run/nostr-auth
sudo install -d -m 0755 /run/nostr-auth/greeter
sudo qrencode -t PNG -l L -o /run/nostr-auth/greeter/current.png "$URI"
sudo cp /var/lib/AccountsService/icons/<some-user> /run/nostr-auth/greeter/avatar.png
sudo chmod 0644 /run/nostr-auth/greeter/current.png /run/nostr-auth/greeter/avatar.png
sudo tee /run/nostr-auth/greeter/current.json >/dev/null <<EOF
{
  "tx_id": "acct-smoketest",
  "png": "current.png",
  "uri": "$URI",
  "pairing_code": "ACCT-C0DE",
  "expires_at": $EXPIRES,
  "hint": "Scan with your Nostr signer",
  "account": {
    "username":     "n_bizarro",
    "display_name": "Biz",
    "identifier":   "chebizarro@coinos.io",
    "avatar":       "avatar.png"
  }
}
EOF
```

The greeter dialog should render `n_bizarro`'s avatar + "Biz" +
"chebizarro@coinos.io" above the (shrunken) QR, replacing the dialog's
own avatar / username for the duration of the pairing.  Removing
`current.json` restores the dialog's own user widget verbatim.

## Producer implementation notes (nostr-authd side)

This section is a producer-side supplement to the consumer contract above.
Nothing in it changes the wire shape; it documents how the shipped
`nostr-authd` broker + `pam_nostr` module actually publish the drop today.

### Atomic write

Both files are written to `<basename>.tmp` in the same directory and then
`rename(2)`'d into place, so a `Gio.FileMonitor` observer never sees a
half-written file. Both files are produced together by the broker
(`gnome/nostr-homed/src/auth/auth_conf.c`, `nh_broker_greeter_artifact_write`)
so one publish = one JSON + one PNG. Design decision D3 originally kept
the QR encoder out of the root daemon; we deliberately relax that in the
shipped implementation because the extension expects both files on every
publish and having two disjoint producers (broker for JSON, PAM for PNG)
would leave the artifact incomplete for non-PAM callers, including our
own headless integration tests. Vendored Nayuki qrcodegen (MIT) is
~1 kLoC of pure arithmetic on a URI we generated ourselves, so the
blast radius on the root daemon is small.

### PNG format

The PAM-produced PNG is a monochrome greyscale (PNG colour type 0, 8-bit)
image with each QR module rendered as a `scale × scale` pixel block. The
shipped encoder uses `scale = 8` at the design's version-9 cap, yielding a
`(size + 4) * 8 × (size + 4) * 8` PNG (typically 456×456 for a
~200-byte URI). Foreground is `0x00`, background `0xff`, no palette or
filter tricks — a stock PNG decoder handles it.

### `expires_at` is absolute unix-seconds

The broker computes `expires_at = time(NULL) + <wait_budget_ms>/1000` at
publish time and writes it into `current.json` as a plain integer number
of seconds. The extension's `now >= expires_at` hide check therefore
compares seconds against seconds. A missing or non-positive value is
treated as "no expiry" per the consumer contract. Example:

```json
{ "tx_id": "…", "png": "current.png",
  "uri": "nostrconnect://…",
  "pairing_code": "A1B2-C3D4",
  "expires_at": 1758560000,
  "hint": "Scan with your Nostr signer" }
```

The provider's internal per-attempt scan budget (78 000 ms at the default)
is also exposed to `pam_nostr` as `expires_in_ms` on the
`SELECT_PROVIDER` reply so the PAM module can drive its own local
countdown; the artifact JSON on disk (which the extension reads) uses
only the absolute `expires_at`. The wire `display` object carries both
fields; a client that only understands one is guaranteed to get the
right units.

### Removal on retire

`current.json` and `current.png` are both `unlink(2)`'d by the broker as
soon as the login transaction retires — success, denial, expiry, cancel,
or the client socket closing — via `nh_broker_greeter_artifact_remove()`
called from `conn_reset_proof`. Removing `current.json` is the primary
hide signal per the consumer contract; the extension also self-hides on
`expires_at` elapsed for the crashed-broker case.

### Test seam

Headless tests can point the drop at a per-test tmpdir via
`nh_broker_greeter_artifact_set_dir(path)` (declared in
`gnome/nostr-homed/src/auth/auth_broker.h`) so
`test_broker_login_nip46_qr` can assert the presence + shape of both
files without touching `/run/nostr-auth/greeter`. Production leaves the
directory at the default and never calls the seam.
