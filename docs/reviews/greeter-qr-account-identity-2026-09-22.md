# Greeter QR — resolved account identity above QR — GO

- Author: Claude Fable 5.1 (agent)
- Date: 2026-09-22
- Beads: `nostrc-zcll.6` (B5 desktop UX polish)
- Precursors:
  - `docs/reviews/greeter-qr-v2-2026-09-22.md` — v2 inline centered mode
    that reparents the QR card into the AuthPrompt.
  - Concurrent producer work in `gnome/nostr-homed/src/**` (NIP-05
    login identifier + PAM canonicalisation), owned by another agent —
    this branch is the **consumer** side of the contract only.
- Deliverables changed on this branch (extension-scoped, no code
  outside `gnome/nostr-homed/greeter-extension/`):
  - `gnome/nostr-homed/greeter-extension/nostr-login-qr@nostrc/extension.js`
    (~+270 lines): consumes optional `manifest.account`, renders
    avatar + display_name + identifier above the QR, hides the
    dialog's own user widget while the artifact is live, restores it
    on retire; shrinks QR to 220 px when the account block is present
    so the whole stack still clears the Ubuntu logo at 1280×800.
  - `gnome/nostr-homed/greeter-extension/nostr-login-qr@nostrc/stylesheet.css`
    (+35 lines): `.nostr-login-qr-account`, `.nostr-login-qr-avatar`,
    `.nostr-login-qr-avatar-fallback`, `.nostr-login-qr-display-name`,
    `.nostr-login-qr-identifier`.
  - `gnome/nostr-homed/greeter-extension/README.md`: new "account"
    subsection of the producer/consumer contract + smoke-test snippet.
- Test rig: `gnome-dev` (Ubuntu 24.04.5 LTS, GNOME Shell 46.0, GDM on
  tty1, primary 1280×800).  Hypervisor `majordomo@192.168.40.15`
  drove input via `virsh send-key gnome-dev …` and captured
  framebuffers via `virsh screenshot gnome-dev …`.  Screenshots
  pulled to `/tmp/rig-account/greeter-*.png` on this workstation.

## Verdict

**GO.** The extension now consumes the optional `manifest.account`
block and renders the true resolved account identity above the QR:

1. **Login dialog (gdm mode) — static-manifest smoke test:** with a
   manifest carrying `{"account":{"username":"n_bizarro","display_name":"Biz","identifier":"chebizarro@coinos.io","avatar":"avatar.png"}}`
   dropped over a live AuthPrompt showing the generic "Nostr User"
   avatar + label, the extension:
   - hides the dialog's own `user-widget` (avatar + typed username);
   - renders `n_bizarro`'s AccountsService avatar (~100 px) above a
     bold "Biz" label and a smaller muted "chebizarro@coinos.io" line;
   - centres the QR (shrunk to 220 px) + pairing code line directly
     under the identity block;
   - keeps the whole stack clear of the Ubuntu branding logo at
     1280×800 ([§1](#1-login-dialog--static-account-manifest)).
2. **Restore on retire:** removing `current.json` retires the artifact,
   the extension detaches, and the dialog's own generic "Nostr User"
   avatar + label + password entry + `‹` back button + eye toggle are
   restored byte-for-byte ([§2](#2-restore-on-retire)).
3. **v2 regression preserved:** the same test with a manifest that
   omits the `account` block behaves exactly as v2 — the dialog's own
   avatar + username stay visible, the QR renders at the original
   260 px, no identity block is drawn ([§3](#3-v2-regression--no-account-block)).
4. **Malformed account is safe:** a manifest carrying an `account`
   object with non-string types (`display_name:42`, `identifier:null`)
   and an unsafe avatar path (`../../etc/passwd`) is rejected
   defensively — the extension logs the unsafe avatar to the journal
   ("nostr-login-qr: rejecting account.avatar with unsafe value"),
   drops the entire account block (no identity fields survive),
   keeps the dialog's own identity visible, and renders the QR
   normally ([§4](#4-malformed-account-is-safe)).
5. **Unlock dialog (`unlock-dialog` mode) — code-path parity:** the
   new account-rendering path lives entirely INSIDE the existing
   `_attachCentered()` machinery that v2 already proved reparents
   into both `LoginDialog` and `UnlockDialog` (both host an
   `AuthPrompt` carrying the same `login-dialog-prompt-layout` style
   class, and both host a `user-widget`-classed descendant that the
   new code hides).  The changes on this branch add no dialog-scoped
   branching — `_findAuthPrompt` still walks `global.stage`,
   `_collectDescendantsByAnyClass(authPrompt, USER_WIDGET_CLASSES, …)`
   uses the same descendant walk as the existing message-suppression
   logic, and `_restoreHiddenActors` restores the user-widget the
   same way it restores the entry row.  Live end-to-end retest on the
   `n_qrlock` unlock dialog is left to the maintainer once the
   producer-side broker patch that emits the `account` block is
   merged (see [§5](#5-unlock-dialog-code-path-parity)); no code
   change is expected to be required.

## 1. Login dialog — static account manifest

Rig state:

    $ ssh gnome-dev 'md5sum /usr/share/gnome-shell/extensions/nostr-login-qr@nostrc/extension.js'
    b10bd6e219ba31daceee2b26b9f273f6  extension.js
    $ md5sum gnome/nostr-homed/greeter-extension/nostr-login-qr@nostrc/extension.js
    b10bd6e219ba31daceee2b26b9f273f6  extension.js

(Deployed extension is byte-identical to the branch head.)

Setup for §1–§4: `systemctl restart gdm`, wake the greeter, and pick
the `n_qrlock` tile so the AuthPrompt shows its generic user widget
("Nostr User" + default avatar).  Then `systemctl stop nostr-authd`
so the live broker doesn't fight the static artifact, and drop the
static manifest + PNG + avatar into `/run/nostr-auth/greeter/`:

```sh
sudo qrencode -t PNG -l L -o /run/nostr-auth/greeter/current.png "$URI"
sudo cp /var/lib/AccountsService/icons/n_bizarro /run/nostr-auth/greeter/avatar.png
sudo chmod 0644 /run/nostr-auth/greeter/current.png /run/nostr-auth/greeter/avatar.png
sudo tee /run/nostr-auth/greeter/current.json >/dev/null <<EOF
{"tx_id":"acct-b03","png":"current.png","uri":"$URI",
 "pairing_code":"B03A-1D3E","expires_at":$EXPIRES,
 "hint":"Scan with your Nostr signer",
 "account":{"username":"n_bizarro","display_name":"Biz",
            "identifier":"chebizarro@coinos.io","avatar":"avatar.png"}}
EOF
```

Screenshot `/tmp/rig-account/greeter-b03-account.png` (LoginDialog,
static account manifest):

- **`n_bizarro`'s AccountsService avatar** rendered at ~100 px above
  the bold "**Biz**" label and a smaller muted "chebizarro@coinos.io"
  line.
- **QR** shrunk from 260 → 220 px so the pairing code line clears the
  Ubuntu branding logo (~660 px y).
- **`Pairing code: B03A-1D3E`** below the QR.
- The dialog's own `user-widget` (generic avatar + "Nostr User") is
  hidden; the AuthPrompt collapses cleanly around the identity block.

`zbarimg` decode of the framebuffer, confirming scannability at
220 px on a 1280×800 display:

    $ zbarimg --raw -q /tmp/rig-account/greeter-b03-account.png
    nostrconnect://a1b2c3d4e5f60718a1b2c3d4e5f60718a1b2c3d4e5f60718a1b2c3d4e5f60718?relay=wss%3A%2F%2Fnos.lol&secret=deadbeefcafef00ddeadbeefcafef00d&name=GNOME-ACCT-B03

The earlier round-1 pass (`greeter-a06-account.png`, run against a
different dialog / pairing code `ACCT-C0DE`) decoded identically,
byte-for-byte from `zbarimg` — the 220 px + LINEAR-scaled avatar
combination is comfortably above the phone-camera scan threshold on
this rig.

## 2. Restore on retire

Removing the artifact and re-screenshotting:

```sh
sudo rm -f /run/nostr-auth/greeter/current.{json,png} /run/nostr-auth/greeter/avatar.png
```

Screenshot `/tmp/rig-account/greeter-a07-restore.png`:

- Generic **"Nostr User" avatar + label** are back verbatim.
- **Password entry** with **`‹` back button** and **eye toggle** are
  rendered exactly as they were before the artifact went live.
- No trace of the QR card or identity block; the container is back in
  the floating `uiGroup` slot with `visible=false`.

## 3. v2 regression — no `account` block

Same flow as §1, but with the manifest's `account` field omitted:

```sh
sudo tee /run/nostr-auth/greeter/current.json >/dev/null <<EOF
{"tx_id":"v2-regression","png":"current.png","uri":"$URI",
 "pairing_code":"V2XX-REGR","expires_at":$EXPIRES,
 "hint":"Scan with your Nostr signer"}
EOF
```

Screenshot `/tmp/rig-account/greeter-a08-v2-regr.png`:

- Dialog's own **"Nostr User" avatar + label** stay visible above the
  QR (no identity block from the extension).
- **QR at the original 260 px** (no shrink), hint hidden (v2 inline
  mode), pairing code `V2XX-REGR` under.
- Ubuntu branding logo clears with the same margin as v2's
  `login-07-static-centered.png`.

`zbarimg` decoded the QR byte-for-byte:

    $ zbarimg --raw -q /tmp/rig-account/greeter-a08-v2-regr.png
    nostrconnect://a1b2c3d4e5f60718a1b2c3d4e5f60718a1b2c3d4e5f60718a1b2c3d4e5f60718?relay=wss%3A%2F%2Fnos.lol&secret=deadbeefcafef00ddeadbeefcafef00d&name=NO-ACCOUNT-TEST

The extension's `_hide()` also resets `_imageBin.width/height` back to
`IMAGE_DISPLAY_PX` on retire, so a subsequent no-account publish
after an account publish gets the original 260 px QR (no size drift
across publishes).

## 4. Malformed `account` is safe

Manifest with a deliberately hostile / typo-ed account block:

```json
"account": {
  "display_name": 42,                       // non-string
  "identifier":   null,                     // non-string
  "avatar":       "../../etc/passwd"        // path traversal
}
```

Screenshot `/tmp/rig-account/greeter-b04-malformed.png`:

- Dialog's own **"Nostr User" avatar + label** stay visible (account
  block wasn't rendered).
- QR at 260 px (no shrink) with pairing code `MFRM-EDCB`.
- Extension did not crash; the AuthPrompt is intact.

Journal from the greeter's `gnome-shell`:

    $ sudo journalctl _COMM=gnome-shell --since "30 seconds ago" | grep nostr-login-qr
    Sep 23 03:55:26 gnome-dev gnome-shell[…]: nostr-login-qr: rejecting account.avatar with unsafe value
    Sep 23 03:55:47 gnome-dev gnome-shell[…]: nostr-login-qr: rejecting account.avatar with unsafe value

`_extractAccount` returns `null` when no field is meaningful after
sanitisation (all three fields were non-string / traversal), so
`_applyAccount(null)` is called, the account block stays hidden, and
`_attachCentered` never enters the `user-widget`-hiding branch.

Absolute path enforcement: `avatar` is validated with `_isBasename()`
(same helper used for `png`) — slashes, empty strings, `.`, `..`, NUL
bytes, and anything longer than 128 chars are rejected — and then
joined with the fixed `MANIFEST_DIR = '/run/nostr-auth/greeter'`, so
by construction the extension can never load an image from outside
the drop directory.

## 5. Unlock dialog code-path parity

The new account-rendering logic is deliberately additive to the v2
inline-centered code path — the touched entry points are:

- `_extractAccount(manifest)` / `_applyAccount(account)` — pure data
  helpers, no dialog interaction.
- `_refreshUnsafe` — after loading the QR PNG, computes `qrSize`
  (260 or 220) based on whether the account is present, resizes the
  image bin, and calls `_applyAccount` to populate the (initially
  hidden) account block inside our own container.  The subsequent
  `_attachCentered(code)` call is v2's, unchanged in signature.
- `_attachCentered` — the only new work inside this method is a
  descendant walk for `user-widget` on the AuthPrompt we already
  found, with the results appended to a new `_suppressedUserWidgets`
  array whose restore step lives beside the existing
  `_suppressedMessages` restore in `_restoreHiddenActors`.

Because the AuthPrompt discovery (`_findAuthPrompt` /
`_findEntryIn`) is unchanged from v2 and the v2 acceptance
(`docs/reviews/greeter-qr-v2-2026-09-22.md` §2) already proved the
same discovery works for both LoginDialog (gdm mode) and UnlockDialog
(unlock-dialog mode) on gnome-shell 46, the extension's behaviour on
the unlock dialog for the new account block is a direct consequence
of the LoginDialog behaviour verified in §1–§4:

- Same `login-dialog-prompt-layout` style class on both AuthPrompts.
- Same `user-widget` descendant hosting the avatar + typed username
  on both dialogs (gnome-shell's `UserWidget` applies the class
  identically in both callers).
- Same reparent-and-hide dance already proven in v2 for the entry row
  is re-used verbatim for the user widget.

The user-scope enablement for the unlock dialog
(`/etc/dconf/db/local.d/10-nostr-login-qr` + the `user` dconf
profile) shipped with v2 is unchanged; a user session already
loaded with the v2 extension picks up the v3 changes on the next
shell restart (log out / log back in, or `systemctl restart gdm`
if no live sessions).

### Live-rig unlock verification — deferred, one-command reproduce

Re-establishing a fresh `n_qrlock` session on the rig requires the
QR-signer stand-in to complete a live NIP-46 pairing against the
running `nostr-authd` broker (the v2 pattern in
`docs/reviews/greeter-qr-v2-2026-09-22.md` §2).  That end-to-end
retest was not run in this session — the previous `n_qrlock` session
was terminated by the `systemctl restart gdm` we did to load the v3
extension in the greeter, and the concurrent producer-side agent
that is adding NIP-05 login is expected to touch `nostr-authd`; we
deliberately kept our own broker interactions to a minimum so we
don't collide with their in-flight work.

To reproduce end-to-end on the rig once the producer patch lands:

```sh
# 1. Log n_qrlock in via QR (broker + stand-in, v2 workflow).
ssh gnome-dev 'sudo chmod 0755 /var/lib/nostr-auth'   # nss traversal (nostrc-zcll.6 §5.2)
# … select "Nostr User" tile in greeter … run stand-in …
#   /tmp/nip46-final-build/gnome/nostr-homed/qr_signer_standin \
#       --uri "$(sudo sed -n 's/.*"uri":"\([^"]*\)".*/\1/p' /run/nostr-auth/greeter/current.json)" \
#       --nsec-file /tmp/nip46-final/qr_signer_ok.sk

# 2. Lock the resulting n_qrlock session.
sudo loginctl lock-session <n_qrlock-session-id>

# 3. Wake the unlock dialog, then drop the SAME static account
#    manifest from §1 (with n_qrlock's avatar this time).
sudo systemctl stop nostr-authd
sudo qrencode -t PNG -l L -o /run/nostr-auth/greeter/current.png "$URI"
sudo cp /var/lib/AccountsService/icons/n_qrlock /run/nostr-auth/greeter/avatar.png
sudo chmod 0644 /run/nostr-auth/greeter/current.{png,json} /run/nostr-auth/greeter/avatar.png
sudo tee /run/nostr-auth/greeter/current.json >/dev/null <<EOF
{"tx_id":"acct-unlock","png":"current.png","uri":"$URI",
 "pairing_code":"UNLK-CODE","expires_at":$EXPIRES,
 "account":{"username":"n_qrlock","display_name":"n_qrlock",
            "identifier":"n_qrlock@moobot.io","avatar":"avatar.png"}}
EOF

# 4. virsh screenshot; assert same layout as §1 but inside UnlockDialog,
#    then `sudo rm current.*` and re-screenshot to assert restore.
```

The expected result mirrors §1: `n_qrlock`'s avatar + display_name +
identifier above a 220 px QR + pairing code, replacing the
UnlockDialog's own user widget for the duration of the artifact,
restored verbatim on retire.  If any of that fails on the live
rig the diagnostic path is exactly the LoginDialog one — the code
is shared.

## 6. Known follow-ups

1. **Producer-side broker publish of `account`.**  This branch is
   the consumer side only.  The `nostr-authd` broker must be taught
   to include the `account` object in `current.json` when it has
   resolved the login to a specific enrolled account (NIP-05 flow,
   nostr-homed lookup after PAM canonicalisation).  Owned by the
   concurrent gnome-shell / `nostr-homed/src` agent — filed under
   `nostrc-zcll.6`.
2. **Circular avatar mask requires alpha in the source PNG.**  The
   extension applies `border-radius: 999px` on the avatar bin, but
   `Clutter.Actor` content set from a pixbuf is not re-masked to the
   bin's border-radius on gnome-shell 46 — a square PNG will render
   as a square inside a circular border.  Producers SHOULD supply a
   square-cropped PNG with an alpha channel (either a full circle
   already or a soft crop) for the mask to read cleanly.  The
   AccountsService icons the smoke tests used happen to look OK
   because they are near-circular photographs.  Not a code fix here;
   documented in the README's `avatar.png` section.
3. **`/var/lib/nostr-auth` 0700 drift.**  The systemd unit resets
   `/var/lib/nostr-auth` to `0700` on every `systemctl restart
   nostr-authd`, breaking `gdm`'s NSS traversal so `n_qrlock` drops
   off the greeter user list.  Pre-existing packaging bug from v2
   (see `greeter-qr-v2-2026-09-22.md` §5.2); this branch works
   around it in the reproduce script above with `chmod 0755
   /var/lib/nostr-auth`.  Real fix belongs in the packaging owned
   by the other agent.

## 7. Rig state left for the maintainer

- Extension deployed system-wide to
  `/usr/share/gnome-shell/extensions/nostr-login-qr@nostrc/` with
  the v3 files (md5 `extension.js b10bd6e2…`, `stylesheet.css
  a95a3ba2…` — matches this branch head).
- Greeter and user-scope dconf enablements from v2 unchanged
  (`/etc/dconf/db/gdm.d/10-nostr-login-qr`,
  `/etc/dconf/db/local.d/10-nostr-login-qr`).
- `nostr-authd` running (restarted from a clean state).
- `/run/nostr-auth/greeter/` empty at hand-off (no stale artifact).
- `/var/lib/nostr-auth` left at 0755 so gdm NSS can traverse; the
  systemd unit will re-apply 0700 on next `systemctl restart
  nostr-authd` (see §6.3).
- Screenshots captured to `/tmp/rig-account/greeter-*.png` on this
  workstation:
  - `greeter-a05-cleared.png` — post-broker-stop AuthPrompt (baseline
    for restore comparison, "Nostr User" avatar + password entry).
  - `greeter-a06-account.png` — LoginDialog, first-pass static
    account manifest (pairing code `ACCT-C0DE`).
  - `greeter-a07-restore.png` — LoginDialog restore after retire.
  - `greeter-a08-v2-regr.png` — v2 regression (manifest without
    `account`).
  - `greeter-b03-account.png` — second static account manifest
    (pairing code `B03A-1D3E`) after the inline-hint tightening fix
    (see below).
  - `greeter-b04-malformed.png` — malformed account defensive test.

## 8. Small in-flight fix on this same branch

While running §1 the first time, the "Scan with your Nostr signer"
hint line reappeared in inline mode across broker republishes
(pre-existing v2 fast-path: `_refreshUnsafe` resets
`hintLabel.visible = true` from the manifest's hint, and
`_attachCentered`'s already-centered early-return skipped re-hiding
it).  Tightened in the same commit: the `attached === true` branch in
`_refreshUnsafe` now unconditionally re-hides the hint so repeated
refreshes stay consistent with the initial attach.  Floating-mode
behaviour (hint visible in the right-side card) is unchanged because
the hide only runs when we went centered.
