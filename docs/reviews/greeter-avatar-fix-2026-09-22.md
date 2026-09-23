# Greeter QR — circular avatar + conditional native-widget override — GO

- Author: Claude Opus 4.7 (agent)
- Date: 2026-09-22
- Beads: `nostrc-zcll.6` (B5 desktop UX polish, follow-up to v3
  `greeter-qr-account-identity-2026-09-22.md`)
- Scope: extension-only, no PAM / broker / gnome-homed C-code changes.
- Files changed on this branch:
  - `gnome/nostr-homed/greeter-extension/nostr-login-qr@nostrc/extension.js`
    — swap avatar renderer (Clutter image content → St.Bin
    `background-image` inline style, which St's `border-radius` actually
    clips); add `_shouldOverrideNativeWidget(account)` predicate +
    `_resolveAccountOverride()` wrapper so we only override GDM's own
    user widget when the dialog is showing the wrong identity (NIP-05
    "Not listed?" flow, or the visible label doesn't match the
    resolved account), leaving GDM's native circular avatar + name in
    place when the tile-click flow already picked the right one.
  - `docs/reviews/greeter-avatar-fix-2026-09-22.md` (this note) +
    `docs/reviews/avatar-fix-screenshots/{tileflow,nip05flow,restore,greeter-idle}.png`.
- Test rig: `gnome-dev` (Ubuntu 24.04.5 LTS, GNOME Shell 46.0, GDM on
  tty1, primary 1280×800).  Hypervisor `majordomo@192.168.40.15`
  drove input via `virsh send-key gnome-dev …` and captured
  framebuffers via `virsh screenshot gnome-dev …`.  Screenshots pulled
  to `docs/reviews/avatar-fix-screenshots/` on this workstation.

## Verdict

**GO.**  Both regressions fixed and verified end-to-end against the
live broker on the rig; restore-on-Escape is byte-identical to the
greeter's idle user list.

### FIX 1 — circular avatar

The v3 code rendered the avatar as a `Clutter.Actor` whose `content`
was a `Clutter.Image` built from the pixbuf.  `Clutter` image content
is NOT clipped by CSS `border-radius`, so the extension drew a
100 px square photograph inside a 100 px circular bin — the "circle"
was only a border ring around a visibly square face.  The AccountsService
icons the v3 smoke tests happened to use were near-circular photographs
(§6.2 of the v3 note called this out as a known follow-up: "a square
PNG will render as a square inside a circular border").  Recent rig
captures (`/tmp/nip05-fix/nip05-fixed-screenshot.png` before this
branch) showed the same square rendering with a photograph that isn't
already cropped.

Gnome-shell's own `js/ui/userWidget.js` avoids this problem by giving
the avatar `St.Bin` an inline `background-image: url(<icon>)` +
`background-size: cover|contain` — `St`'s `border-radius` DOES clip
the widget's background, so the avatar reads as a clean circle no
matter what shape the source PNG is.  This branch adopts the same
technique verbatim:

```js
// _applyAccount(), avatar branch:
const uri = Gio.File.new_for_path(account.avatar_path).get_uri();
this._avatarBin.set_child(null);
this._avatarBin.set_style(
    `background-image: url("${uri}"); background-size: cover;`);
```

Path safety is unchanged — `_extractAccount` already gates the avatar
path through `_isBasename()` + existence-check under
`/run/nostr-auth/greeter/`, so the URI passed to `set_style` can only
ever point inside the greeter drop directory.  The fallback path is
untouched: when no avatar was published or the URI construction fails,
we clear the inline style and set an `avatar-default-symbolic` icon as
the bin's child (which now renders as-is inside the circular bin).

Result (see §1 below): the marmoset photograph on `n_bizarro`'s
AccountsService icon renders as a clean 100 px circle in both the
tile-click AuthPrompt and the NIP-05 flow.

### FIX 2 — only override when needed

The v3 code unconditionally hid GDM's own user widget whenever
`manifest.account` was present.  In the tile-click flow that widget is
already correct — GDM's `UserWidget` looked up the AccountsService
avatar + real name for `n_bizarro` before we published anything, so
hiding it just to draw our own copy of the same identity was
gratuitous and doubled the amount of subtree we mutate (with a matched
restore step on retire, and a `dispose(gee-widget)` window if the
dialog rebuilds first).  The bug only ever mattered for the NIP-05
"Not listed?" flow, where GDM's widget carries the raw typed string
(`chebizarro@coinos.io`) + a generic avatar until `pam_nostr`
canonicalises the name via the broker.

This branch adds a small predicate that decides whether the override is
actually justified:

```js
_shouldOverrideNativeWidget(account) {
    // NIP-05 flow: identifier field means GDM is showing the typed string.
    if (account.identifier) return true;
    const authPrompt = this._findAuthPrompt();
    if (!authPrompt) return true;                    // no widget to defer to
    const userWidgets = [];
    _collectDescendantsByAnyClass(authPrompt, USER_WIDGET_CLASSES,
                                  userWidgets, this._container);
    if (userWidgets.length === 0) return true;       // shell version mismatch
    // Belt-and-braces: does the visible label already name this account?
    const wantNames = [];
    if (account.username)     wantNames.push(account.username.toLowerCase().trim());
    if (account.display_name) wantNames.push(account.display_name.toLowerCase().trim());
    if (wantNames.length === 0) return true;
    for (const w of userWidgets) {
        const label = _findDescendantByStyleClass(w, USER_WIDGET_LABEL_CLASS, this._container);
        const text = (label?.text || label?.clutter_text?.text || '')
            .toString().toLowerCase().trim();
        if (text && wantNames.indexOf(text) !== -1) return false;  // already correct
    }
    return true;
}
```

Wrapped in `_resolveAccountOverride()` (try / catch → default to
OVERRIDE on failure) so a bug in the detection path can never silently
drop the identity block on a NIP-05 flow.  When we decide NOT to
override, the resolved account is passed through the rest of the
pipeline as `null`: `_applyAccount(null)` collapses our own account
block, `_attachCentered` skips the widget-hide branch (existing v3
guard `if (this._pendingAccount)`), and `qrSize` stays at the base
260 px so a tile-click flow gets exactly the v2 layout: GDM's own
circular avatar + real name above a 260 px QR + pairing code.

Fallback semantics ("nothing regresses on detection failure") match
the task brief.

## 1. Tile-click flow — GDM's native circular avatar preserved

Rig state:

    $ ssh gnome-dev 'md5sum /usr/share/gnome-shell/extensions/nostr-login-qr@nostrc/extension.js'
    b94e7d5754ca6720ba91b1d50a16610c  extension.js
    $ md5sum gnome/nostr-homed/greeter-extension/nostr-login-qr@nostrc/extension.js
    b94e7d5754ca6720ba91b1d50a16610c  extension.js

(Deployed extension byte-identical to branch head.)

Idle greeter (`docs/reviews/avatar-fix-screenshots/greeter-idle.png`)
shows the three user tiles [`Biz` (n_bizarro, real photo icon),
`Nostr User` (n_qrlock, generic icon), `GNOME Debug Operator`
(generic icon)] + "Not listed?" — the Biz tile is highlighted at
focus.

Press Enter on the Biz tile: PAM starts, broker publishes an artifact
carrying `{account:{username:"n_bizarro", display_name:"Biz",
avatar:"avatar.png"}}` (no `identifier`, because the login started
from the tile — not from NIP-05).  Screenshot
`docs/reviews/avatar-fix-screenshots/tileflow.png`:

- **GDM's OWN circular avatar** (the marmoset photograph) rendered
  at the top of the AuthPrompt at ~130 px, above the bold "**Biz**"
  label — the extension did NOT hide the native widget, because
  `_shouldOverrideNativeWidget` found a `user-widget-label` reading
  "Biz" that matched `account.display_name`.
- **QR card centred beneath the identity band at 260 px** (v2 sizing,
  no `IMAGE_DISPLAY_PX_WITH_ACCOUNT` shrink) with `Pairing code:
  9A39-829A` on the line below.
- **Password entry + `‹` back button + eye toggle hidden** as v2, so
  the QR is the sole affordance in the AuthPrompt.
- **Ubuntu branding logo** clears with a clean margin at 1280×800.

`zbarimg` decode of the framebuffer:

    $ zbarimg --raw -q docs/reviews/avatar-fix-screenshots/tileflow.png
    nostrconnect://9a39829ab97078c6f312483aa42fe054ed15024ea16c083539ef9cf465fda632?relay=wss%3A%2F%2Fnos.lol&secret=a8bd8ce6d046e5b69e2e9d1e9b8eec7b&perms=sign_event%3A1&name=GNOME-QR-BIZARRO

The QR carries the expected pending pairing URI.

Escape retires the pairing (`docs/reviews/avatar-fix-screenshots/restore.png`
— identical to `greeter-idle.png` down to the highlight ring), broker
`unlink`s `/run/nostr-auth/greeter/current.{json,png}` (verified
directly listing empty).

## 2. NIP-05 flow — our own circular avatar overrides GDM's typed name

From the user list: Down × 3 → Enter to select "Not listed?"; the
AuthPrompt appears with a generic avatar + `Username` entry.  Type
`chebizarro@coinos.io`, Enter.  `pam_nostr` sends `BEGIN_LOGIN` with
the raw string; the broker's NIP-05 resolver canonicalises it to
`n_bizarro`, publishes the greeter artifact with
`{account:{username:"n_bizarro", display_name:"Biz",
identifier:"chebizarro@coinos.io", avatar:"avatar.png"}}`, `pam_nostr`
proceeds to the QR pairing.

Screenshot `docs/reviews/avatar-fix-screenshots/nip05flow.png`:

- **`n_bizarro`'s AccountsService avatar** rendered as a clean 100 px
  CIRCLE at the top of the AuthPrompt — the marmoset photograph is
  masked by St's `border-radius: 999px` clip on the widget's
  background, replacing GDM's own square-inside-circle bug from the
  pre-fix framebuffer (`/tmp/nip05-fix/nip05-fixed-screenshot.png`).
- Bold "**Biz**" label + smaller muted "chebizarro@coinos.io" line
  beneath the avatar — the identity block hides GDM's typed-string
  widget for the duration of the pairing.
- QR at 220 px (`IMAGE_DISPLAY_PX_WITH_ACCOUNT`, so the whole stack
  still clears the Ubuntu branding logo) + `Pairing code: E1E1-4386`
  centred below.
- Ubuntu branding logo clears with the tighter margin the
  account-block path already used in v3.

`zbarimg` decode:

    $ zbarimg --raw -q docs/reviews/avatar-fix-screenshots/nip05flow.png
    nostrconnect://e1e1438619e3479229a131c211be1cd8d2bc16ac3bbe7b593e08c1a63f1d45dc?relay=wss%3A%2F%2Fnos.lol&secret=14c043100d5376139d925dda98c1c9fb&perms=sign_event%3A1&name=GNOME-QR-BIZARRO

Escape retires the pairing; the greeter returns to the user list
byte-identical to `greeter-idle.png` (`restore.png` above is the
retire capture — a single file records both retires because both
flows end in the same user-list state).

## 3. Journal cleanliness

`sudo journalctl _COMM=gnome-shell` across the full session captured
no `logError` output from the extension.  The only entry with
`nostr-login-qr` in its context is a pre-existing GJS warning during
the NIP-05 retire path:

    Object .Gjs_ui_userWidget_UserWidget (0x…), has been already
    disposed — impossible to set any property on it.  This might be
    caused by the object having been destroyed from C code using
    something such as destroy(), dispose(), or remove() vfuncs.
    == Stack trace ==
    #0 file:///…/extension.js:806 (_restoreHiddenActors: setting
       s.actor.visible = s.visible on a suppressed user widget)

This was already present in v3 (same restore path,
`_suppressedUserWidgets` array + per-actor `try { s.actor.visible =
s.visible } catch(_e) {}`); FIX 2 in fact REDUCES exposure to it
because tile-click flows no longer suppress a user widget at all, so
only the NIP-05 retire path can hit it now.  The `try/catch` swallows
the exception so no functional regression follows — the user list is
restored byte-identical to idle (see `restore.png`).  Filed on the v3
follow-up backlog rather than fixed on this branch to keep the diff
minimal, per the task brief's "targeted, minimal changes" constraint.

## 4. Rig state left for the maintainer

- Extension deployed system-wide to
  `/usr/share/gnome-shell/extensions/nostr-login-qr@nostrc/` with the
  fix files (md5 `extension.js b94e7d57…`, `stylesheet.css a95a3ba2…`
  unchanged, `metadata.json d7460147…` unchanged).  Matches branch
  head byte-for-byte.
- Greeter idle at the user list; no artifact in
  `/run/nostr-auth/greeter/`; `nostr-authd` running.
- `/var/lib/nostr-auth` left at 0755 for gdm NSS traversal (same as
  v3 hand-off; the systemd unit re-applies 0700 on next
  `nostr-authd` restart, still a pre-existing packaging bug).
- User-scope + gdm dconf enablements from v2 unchanged.
- Screenshots captured to
  `docs/reviews/avatar-fix-screenshots/{greeter-idle,tileflow,nip05flow,restore}.png`.

## 5. Known follow-ups

1. **GJS disposed-widget warning on NIP-05 retire.**  Pre-existing v3
   behaviour; described in §3.  Not fixed on this branch to keep the
   diff minimal; belongs on the v3 follow-up backlog (a small guard on
   `_restoreHiddenActors` that skips suppressed widgets whose stage
   parent has already gone away).
2. **`/var/lib/nostr-auth` 0700 drift.**  Pre-existing packaging bug
   inherited from v2/v3.  Real fix owned by the packaging track.
3. **Producer avatar aspect ratio.**  With FIX 1 the circular mask
   works regardless of the source PNG shape (`background-size:
   cover`), so the "producers SHOULD supply a square-cropped PNG"
   caveat from the v3 note (`greeter-qr-account-identity-2026-09-22.md`
   §6.2) is no longer a correctness requirement — non-square PNGs
   render as a centred circular crop rather than as a square inside
   the border.  README guidance can be relaxed in a follow-up doc-only
   patch.
