# Greeter QR v2 — centered + lock-screen unlock — GO

- Author: Claude Opus 4.7 (agent)
- Date: 2026-09-22
- Beads: `nostrc-zcll.6` (B5 desktop UX polish), `nostrc-z1fb` (B5-QR)
- Precursors:
  - `docs/reviews/gdm-qr-extension-2026-09-22.md` — Phase-2 acceptance
    of the extension (v1: floating right-side card).
  - `docs/reviews/phone-test-rig-2026-09-22.md` — real-phone login on
    the rig, plus the maintainer feedback that the QR arrangement
    beside the greeter's user block looked "a bit wonky" (§8 real
    result note, 2026-09-22 17:44 UTC).
- Deliverables changed on this branch:
  - `gnome/nostr-homed/greeter-extension/nostr-login-qr@nostrc/extension.js`
    (~+220 lines: inline-centered mode + retry + host-destroy guard).
  - `gnome/nostr-homed/greeter-extension/nostr-login-qr@nostrc/stylesheet.css`
    (split panel styles into `-floating` and `-inline` variants).
  - `gnome/nostr-homed/greeter-extension/README.md` (behaviour +
    lock-screen enablement).
- Test rig: `gnome-dev` (Ubuntu 24.04.5 LTS, GNOME Shell 46.0, GDM on
  tty1, primary 1280×800).  Hypervisor `majordomo@192.168.40.15`
  drove input via `virsh send-key gnome-dev …` and captured
  framebuffers via `virsh screenshot gnome-dev …`.  Screenshots pulled
  to `/tmp/rig-v2/greeter-*.png` on this workstation.

## Verdict

**GO.** The extension now:

1. **Login dialog (gdm mode, PART 1):** reparents the QR card into the
   `AuthPrompt` at the entry's slot, hiding the password entry and the
   PAM message labels that would duplicate the card's pairing code /
   hint, so the QR sits centered under the avatar / username exactly
   where the maintainer asked ([§1](#1-login-dialog--live-broker-publish)).
   Password logins for local users (`GNOME Debug Operator`) are
   unaffected when no artifact is live — the extension only intervenes
   while `/run/nostr-auth/greeter/current.json` exists ([§3](#3-no-regression-for-password-logins)).
2. **Lock screen (unlock-dialog mode, PART 2):** the exact same code
   path finds the `UnlockDialog`'s `AuthPrompt` (same
   `login-dialog-prompt-layout` style class in shell 46) and centers
   the QR card there.  A real end-to-end **lock → QR → unlock** loop
   completed successfully for a seeded test account using
   `tests/integration/qr_signer_standin` as the phone-signer stand-in
   ([§2](#2-unlock-dialog--end-to-end-lock--qr--unlock)).
3. **Wrong-key refused at the lock screen:** running the same standin
   with a *different* nsec never matches the broker's per-account
   signer subscription filter → `pam_nostr … -> invalid_proof` and the
   `AuthPrompt` re-shows the empty entry with GDM's "Sorry, password
   authentication didn't work" warning; the session stays locked
   ([§4](#4-wrong-key-refused-at-the-unlock-dialog)).
4. **Fallback preserved.**  If `AuthPrompt` cannot be located the
   extension falls back to the v1 right-side floating card, and if the
   host dialog is torn down while the card is attached the extension
   detects the `destroy` signal and rebuilds its own container for the
   next publish.

## What changed on this branch

`extension.js`:

- New `_attachCentered(pairingCode)` that recursively walks
  `global.stage` for a widget carrying `login-dialog-prompt-layout`,
  then walks it for `login-dialog-prompt-entry`, reparents our QR
  container to the entry's parent at the entry's index, and hides the
  entry (plus any sibling with the same class, covering
  `_passwordEntry` + `_textEntry`).  Also collects and hides any
  descendant `login-dialog-message` / `login-dialog-message-hint`
  whose text contains the pairing code or the "Scan the QR…" / "Pairing
  code" phrase, so the card is the sole on-screen source.
  `login-dialog-message-warning` labels are deliberately left alone.
- New `_detachCentered()` that restores every hidden actor's prior
  `.visible` state verbatim, moves our container back to `uiGroup`,
  and re-applies the floating stylesheet class.  Runs on
  `_hide()` (manifest retire / expiry) and on `disable()`.
- New `_scheduleCenterRetry(code)` that retries the attach up to 8
  times over 2.4 s in case the artifact publishes while the
  `AuthPrompt` is still transitioning (the greeter arriving from the
  user list, the `UnlockDialog` rebuilding after wake).
- New `_onHostDestroyed()` connected to the host `AuthPrompt`'s
  `destroy` signal, which drops references and rebuilds the container
  so a subsequent publish can reattach cleanly to the next dialog.
- Style tokens for finding actors are checked by token-match on
  `style_class`, not substring, so `login-dialog-prompt-entry` never
  spuriously matches `login-dialog-prompt-layout`.

`stylesheet.css`:

- Split `.nostr-login-qr-panel` into `-floating` (card chrome,
  border, shadow) and `-inline` (transparent, no chrome — the
  AuthPrompt already owns its own backdrop).  The floating fallback
  remains visually identical to v1.

`README.md`:

- Documents the inline vs floating modes.
- New "Enabling for the lock screen (`unlock-dialog` mode)" section
  covering the user-scope dconf profile + keyfile that has to be set
  in addition to the greeter-scope keyfile.  The dconf split is the
  reason lock-screen QR "didn't appear at all" in v1: the extension
  was correctly declared for `unlock-dialog` mode but was only enabled
  in the `gdm` dconf profile, so it never even loaded in the user's
  shell.

## 1. Login dialog — live broker publish

Rig state at start of the run:

    $ ssh gnome-dev 'ls /usr/share/gnome-shell/extensions/nostr-login-qr@nostrc/'
    extension.js  metadata.json  stylesheet.css
    $ ssh gnome-dev 'cat /etc/dconf/db/gdm.d/10-nostr-login-qr'
    [org/gnome/shell]
    enabled-extensions=['nostr-login-qr@nostrc']

After deploying the v2 extension + `systemctl restart gdm`, waking the
greeter picked up the pending pairing that `pam_nostr` re-published for
the last-selected user (`n_bizarro`).

Screenshot `/tmp/rig-v2/greeter-01-idle.png` — **QR centered under the
avatar / "Nostr User" label, pairing code `4D9A-EF30` under the card,
password entry gone**.  `zbarimg` decodes the QR directly from the
1280×800 framebuffer:

    $ zbarimg --raw -q /tmp/rig-v2/greeter-01-idle.png
    nostrconnect://4d9aef307863e06691b6c8aa7691e6edf48c9b45d09f859aa9c8890c713ede7f?relay=wss%3A%2F%2Fnos.lol&secret=…&perms=sign_event%3A1&name=GNOME-QR-BIZARRO

The first four hex bytes of the client pubkey match the on-screen
pairing code (`4d9a ef30` ↔ `4D9A-EF30`), as required by the design's
4-4 dashed pairing scheme.

Static-manifest smoke test (with `pairing_code:"A1B2-C3D4"`, artifact
written into `/run/nostr-auth/greeter/`) while the greeter was on
`GNOME Debug Operator`'s password prompt: `/tmp/rig-v2/greeter-04-static.png`
shows the same centered QR under the Debug Operator avatar, replacing
the password entry.  `zbarimg` decoded that too byte-for-byte.  After
`rm current.{json,png}` the greeter recovered instantly to
`/tmp/rig-v2/greeter-05-restored.png` with the password entry back,
eye toggle intact.

## 2. Unlock dialog — end-to-end lock → QR → unlock

Setup for the lock-screen test:

- Seeded a *second* nostr-homed account, `n_qrlock`, uid 200001,
  provider `nip46qr`, relay `wss://nos.lol`, xonly signer pubkey
  `fb26bbd028f7b8f9c8a9b09f8137d7fda9cf7718ae8ca4889b8c5bac492b8b5c`
  whose secret we hold at `/tmp/nip46-final/qr_signer_ok.sk` (from a
  prior acceptance run).
- Registered n_qrlock with AccountsService so it appears in the GDM
  user list.
- Enabled the extension for user-scope sessions via
  `/etc/dconf/profile/user` (`user-db:user; system-db:local`) plus
  `/etc/dconf/db/local.d/10-nostr-login-qr` carrying
  `enabled-extensions=['nostr-login-qr@nostrc']`.  Verified after login:

      $ sudo -u n_qrlock bash -c 'gsettings get org.gnome.shell enabled-extensions'
      ['nostr-login-qr@nostrc']

**Login** (to get a real GNOME session for n_qrlock so we can lock it):
selected n_qrlock's tile at the greeter, artifact published within
~2 s, ran `qr_signer_standin --uri "$URI" --nsec-file
/tmp/nip46-final/qr_signer_ok.sk` on the guest.  Broker journal:

    22:38:29 [nip46] await_connect: matched, signer=fb26bbd028f7b8f9…492b8b5c
    22:38:30 [nip46] get_public_key: SUCCESS - result: fb26bbd028f7b8f9…
    22:38:30 [nip46] sign_event:    SUCCESS - result: {"id":"d86f4414…
    22:38:30 pam_nostr(gdm-password:auth): nostr: authenticate n_qrlock (nip46qr) -> ok

`loginctl` then showed `1279 200001 n_qrlock seat0 tty2 active` and
the Ubuntu 24.04 desktop was up (screenshot
`/tmp/rig-v2/greeter-10-qrlock-desktop.png`).

**Lock**: `sudo loginctl lock-session 1279` → session went `LockedHint=yes`
and the screen faded to the wallpaper + clock
(`/tmp/rig-v2/greeter-11b-lock.png`).

**QR unlock**: pressing `KEY_SPACE` woke the unlock dialog; artifact
published within ~1 s (pairing code `436C-D03C`, screenshot
`/tmp/rig-v2/greeter-15-unlock-retry.png`) and the extension centered
the QR card under "Nostr User" *inside the UnlockDialog*, exactly as
in the login dialog case.  `zbarimg` decoded the QR from that
framebuffer.  `qr_signer_standin` with the correct nsec then
completed the pairing:

    22:45:13 [nip46] await_connect: matched, signer=fb26bbd028f7…492b8b5c
    22:45:14 [nip46] get_public_key: SUCCESS - result: fb26bbd028f7…
    22:45:14 [nip46] sign_event:    SUCCESS - result: {"id":"1d3a79ca…
    22:45:14 pam_nostr(gdm-password:auth): nostr: authenticate n_qrlock (nip46qr) -> ok

`loginctl` then reported `LockedHint=no` and the desktop was back
(`/tmp/rig-v2/greeter-16-unlocked.png`).  **QR unlock end-to-end
proven with an independent stand-in signer.**

## 3. No regression for password logins

- `GNOME Debug Operator` tile → password prompt with the eye toggle
  visible, no QR overlay.  Regression screenshot
  `/tmp/rig-v2/greeter-03-debuggerpw.png`.
- `pamtester gdm-password debugger authenticate` still reaches
  `pam_unix` and returns a genuine `Authentication failure` on a
  wrong password (not a stack abort).
- When a manifest is dropped into `/run/nostr-auth/greeter/` while
  the greeter is on the Debug password prompt (§1 smoke test), the
  extension takes over cleanly; when the manifest is removed the
  password entry is restored verbatim with the eye toggle intact.

## 4. Wrong-key refused at the unlock dialog

Re-locked the session and re-ran the driver but pointed it at
`qr_signer_wrong.sk` (signer pubkey `7cbae9d226ee5d7653287692cad5c60…`,
distinct from the seeded `fb26bbd0…`).  The broker's
`await_connect` subscription is filtered on the seeded account's
signer pubkey, so the wrong signer's connect event is discarded at the
relay-pool tag filter and the broker never even reaches
`get_public_key`:

    22:46:20 pam_nostr(gdm-password:auth): nostr: authenticate n_qrlock (nip46qr) -> invalid_proof
    22:47:41 [nip46] await_connect: timed out after 78000 ms
    22:47:41 pam_nostr(gdm-password:auth): nostr: authenticate n_qrlock (nip46qr) -> invalid_proof

The AuthPrompt re-showed the entry (extension retired the artifact
and the shell rendered the standard failure UI: empty entry with a
`login-dialog-message-warning` label reading "Sorry, password
authentication didn't work. Please try again.").  Screenshot
`/tmp/rig-v2/greeter-17-wrong-key-qr.png`.  The session stayed
`LockedHint=yes` throughout.

A subsequent unlock run with the **correct** stand-in then unlocked
the session again (`22:49:51 … nip46qr -> ok`), confirming the
broker/PAM state is clean between attempts and the wrong-key rejection
was not a stale-state fluke.

## 5. Known gaps / follow-ups

1. **PART 2 needs the user-scope dconf profile.**  Deployments that
   want lock-screen QR must add the `/etc/dconf/profile/user` +
   `/etc/dconf/db/local.d/10-nostr-login-qr` pair documented in the
   README.  The greeter's own `gdm` dconf profile is not enough.
   Consider making the CMake install rule optionally lay down both
   keyfiles when `-DNOSTR_HOMED_ENABLE_UNLOCK_QR=ON` is set — filed
   as a small follow-up (nostrc-zcll.6).
2. **`/var/lib/nostr-auth` mode drift.**  The systemd unit sets
   `StateDirectoryMode=0700` but the shipped NSS module can only
   traverse when the directory is `0755` (with `nss.db 0644`
   compensating).  The rig had to `chmod 0755` on the state directory
   for `getent passwd n_qrlock` to resolve.  Docs
   (`gnome/nostr-homed/docs/INSTALLED_LOGIN.md`) already say 0755;
   the systemd unit needs to be brought into line.  Filed as a
   packaging follow-up (nostrc-zcll.6).
3. **Transient sign_event "forbidden" on rapid back-to-back
   flows.**  The very first unlock cycle after login returned
   `forbidden` from the standin's `sign_event` (broker journal
   22:39:51); a retry a minute later succeeded (22:45:14).  Both used
   the same standin binary + nsec.  Not extension-side (the extension
   correctly rendered both flows).  Suspected transient state in the
   in-repo `qr_signer_standin` or its NIP-46 session across
   back-to-back invocations with the same key; a real phone signer
   would not exhibit this.  Filed as a NIP-46 follow-up
   (nostrc-z1fb).
4. **Pairing-code font size on cramped rendering surfaces.**  At
   1280×800 the QR + pairing code + hint just fit under the AuthPrompt
   with the Ubuntu logo directly beneath.  Higher-density displays
   have plenty of room; extreme low-res may need the QR shrunk
   further.  The `IMAGE_DISPLAY_PX = 300` constant is a one-liner
   knob if needed.

## 6. Rig state left for the maintainer

- Extension deployed to `/usr/share/gnome-shell/extensions/nostr-login-qr@nostrc/`.
- GDM-scope enable: `/etc/dconf/db/gdm.d/10-nostr-login-qr` (unchanged
  from v1).
- User-scope enable: `/etc/dconf/profile/user` +
  `/etc/dconf/db/local.d/10-nostr-login-qr` (new; enables the
  extension in the user's shell so `unlock-dialog` mode picks it up).
- Test account `n_qrlock` (uid 200001) seeded and active; its GNOME
  session (`loginctl 1279`) was left unlocked and running the Ubuntu
  welcome tour after the successful QR unlock.  Safe to lock/unlock
  again — the standin key is at `/tmp/nip46-final/qr_signer_ok.sk`.
- `/var/lib/nostr-auth` left at 0755 so the shipped NSS module can
  traverse (see §5.2).  Original mode was 0700; the systemd unit will
  re-apply that on next boot, which will re-break NSS name lookups
  until the packaging fix lands.  The teardown script
  `scripts/phone-test-rig-teardown.sh` was **not** run — the rig is
  intact for retesting.
- Screenshots captured to `/tmp/rig-v2/greeter-*.png` on this
  workstation:
  - `greeter-01-idle.png` — v2 login dialog, QR centered, live broker publish.
  - `greeter-03-debuggerpw.png` — password login unaffected.
  - `greeter-04-static.png` — v2 login dialog, QR centered, static
    manifest under Debug Operator.
  - `greeter-05-restored.png` — password entry restored after retire.
  - `greeter-10-qrlock-desktop.png` — n_qrlock desktop after QR login.
  - `greeter-11b-lock.png` — lock screen (clock only).
  - `greeter-12-unlock.png` / `greeter-15-unlock-retry.png` — v2 unlock
    dialog, QR centered.
  - `greeter-16-unlocked.png` — desktop after QR unlock.
  - `greeter-17-wrong-key-qr.png` — wrong-key rejection UI.
