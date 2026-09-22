# Phase-2 acceptance: gnome-shell greeter extension `nostr-login-qr@nostrc` — GO

- Author: Claude Fable 5.1 (agent)
- Date: 2026-09-22
- Beads: `nostrc-z1fb` (Phase-2 richer rendering), `nostrc-zcll.6` (B5 greeter UX)
- Design under test: [`docs/designs/nip46-qr-login-greeter.md`](../designs/nip46-qr-login-greeter.md) §3.5 (Phase-2 richer rendering) and §5.3 (broker `display` object)
- Precursor spike: [`docs/reviews/qr-greeter-render-spike-2026-09-22.md`](qr-greeter-render-spike-2026-09-22.md) — NO-GO on rendering the QR as PAM text; this is the follow-up that ships the QR as a real image via a greeter extension
- Deliverable: [`gnome/nostr-homed/greeter-extension/`](../../gnome/nostr-homed/greeter-extension/)
  (extension source + README) + CMake install rule under
  `NOSTR_HOMED_ENABLE_GREETER_EXTENSION`
- Environment:
  - `gnome-dev` — Ubuntu 24.04.5 LTS, GNOME Shell 46.0, GDM active on tty1,
    primary display 1280×800, `debugger` user (uid 1000)
  - `gdm` uid 124
  - `qrencode` 4.1.1, `zbarimg` 0.23.93 (both preinstalled from the earlier spike)
  - Hypervisor `192.168.40.15` (`majordomo`) drove input via
    `virsh send-key gnome-dev …` and captured the framebuffer via
    `virsh screenshot gnome-dev …` (QXL/SPICE returns PNG despite the `.ppm`
    suffix — same as the spike)
- Screenshots (this workstation): `/tmp/qr-ext/greeter-0{1..6}-*.png`.
  Every URI encoded into a QR in this run is a static throwaway
  (`nostrconnect://…&name=GDM-QR-EXT-SMOKETEST` / `…&name=EXPIRED-TEST`) —
  no real signer, no relay traffic, no secrets.

## Verdict

**GO.** The extension:

1. Loads in the real GDM greeter as a **system extension** in `gdm` session
   mode (declared `"session-modes": ["gdm", "unlock-dialog"]` in
   `metadata.json`).
2. Renders the pending QR pairing as a real image (`Clutter.Actor` with a
   `Clutter.Image` fed from `GdkPixbuf`, upscaled with
   `Clutter.ScalingFilter.NEAREST`) plus the pairing code and hint below.
3. Is scanner-clean: `zbarimg` decodes the exact test URI directly from
   the greeter framebuffer screenshot — no cropping, no upscaling
   ([§2](#2-primary-acceptance-zbarimg-decodes-from-the-greeter-framebuffer)).
4. Hides cleanly when the manifest is removed, when `expires_at` is in the
   past, and when the manifest is malformed — the underlying login dialog
   is fully functional in every case
   ([§3-§5](#3-hide-on-manifest-removal)).
5. Leaves the stock `gdm-password` PAM stack untouched — `pamtester
   gdm-password debugger authenticate` still reaches `pam_unix`
   (`Password: Authentication failure` on the wrong password, not a config
   error).

The extension is safe to leave installed on `gnome-dev` for follow-up
work; the greeter-side dconf keyfile can be dropped to disable it
without uninstalling.

## 1. What was installed

Files copied to the system extensions dir on `gnome-dev`:

```
$ ls -la /usr/share/gnome-shell/extensions/nostr-login-qr@nostrc/
-rw-r--r-- 1 root root 13251 extension.js
-rw-r--r-- 1 root root   397 metadata.json
-rw-r--r-- 1 root root   785 stylesheet.css
```

`metadata.json` (the load contract):

```json
{
    "uuid": "nostr-login-qr@nostrc",
    "name": "Nostr Login QR",
    "shell-version": ["46"],
    "session-modes": ["gdm", "unlock-dialog"]
}
```

`/etc/dconf/profile/gdm` (created — did not previously exist on this VM):

```
user-db:user
system-db:gdm
file-db:/usr/share/gdm/greeter-dconf-defaults
```

`/etc/dconf/db/gdm.d/10-nostr-login-qr`:

```
[org/gnome/shell]
enabled-extensions=['nostr-login-qr@nostrc']
```

`sudo dconf update && sudo systemctl restart gdm` — the greeter comes back
online in ~6 s and gnome-shell logs the extension without a JS traceback.

## 2. Primary acceptance: `zbarimg` decodes from the greeter framebuffer

Producer side (on `gnome-dev`, as `debugger` + sudo):

```
URI='nostrconnect://a1b2c3d4…?relay=wss%3A%2F%2Fbunker.sharegap.net&secret=deadbeefcafef00ddeadbeefcafef00d&name=GDM-QR-EXT-SMOKETEST'
sudo install -d -m 0711 /run/nostr-auth
sudo install -d -m 0755 /run/nostr-auth/greeter
sudo qrencode -t PNG -l L -s 8 -m 4 -o /run/nostr-auth/greeter/current.png "$URI"
sudo tee /run/nostr-auth/greeter/current.json >/dev/null <<JSON
{"tx_id":"static-smoketest","png":"current.png","uri":"$URI","pairing_code":"A1B2-C3D4","expires_at":$EXPIRES,"hint":"Scan with your Nostr signer app"}
JSON
```

`zbarimg` on the source PNG:

```
$ zbarimg --raw -q /run/nostr-auth/greeter/current.png
nostrconnect://a1b2c3d4e5f60718a1b2c3d4e5f60718a1b2c3d4e5f60718a1b2c3d4e5f60718?relay=wss%3A%2F%2Fbunker.sharegap.net&secret=deadbeefcafef00ddeadbeefcafef00d&name=GDM-QR-EXT-SMOKETEST
```

Greeter driven from the hypervisor (`virsh send-key gnome-dev KEY_SPACE`,
etc.), framebuffer captured via `virsh screenshot`, copied back to this
workstation, then decoded via `scp` back onto `gnome-dev` and
`zbarimg`:

```
$ zbarimg --raw -q /tmp/greeter-01-idle.png
nostrconnect://a1b2c3d4e5f60718a1b2c3d4e5f60718a1b2c3d4e5f60718a1b2c3d4e5f60718?relay=wss%3A%2F%2Fbunker.sharegap.net&secret=deadbeefcafef00ddeadbeefcafef00d&name=GDM-QR-EXT-SMOKETEST
```

Byte-for-byte equal to the source URI. The greeter screenshot at
[`/tmp/qr-ext/greeter-01-idle.png`](/tmp/qr-ext/greeter-01-idle.png)
shows the panel top-centre with QR + `Pairing code: A1B2-C3D4` + `Scan
with your Nostr signer app` below.

The second screenshot,
[`/tmp/qr-ext/greeter-02-prompt.png`](/tmp/qr-ext/greeter-02-prompt.png),
was taken after selecting the `GNOME Debug Operator` user so the
`Password:` prompt is on-screen at the same time as the QR panel — proof
that the extension coexists with an active `gdm-password` conversation.

## 3. Hide on manifest removal

```
sudo rm -f /run/nostr-auth/greeter/current.json
```

Screenshot [`/tmp/qr-ext/greeter-03-hidden.png`](/tmp/qr-ext/greeter-03-hidden.png):
panel gone; the user avatar, name, and password field are all fully
visible again. `Gio.FileMonitor` fires DELETED, the 100 ms debounce
coalesces, and `_refresh()` takes the `!query_exists` branch.

## 4. Hide on expiry

Same PNG on disk, manifest overwritten with `expires_at` 60 s in the
past:

```
PAST=$(( $(date +%s) - 60 ))
sudo tee /run/nostr-auth/greeter/current.json >/dev/null <<JSON
{"tx_id":"expired","png":"current.png","pairing_code":"EXPI-RED0","expires_at":$PAST,"hint":"This should not appear"}
JSON
```

Screenshot [`/tmp/qr-ext/greeter-04-expired.png`](/tmp/qr-ext/greeter-04-expired.png):
panel gone. The extension's `_refreshUnsafe()` takes the
`expiresAt <= nowSec` branch before ever touching the PNG.

## 5. Hide on malformed manifest

```
sudo tee /run/nostr-auth/greeter/current.json >/dev/null <<'JSON'
{ this is not json, "png": "current.png"
JSON
```

Screenshot [`/tmp/qr-ext/greeter-05-malformed.png`](/tmp/qr-ext/greeter-05-malformed.png):
panel gone; login dialog unaffected. gnome-shell's journal captures
the extension's own error line (and *only* that — no traceback, no crash):

```
Sep 22 10:50:36 gnome-dev gnome-shell[164702]: nostr-login-qr: manifest read/parse failed: JSON.parse: expected property name or '}' at line 1 column 3 of the JSON data
```

A follow-up write of a syntactically-valid but non-object payload
(`"not an object"`) also hides the panel via the `typeof manifest !==
'object'` guard — verified by observation in the same run; no separate
screenshot needed since the outcome is byte-identical to the parse-error
case.

## 6. Stock login intact

Post-cleanup:

```
$ ls /run/nostr-auth 2>&1 || echo gone
gone

$ echo debugger | pamtester gdm-password debugger authenticate
Password: pamtester: Authentication failure
rc=1
```

`pamtester` reached `pam_unix` and produced a genuine
`Authentication failure` (wrong password, not the account password) — not
a PAM configuration error, not a stack abort. `/etc/pam.d/gdm-password`,
`/etc/pam.d/common-auth` were not touched by this run. The final idle
greeter screenshot is
[`/tmp/qr-ext/greeter-06-final-idle.png`](/tmp/qr-ext/greeter-06-final-idle.png).

## 7. Producer contract (documented in the extension README)

Full contract in
[`gnome/nostr-homed/greeter-extension/README.md`](../../gnome/nostr-homed/greeter-extension/README.md).
Short form for the auth-broker implementer:

- Directory layout:
  - `/run/nostr-auth` — `0711 root:root` (traversable by `gdm`).
  - `/run/nostr-auth/greeter` — `0755 root:root`.
  - `/run/nostr-auth/greeter/current.json` — `0644 root:root`.
  - `/run/nostr-auth/greeter/current.png` — `0644 root:root`.
- Manifest schema:
  ```json
  {
      "tx_id": "…",
      "png": "current.png",
      "uri": "nostrconnect://…",
      "pairing_code": "6131-6232",
      "expires_at": 1758560000,
      "hint": "Scan with your Nostr signer"
  }
  ```
- `png` MUST be a basename inside the same directory; slashes and `..` are
  rejected by the extension.
- Retire = remove `current.json` (and the PNG). The extension also
  auto-hides when `expires_at <= now`.
- Atomic publish (rename into place) is recommended but the extension
  tolerates partial reads via debounce + JSON try/catch.

## 8. Extension design notes (why it never breaks the greeter)

- Every JS entry point (`enable`, `disable`, monitor callbacks, refresh)
  is wrapped in try/catch that falls back to `_hide()`.
- Manifest is capped at 8 KiB — a rogue producer can't exhaust the shell
  reading it.
- `expires_at` timeout is clamped to 24 hours so a broken producer with
  `expires_at = 9e12` can't schedule an infinite timer.
- Widget is attached to `Main.layoutManager.uiGroup` — no monkeypatching
  of `LoginDialog`, no signal reroutes.
- Session modes intentionally do NOT include `user`, so a signed-in user
  never has this extension loaded on their own screen.

## 9. Known gaps / follow-ups

1. **UX: top-centre panel overlaps the greeter user list / username label**
   at 1280×800. The QR is still readable and coexists with the password
   prompt (screenshot 02), but a real deployment should reposition the
   panel to the right of the login dialog on wide screens or above it
   with reduced height on narrow ones. Filed as an aesthetic follow-up,
   not a functional blocker.
2. **`gdm-session-worker` timeout during a real 90 s wait** (design D9)
   is not exercised by this run — this deliverable is display-only. Test
   at Phase-5 acceptance.
3. **Real broker integration.** The producer contract is documented but
   the broker side (`nostr-authd` publishing to `/run/nostr-auth/greeter/`)
   is not implemented in this commit — it belongs to the broker / PAM
   agent that owns `src/pam/src/auth/`.
4. **`current.png` decode format assumptions.** The extension expects an
   RGB or RGBA `GdkPixbuf` and passes the raw pixels straight to
   `Clutter.Image`. Any other pixbuf shape (e.g. RGBA-premultiplied
   pixbuf from a themed source) would show incorrectly. `qrencode -t PNG`
   is opaque RGB and works; a broker using another encoder should verify.

## 10. Disable / uninstall

Leave the extension installed for follow-up work, but note the disable
path:

```sh
# Disable for the greeter (do not restart gdm during a real session).
sudo rm /etc/dconf/db/gdm.d/10-nostr-login-qr
sudo dconf update
sudo systemctl restart gdm

# Uninstall.
sudo rm -rf /usr/share/gnome-shell/extensions/nostr-login-qr@nostrc
sudo rm -f /etc/dconf/profile/gdm  # only if this file was created by this run
```

The current state on `gnome-dev` is: extension installed and enabled,
`/run/nostr-auth/greeter/` empty, greeter idle at the standard login
prompt.
