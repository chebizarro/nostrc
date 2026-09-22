# Phase-0 spike: half-block QR rendered inside GDM greeter — NO-GO

- Author: Claude Opus 4.7 (agent)
- Date: 2026-09-22
- Design under test: [`docs/designs/nip46-qr-login-greeter.md`](../designs/nip46-qr-login-greeter.md) §3.3
- Beads: `nostrc-z1fb` (B5-QR), depends on `nostrc-zcll.6` (B5 GDM UX)
- Environment: `gnome-dev` VM — Ubuntu 24.04.5 LTS, GNOME on Wayland, GDM active, screen 1280×800.
  Hypervisor `192.168.40.15` (`majordomo`) drove input via `virsh send-key` and captured the
  console via `virsh screenshot gnome-dev` (QXL/SPICE returns PNG despite `.ppm` suffix).
- Artefacts (local, this workstation): `/tmp/qr-spike/*.png`. Nothing secret is embedded —
  the URI in the screenshots contains a throwaway ephemeral pubkey/secret used solely for the
  spike, generated locally on `gnome-dev` and never transmitted anywhere else.

## Verdict

**NO-GO** on the in-dialog half-block QR for v1. Ship the greeter with the
design’s always-there fallback (short URI + pairing code + hint) as the only
graphical-greeter surface. Config default `nip46_qr_render=uri` (per design D7).
The QR text remains generated and emitted — it is still correct and scannable
on a text console (`pamtester`, `tty1`, ssh) — but is **not shown in the
graphical greeter**. The in-dialog QR moves to Phase 2 (design §3.5): a
`/run/nostr-auth/greeter/<tx-id>.png` written by the broker plus a small
gnome-shell extension that watches the path.

This confirms the design’s §3.2 pre-mortem (proportional Cantarell + wrapping =
unscannable) as **the** dominant failure mode, and adds a second independent
failure that on its own would also be blocking (see §4 below).

## 1. What was measured

Design §3.4 canonical URI, generated on `gnome-dev`:

```
nostrconnect://6131623263336434d272e81bbb5f695fb82969979d33351fd6473a6865183bdf
              ?relay=wss%3A%2F%2Fbunker.sharegap.net
              &secret=0f102018372728280e2b1d375138c475
              &perms=sign_event%3A1&name=GNOME
```

- 189 bytes → QR version 8–9 at ECC L (within the design’s ≤230-byte budget).
- Encoder: `qrencode -t UTF8 -l L -m 2` (byte mode, quiet-zone 2 modules, half-blocks `▀ ▄ █`).
- Fallback URI (design §3.4 budget steps 4+5 — drop `perms`, drop `name`): 157 bytes.

QR emission channel: a scratch `pam_echo.so file=/etc/nostr-qr-spike-full.txt`
line inserted at the top of the real `gdm-password` auth stack, guarded by
`pam_succeed_if user = qrspike` so it only fires for the throwaway test user.
Full diff of the temporary edit:

```diff
+# --- QR SPIKE: emit QR block for qrspike only ---
+auth    [success=ignore default=1] pam_succeed_if.so user = qrspike quiet
+auth    optional        pam_echo.so file=/etc/nostr-qr-spike-full.txt
+# --- END QR SPIKE ---
```

`pamtester -v gdm-password qrspike authenticate` reproduced the exact 4240-byte
PAM_TEXT_INFO block: the 27-line half-block QR, the URI, and the pairing
line (`Pairing code: 6131-6232   *   expires in 90 s`). Note: pam_echo does its
own `%` expansion, so the file on disk stores every `%` as `%%` — the URI
reaches the conversation intact as `wss%3A%2F%2Fbunker.sharegap.net…`.

The greeter was driven headlessly from the hypervisor:

```
virsh send-key gnome-dev KEY_SPACE   # wake DPMS
virsh send-key gnome-dev KEY_TAB     # focus user list
virsh send-key gnome-dev KEY_DOWN    # to qrspike
virsh send-key gnome-dev KEY_ENTER   # activate → runs gdm-password
virsh screenshot gnome-dev …         # capture at prompt
```

## 2. Baseline sanity (encoding is correct)

`qrencode` PNG output of the full URI decodes cleanly with `zbarimg`:

```
$ zbarimg --raw -q /tmp/qr-spike/qr-baseline.png
nostrconnect://6131…&perms=sign_event%3A1&name=GNOME
```

So the QR itself is well-formed. What follows measures whether the *text*
form of that same QR survives the greeter’s label rendering.

Reference: [`/tmp/qr-spike/qr-baseline.png`](/tmp/qr-spike/qr-baseline.png).

## 3. In-greeter render — **unscannable**

Real greeter screenshot with the QR PAM_TEXT_INFO block emitted:
[`/tmp/qr-spike/greeter-qr-broken.png`](/tmp/qr-spike/greeter-qr-broken.png).

Visually the QR is destroyed:

- gnome-shell renders the label in **Cantarell (proportional)** at the theme
  size, not in a monospace face — module columns do not align.
- The label wraps at the prompt column (~23em ≈ 440 px at this DPI). The 57-
  character-wide half-block rows are folded to multiple visual lines, then the
  next module-row pair starts on a new visual line, so the 2-D module grid is
  scrambled beyond any error correction.
- The whole payload also exceeds the visible area — the URI, pairing code, and
  even the “Or open this link…” hint fall below the fold on 1280×800. There is
  no scrollback in the login dialog.

Decode attempts:

```
$ zbarimg --raw -q /tmp/qr-spike/greeter-qr-broken.png
(no output)
$ convert greeter-qr-broken.png -crop 500x500+390+340 crop.png
$ zbarimg --raw -q crop.png ; zbarimg --raw -q crop-2x.png
(no output)          (no output)
```

**Result: 0 successful decodes across the full frame, cropped ROI, and 2×
upscale.**

## 4. Independent failure: even a *perfect* text render doesn’t decode

Before touching the greeter, I built a Pillow harness that renders arbitrary
text to PNG with a chosen font, wrap width, and — critically — a
line-height set exactly to the height of the `█` (U+2588) full-block glyph
(so consecutive text lines share edges, no leading gap). I then ran
`zbarimg` on every variant.

| Font                | Size | Wrap | zbarimg |
|---------------------|-----:|-----:|---------|
| DejaVu Sans Mono    | 12   |  ∞   | FAIL    |
| DejaVu Sans Mono    | 16   |  ∞   | FAIL    |
| DejaVu Sans Mono    | 20   |  ∞   | FAIL    |
| DejaVu Sans Mono    | 24   |  ∞   | FAIL    |
| DejaVu Sans Mono    | 32   |  ∞   | FAIL    |
| Noto Sans Mono      | 16…32|  ∞   | FAIL (all sizes) |
| JetBrains Mono      | 16…32|  ∞   | FAIL (all sizes) |
| Cantarell           | 12…24|  ∞   | FAIL (all sizes) |
| Cantarell           | 12…20| 23em | FAIL (all sizes) |

For reference, [`/tmp/qr-spike/mono-nowrap-32.png`](/tmp/qr-spike/mono-nowrap-32.png)
is DejaVu Sans Mono at 32 px, no wrap, tight line spacing — visually a very
believable QR to the human eye, yet zbarimg still refuses it. The failure mode
is visible on close inspection: the `▀` / `▄` glyphs in every TrueType face
we tried do not fill the em-square vertically to the pixel — a thin residual
white band appears between each two-module row. That band is dark-to-light-
to-dark to a QR decoder’s adaptive threshold and breaks module grid detection.
Morphological repair (`-morphology Close Rectangle:1x3`, `1x5`) also failed.

Consequence: even if we could force gnome-shell to use a monospace font
and disable wrapping (we cannot — no markup, no style, no font control from
PAM_TEXT_INFO), the text QR *itself* is unreliable under a strict scanner.
Phones are more forgiving than zbarimg, but the design’s own gate says the
scanner-of-record for the spike is “a scan”: with zbarimg refusing every
variant we generated, we cannot in good conscience ship a v1 whose primary
promise (“scan this with your Nostr signer app”) depends on the user’s
particular phone being more permissive than the reference free scanner.

## 5. Fallback path — **legible, works**

Same PAM_TEXT_INFO channel, `pam_echo` pointed at
`/etc/nostr-qr-spike-fallback.txt` (URI + pairing code + hint, no QR block).

Screenshot: [`/tmp/qr-spike/greeter-fallback.png`](/tmp/qr-spike/greeter-fallback.png).

- Hint (“Open this link on your phone or paste into your Nostr signer:”) is
  legible.
- URI wraps across three lines but every character reaches the label and the
  full string is visually present. A user can read it, and a paired desktop
  signer can consume it from a text console or a photograph via OCR.
- `Pairing code: 6131-6232   *   expires in 90 s` renders cleanly on one line.
- The pairing code (design §3.4: first 8 hex chars of the ephemeral client
  pubkey, upper-cased, hyphenated) is fixed at 9 characters plus separator —
  no wrapping risk.

Cosmetic observations, filed for the B5-QR follow-up rather than blocking:

- The URI wraps mid-character rather than at `?` / `&` breakpoints, and the
  label is centered rather than left-aligned, so each wrap line has a
  half-glyph of left-side clipping visually. Content is intact but eye-parsing
  is harder than it needs to be. Consider inserting zero-width whitespace at
  `?`, `&`, `=` (`​`) so the wrapper prefers those points — need to
  verify the same signer/URI-consumer path accepts the string after we strip
  them.
- The fallback message fits on the 1280×800 screen with room to spare; the
  full-QR variant does not fit (§3).

## 6. Restoration and evidence of stock state

- `/etc/pam.d/gdm-password` restored byte-identical from
  `/etc/pam.d/gdm-password.spike-backup` (`diff -q` = no differences).
- `/etc/nostr-qr-spike-full.txt`, `/etc/nostr-qr-spike-fallback.txt`,
  `/etc/pam.d/gdm-password.spike-new` deleted.
- Test user `qrspike` deleted (`userdel -r qrspike`), no `id qrspike`.
- Post-restore `pamtester -v gdm-password debugger authenticate`: no
  `Nostr signer` text emitted; standard `Password:` prompt reached.
- Greeter left in idle state; not restarted. Post-restore screenshot:
  [`/tmp/qr-spike/greeter-restored.png`](/tmp/qr-spike/greeter-restored.png)
  — only the original `GNOME Debug Operator` user listed, no `qrspike`.

## 7. Screenshots on this workstation

All under `/tmp/qr-spike/` (nothing secret; ephemeral spike keys only):

- `greeter-idle.png` — DPMS-off frame before wake.
- `greeter-wake.png` — greeter user list after `KEY_SPACE`.
- `greeter-qr-broken.png` — **the acid test**: QR PAM_TEXT_INFO in the real
  gnome-shell login dialog. Cantarell + wrap-at-23em destroys the QR.
- `greeter-fallback.png` — URI + pairing code layout in the same slot; legible.
- `greeter-restored.png` — post-cleanup idle greeter.
- `qr-baseline.png` — `qrencode -t PNG` of the exact URI (zbarimg-OK).
- `mono-nowrap-32.png` — best-case off-greeter render: DejaVu Sans Mono,
  no wrap, tight line height. zbarimg still refuses.

## 8. Recommendation for v1 (design D7)

1. **Flip the default**: ship with `nip46_qr_render=uri`. Do not attempt the
   in-dialog QR at the graphical greeter; do not attempt a font/wrap override
   (we have none via PAM_TEXT_INFO).
2. Keep the QR generator wired up. On text consoles (`pamtester`, `tty1`,
   ssh) the greeter is a monospace terminal with no proportional wrapping,
   and the same 27-line half-block emission is directly scannable there.
   Cost of retaining it: zero — it is already generated once per session.
3. Fallback layout in the graphical greeter: hint line + URI + pairing code
   only, in that order. Nothing else fits above the fold at 1280×800 and
   nothing else is durable across theme/DPI changes.
4. Track the in-dialog QR revival as Phase 2 (design §3.5): broker writes
   `/run/nostr-auth/greeter/<tx-id>.png`, a small gnome-shell extension
   renders it. That path escapes both failure modes we hit here
   (proportional font *and* half-block-glyph gap) because it moves the QR
   out of a text label entirely.
5. Small UX polish for the URI/pairing layout, filed under B5-QR (§5): pick
   line-break hints (`​` at `?`/`&`/`=`) so a wrapped URI is easier for
   humans to eye-parse and OCR — pending confirmation that the signer-side
   consumer strips them cleanly. Non-blocking for the v1 default flip.

## 9. Beads follow-ups (proposed, to file after commit)

- `nostrc-z1fb`: update — record NO-GO, note that v1 default `nip46_qr_render`
  becomes `uri` (design D7), and demote the in-dialog QR work to a
  Phase-2 dependency behind an as-yet-unfiled gnome-shell-extension issue.
- New issue (Phase-2): implement `/run/nostr-auth/greeter/<tx-id>.png` broker
  drop + minimal gnome-shell extension per design §3.5. Depends on `nostrc-z1fb`
  Phase-1 lib work (`build_connect`, `client_await_connect`,
  `bunker_connect_to_client`) so the URI is already available to render.
- New issue (v1 UX polish): zero-width-space break hints in the fallback URI
  and `nostr-auth-ui.c` string-formatting equivalents; verify signer URI
  parser tolerates stripped ZWSPs.

