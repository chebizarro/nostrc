# Phone-test rig on gnome-dev — real signer, real greeter

- Author: Claude Opus 4.7 (agent)
- Date: 2026-09-22
- Related: `docs/reviews/nip46-greeter-acceptance-final-2026-09-22.md`
  (the two-path acceptance, whose install/seed/enable steps this rig
  reproduces verbatim), `docs/reviews/gdm-qr-extension-2026-09-22.md`,
  and this branch's greeter-extension polish (`gnome/nostr-homed/greeter-extension/
  nostr-login-qr@nostrc/`).
- Beads: `nostrc-zcll.6` (B5 desktop UX polish), `nostrc-z1fb` (B5-QR)
- Guest: `ssh gnome-dev` — Ubuntu 24.04 LTS, x86_64, GNOME Shell 46, GDM
  active on tty1, primary display 1280×800.  Hypervisor:
  `majordomo@192.168.40.15` (passwordless sudo,
  `sudo virsh screenshot gnome-dev`, `sudo virsh send-key gnome-dev …`).
- Verified relay: `wss://nos.lol` (public, reachable from any consumer
  phone signer — sharegap relays are NIP-42-gated and unusable from a
  phone that never authed).
- Extension: `nostr-login-qr@nostrc`, polished on this branch (right-side
  card that never overlaps the greeter user block — see
  `docs/reviews/gdm-qr-extension-2026-09-22.md` for the prior top-centre
  version's overlap failure at 1280×800).
- Screenshots proving the polish (post PAM fix, current):
  `/tmp/rig-final/greeter-05-post-pam-fix-qr.png` — Nostr User avatar
  and password entry centred, a SHORT `Pairing code: 22F4-088C /
  Scan the QR code shown on screen with your Nostr signer app` line
  under the entry (no block QR, no URI dump — see §7.1), and the
  right-side QR card with the matching pairing code; Ubuntu logo
  fully visible at the bottom.  `/tmp/rig-final/greeter-06-post-pam-fix-idle.png`
  — after ESC, the extension hides and the greeter is back to the
  clean tile list.  Older reference shot at
  `/tmp/rig-final/greeter-01-idle.png` still shows the layout polish
  but pre-dates the PAM fix so its centre still carries the block QR
  garbage — kept for the layout-fix diff, not as a maintainer target.
- Seeded pubkey (maintainer's npub, bech32 checksum verified):
  `cdee943cbb19c51ab847a66d5d774373aa9f63d287246bb59b0827fa5e637400`
  (`npub1ehhfg09mr8z34wz85ek46a6rww4f7c7jsujxhdvmpqnl5hnrwsqq2szjqv`).
- Seeded account: `n_bizarro`, uid 200000, provider `nip46qr`, relay
  `wss://nos.lol`, name `GNOME-QR-BIZARRO`.

## 1. Rig state (leave running)

Broker, PAM profile, NSS, and greeter extension are all enabled and
active on the guest.  Nothing is scheduled to teardown.  Point a phone
signer at the greeter and it will work.

Verify from the guest:

    $ ssh gnome-dev
    $ systemctl is-active gdm
    active
    $ systemctl is-active nostr-authd
    active
    $ grep -E '^(passwd|group):' /etc/nsswitch.conf
    passwd:         files systemd sss nostr
    group:          files systemd sss nostr
    $ grep -c pam_nostr /etc/pam.d/common-auth
    1
    $ getent passwd n_bizarro
    n_bizarro:x:200000:200000:Nostr User:/home/n_bizarro:/bin/bash
    $ cat /etc/nostr-auth/auth.conf
    nip46_qr_relays = wss://nos.lol
    $ ls /etc/dconf/db/gdm.d/
    10-nostr-login-qr
    $ ls /usr/share/gnome-shell/extensions/nostr-login-qr@nostrc/
    extension.js  metadata.json  stylesheet.css

Verify from the hypervisor:

    $ ssh majordomo@192.168.40.15 'sudo virsh list --all'
     Id   Name        State
    ---------------------------
      4   gnome-dev   running

## 2. Console access for the maintainer

Two ways to reach the guest's display for the phone-scan step:

- `virt-viewer` / `remote-viewer` against the libvirt domain:
    - `remote-viewer -f spice+unix:///var/run/libvirt/qemu/gnome-dev.sock`
      over the majordomo SSH tunnel per the VM ops doc; or
    - Point Remmina at the hypervisor's SPICE port (per VM doc).
- Any SPICE console (Boxes, virt-manager) that reaches libvirt on
  majordomo works — the greeter renders on tty1 at 1280×800 and the
  phone camera reads the QR at that pixel density comfortably (`zbarimg`
  decodes the same screenshot cleanly — see §5).

You want a real screen so the phone camera can point at it.  The QR is
rendered nearest-neighbour at ~300 px so the modules stay crisp; anything
sharper than a phone webcam sees it fine.

## 3. Maintainer steps at the greeter (with a real phone NIP-46 signer)

Install a NIP-46 signer app on the phone first — Amber is the reference,
but any app that speaks `nostrconnect://` per NIP-46 works.  The signer
must hold the private key for
`npub1ehhfg09mr8z34wz85ek46a6rww4f7c7jsujxhdvmpqnl5hnrwsqq2szjqv`.

The greeter idles on a user list containing `Nostr User` (which is
`n_bizarro`'s NSS display name) and `GNOME Debug Operator`
(`debugger`, the stock account).

Two entry paths, either works:

**A. Pick from the list (fastest)**

1. Click the `Nostr User` tile.
2. GDM begins the PAM conversation.  The right-side QR card appears
   with the pairing code (`E799-5FC2` style, first 4-4 hex of the
   ephemeral client pubkey).
3. Below the avatar the greeter also prompts `Sign in with
   (local/remote/qr):` — this is because pam_nostr configures the
   default provider order.  For a fresh session with only the QR
   provider seeded, GDM should auto-pick `qr` and the panel appears
   with no additional keystroke; if it prompts, type `qr` and press
   Enter.

**B. Type the username explicitly** (matches the task's canonical
sequence)

1. Click `Not listed?`
2. Type `n_bizarro` and press Enter.
3. At the `Sign in with (local/remote/qr):` prompt, type `qr`, Enter.

Either way, the greeter now shows:

- The right-side QR card with `Scan this with your Nostr signer app`,
  the QR image (~300 px, white plate, rounded card), and the
  `Pairing code: XXXX-YYYY` legend.
- The centred greeter user block (avatar, `Nostr User`, password entry)
  remains fully visible; the panel does **not** overlap them (this is
  the fix over the previous top-centre anchor documented in the QR
  extension review).

Scan the QR with the phone signer app.  It reads a
`nostrconnect://<client-pubkey>?relay=wss%3A%2F%2Fnos.lol&secret=<hex>&perms=sign_event%3A1&name=GNOME-QR-BIZARRO`
URI, opens a WebSocket to `wss://nos.lol`, and prompts the operator to
approve the connect.  Approve it in the phone app; the signer then
receives a `sign_event` request for the auth challenge (kind-1 with the
tx nonce) and prompts again.  Approve the signature.

Back on the greeter (~1-3 seconds later):

- The QR card retires (extension hides itself).
- GDM opens the session for `n_bizarro`, uid 200000, home
  `/home/n_bizarro` (created on first login by pam_mkhomedir /
  pam_systemd — the shipped PAM profile does this automatically).
- You're at the Ubuntu 24.04 GNOME desktop.

## 4. Timing and retry behaviour

- Broker per-attempt scan window: **78 000 ms** (`nip46_qr_wait_ms`
  default; PAM prints `Waiting for your signer…` up to this budget).
  If nothing arrives on the relay in that window, `pam_nostr` returns
  `PAM_AUTH_ERR` and GDM either loops back to the user list or
  represents the QR (GDM's own retry policy — typically 3 tries
  before it drops back to the tile view).
- Extension hides on any of: `current.json` unlink, `expires_at`
  passing, `current.png` decode failure, malformed manifest.
- If you dismiss with `Esc`, GDM goes back to the user list and the
  broker's `conn_reset_proof` retires the artifact atomically — you'll
  see the panel disappear within ~100 ms (extension debounce is 100 ms
  on the file monitor).
- New attempt = new client keypair = new pairing code and new
  `secret=`.  There's no retryable "same code" — every fresh flow is
  a fresh transaction.

## 5. Watching the run

From the hypervisor for screenshots + key input:

    ssh majordomo@192.168.40.15
    sudo virsh screenshot gnome-dev /tmp/live.png
    sudo virsh send-key gnome-dev KEY_ESC
    sudo virsh send-key gnome-dev KEY_SPACE   # wake screen
    # etc.

From the guest for logs (each of these is safe to leave `-f`'d in a
side tmux):

    sudo journalctl -u nostr-authd -f | grep -E 'nip46|await|SUCCESS|matched|invalid|DENIED'
    sudo tail -f /var/log/auth.log | grep -E 'pam_nostr|nostr:'

The two lines to watch for on the guest during a happy-path scan:

    [nip46] await_connect: matched, signer=cdee943c…7400
    [nip46] get_public_key: SUCCESS - result: cdee943c…7400
    [nip46] sign_event: SUCCESS - result: {"id":"…"}
    pam_nostr(gdm-password:auth): nostr: authenticate n_bizarro (nip46qr) -> ok
    pam_unix(gdm-password:session): session opened for user n_bizarro(uid=200000)

The signer pubkey MUST come back as
`cdee943cbb19c51ab847a66d5d774373aa9f63d287246bb59b0827fa5e637400`.
If a different key answers (wrong phone, wrong slot), the provider
returns `NH_AUTH_NIP46_SIGN_DENIED` and PAM logs
`authenticate n_bizarro (nip46qr) -> invalid_proof` — sign_event is
never requested from the phone in that case.

## 6. Zbarimg proof of the polished layout

From this workstation:

    scp majordomo@192.168.40.15:/tmp/rig-idle.png .
    /opt/homebrew/opt/zbar/bin/zbarimg rig-idle.png
    # -> QR-Code:nostrconnect://3653b4d1dbd696775ff3b2091bcebc326d42c9f7abf6f2548e56fb0ff9feb575?relay=wss%3A%2F%2Fnos.lol&secret=…&perms=sign_event%3A1&name=GNOME-QR-BIZARRO
    #    scanned 1 barcode symbols from 1 images in 0.04 seconds

The QR decodes cleanly from the greeter screenshot — same pixels a phone
camera will see, but through a virsh screenshot capture at 1280×800.
Pairing code `3653-B4D1` visible in the screenshot matches the first 8
hex of the client pubkey `3653b4d1…`, as required by the design's 4-4
dashed pairing scheme.

## 7.1 PAM graphical/text-console detection fix

`gnome/nostr-homed/src/pam/pam_nostr_broker.c` — the `is_text_console()`
heuristic was reshuffled to default to graphical and require positive
proof of a text console before emitting the block QR.  The earlier
version keyed on `PAM_TTY` first, which under Wayland-GDM comes through
as `/dev/tty1` and naïvely matched the `"/dev/tty"` prefix — the
graphical greeter then rendered the half-block QR (proportional
Cantarell wraps every module; unscannable, and it overflowed onto the
Ubuntu logo — see the older `/tmp/qr-ext/greeter-02-prompt.png`).

New rules, in order:

1. `service_is_graphical(service)` short-circuits to graphical if
   `PAM_SERVICE` starts with `gdm` (`gdm-password`,
   `gdm-launch-environment`, …), contains `gnome`/`lightdm`/`sddm`,
   or is literal `xdm`.
2. `PAM_XDISPLAY` set → graphical (X greeters only; Wayland leaves it
   unset).
3. An explicit graphical `PAM_TTY` (`:0` / `:1` / `:wayland-*`) →
   graphical.
4. `service_is_known_text(service)` is authoritative for text-console:
   `login`, `nostr-login`, `sshd`/`ssh`, `su`/`su-l`,
   `sudo`/`sudo-i`, `systemd-user`, `check_user` (pamtester default).
5. Anything else → graphical (safe default).

Rule 4 is authoritative even without `PAM_TTY` because `pamtester`
without `-h` leaves `PAM_TTY` unset — requiring a console-looking TTY
plus a known-text service was too strict and broke the
`nostr-login` smoke path.

On the graphical branch of `render_qr_info()` the `pam_info` payload
is a fixed 2-line message that always reads `Pairing code: X /
Scan the QR code shown on screen with your Nostr signer app` (no URI
dump, no block QR).  The broker's `hint` field is ignored on the
graphical path so on-screen text is stable across account
configurations.  The text-console branch still emits the half-block
QR + URI + pairing code (design D11), verified with
`printf 'qr\n' | pamtester -v nostr-login n_bizarro authenticate`
on the guest — see the standin_ok-style log embedded below.

Text-console proof (guest pts, service `nostr-login`, timed out
waiting for a signer after 12 s, block QR present):

    pamtester: invoking pam_start(nostr-login, n_bizarro, ...)
    pamtester: performing operation - authenticate
    pamtester: Authentication failure
    Scan this with your Nostr signer app:

      █▀▀▀▀▀█  ▄▀▀█  ▄██▄▄███ ▄ █  █▀▀█ ████▄▄█ █▀▀▀▀▀█
      █ ███ █ ▄ ▀▄██▄▀▄█▄ ▄ ▄█▄█▄ █▄█▀██ ▄▄▄ █▀ █ ███ █
      … (half-block QR of the nostrconnect:// URI, elided) …
      █ ▀▀▀ █ ▄ ▀▄  ▄  ██▄▀▀▄█▀█▀▀▄ ▄▀▄▄▀ ▀ ▀█  █▀▄▀  ▄
      ▀▀▀▀▀▀▀ ▀  ▀▀ ▀▀  ▀  ▀▀▀▀▀  ▀▀▀ ▀ ▀ ▀ ▀      ▀  ▀

    Or open this link on your phone:
    nostrconnect://d8413ad5…?relay=wss%3A%2F%2Fnos.lol&secret=<REDACTED>&perms=sign_event%3A1&name=GNOME-QR-BIZARRO

    Pairing code: D841-3AD5

## 7. What was polished on this branch

`gnome/nostr-homed/greeter-extension/nostr-login-qr@nostrc/extension.js`:

- `_positionContainer()`: the QR card now anchors to the RIGHT of the
  primary monitor, vertically centred, with a 40 px edge margin, instead
  of top-centre.  The previous top-centre anchor overlapped the greeter's
  centred user block (`GNOME Debug Operator` name label appeared behind
  the panel in `docs/reviews/gdm-qr-extension-2026-09-22.md`'s
  `greeter-02-prompt.png`); the right-side anchor puts the card in a
  distinct block that never overlaps avatar, username label, password
  entry, or `Not listed?`.
- `IMAGE_DISPLAY_PX` dropped from 320 to 300 px so the pairing code +
  hint lines fit under the QR without the card growing taller than
  the greeter's vertical safe area.  300 px is still a comfortable
  phone-scan target at 1280×800; `zbarimg` decodes the same screenshot
  in 40 ms (§5).
- Narrow-screen fallback: on monitors narrower than about
  `panelWidth * 2 + 340` px, the card falls back to top-centre — the
  practical minimum for the right-side layout to not crowd the login
  stack.  Standard GDM configurations are never that narrow, so the
  fallback is defensive only.

`gnome/nostr-homed/greeter-extension/nostr-login-qr@nostrc/stylesheet.css`:

- Rounded card, slightly darker background (rgba 24,24,26,.94), an
  outer `box-shadow` so the card reads as intentional chrome against
  the greeter's dark backdrop.
- Pairing-code font slightly smaller (15 pt vs 16 pt) and hint padding
  tightened so the whole card stays under ~430 px tall — comfortably
  within the primary monitor at 1280×800.

## 8. Teardown

When done, run the teardown script bundled with this rig to restore
the guest to stock:

    ssh gnome-dev sudo bash /path/to/repo/scripts/phone-test-rig-teardown.sh

The script (`scripts/phone-test-rig-teardown.sh`) does, idempotently:

1. `systemctl disable --now nostr-authd`, `daemon-reload`.
2. `pam-auth-update --package --remove nostr`.
3. Restore `/etc/nsswitch.conf` from
   `/etc/nsswitch.conf.rig-backup` (or, if the backup isn't present,
   edit `passwd:` / `group:` back to `files systemd sss`).
4. Remove the installed binaries + libraries + PAM module + NSS
   module + systemd unit + pam-configs profile + auth.conf + docs +
   pkgconfig files under `/usr/...` and `/etc/nostr-auth`.
5. Kill any `n_bizarro` processes, remove `/home/n_bizarro` and
   `/var/lib/nostr-auth` and `/run/nostr-auth`.
6. `rm /etc/dconf/db/gdm.d/10-nostr-login-qr && dconf update` and
   `rm -rf /usr/share/gnome-shell/extensions/nostr-login-qr@nostrc`.
7. `systemctl restart gdm` and print a verification block.

The script prints the post-restore state as its last block:

    passwd:  passwd:         files systemd sss
    pam_nostr in common-auth: 0
    nostr-authd unit: Unit nostr-authd.service could not be found.
    getent n_bizarro: gone
    gdm: active

If the maintainer wants to disable the QR extension without ripping the
whole rig, the single-command version is:

    sudo rm /etc/dconf/db/gdm.d/10-nostr-login-qr
    sudo dconf update
    sudo systemctl restart gdm

which is a subset of §8.6 above.

## 9. Known quirks the maintainer might see

- On some SPICE clients the first `virt-viewer` connect after a boot
  shows a black screen until you `Esc`+`Space`.  The screenshot in
  `/tmp/rig-final/greeter-01-idle.png` was taken after that same wake
  sequence (`sudo virsh send-key gnome-dev KEY_ESC`, then
  `KEY_SPACE`).
- If `Nostr User` doesn't appear in the tile list, GDM cached an
  older nss projection — restart gdm (`sudo systemctl restart gdm`)
  and it'll pick up the current `nss.db`.
- If the phone can't hit `wss://nos.lol` from its network (rare, but
  captive-portal WiFi sometimes filters WSS), swap the value in
  `/etc/nostr-auth/auth.conf` for any public non-authed relay
  (`wss://relay.snort.social`, `wss://nostr.wine`) and
  `systemctl restart nostr-authd`.  The relay embedded in the QR is
  whichever one is first in that CSV.
- `wss://relay.damus.io` is on the deny-list for this rig: the earlier
  partial run (see `docs/reviews/nip46-greeter-acceptance-2026-09-22.md`
  §5) proved it silently drops `["EVENT",…]` publishes for our
  ephemeral clients.  Don't use it here.

## 8. REAL-PHONE RESULT — 2026-09-22 17:44 UTC — SUCCESS

The maintainer logged into GNOME at the real GDM greeter by scanning the QR card with
their phone NIP-46 signer app holding the operator key (`npub1ehhfg…szjqv`). Broker
journal (`nostr-authd`) and the real `gdm-password` PAM service recorded:

```
17:44:16 [nip46] await_connect: matched, signer=ba68d81925a53db21d5c56a336981ab1c4336d4e8425e79ee0c2d4e3a373859f
17:44:18 [nip46] get_public_key: SUCCESS - result: cdee943cbb19c51ab847a66d5d774373aa9f63d287246bb59b0827fa5e637400
17:44:19 [nip46] sign_event: SUCCESS
17:44:19 gdm-password]: pam_nostr(gdm-password:auth): nostr: authenticate n_bizarro (nip46qr) -> ok
```

`loginctl` showed the uid-200000 session active on seat0. This closes the "human with a
phone at the console" manual step: the client-initiated `nostrconnect://` QR login is
proven with a real, independent signer implementation (not the in-repo stand-in).
Earlier `invalid_proof` lines that day are the wrong-key negative smoke tests and
scan-window timeouts. Maintainer feedback: the on-screen arrangement of the QR card vs
the greeter elements is "a bit wonky" — layout polish follow-up (nostrc-zcll.6).

Observation: `loginctl` reported the session user as `n_smbd7g` for uid 200000 — a
leftover local /etc/passwd user from the D7 gvfs acceptance shares uid 200000 with the
seeded account (files precede nostr in nsswitch for uid lookups). Cosmetic here, but
the seeder/authority should avoid uids already present in /etc/passwd, and acceptance
scripts must userdel their throwaway users — follow-up filed.
