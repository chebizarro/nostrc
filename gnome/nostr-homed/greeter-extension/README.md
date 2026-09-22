# gnome-shell greeter-extension contract for the nostr NIP-46 QR login

Status: **draft, producer side (broker) shipped**. See design
[`../../docs/designs/nip46-qr-login-greeter.md`](../../docs/designs/nip46-qr-login-greeter.md) §3.5.

The nostr-authd broker publishes a small drop into
`/run/nostr-auth/greeter/` while a QR-login transaction is live. A gnome-shell
greeter extension consumes it to render the real QR image inside the login
dialog (the in-dialog half-block QR was ruled NO-GO by the Phase-0 spike —
see `docs/reviews/qr-greeter-render-spike-2026-09-22.md`).

This file is the **producer contract**. The extension itself is being built in
a separate branch and is not shipped here.

## Files

Both files live under `/run/nostr-auth/greeter/` (directory mode `0755`, owner
`root:root`, individual files `0644`).

```
current.json     # metadata (see below); overwritten atomically
current.png      # the QR image; monochrome greyscale PNG
```

Both files exist ONLY while a QR transaction is waiting for a signer. As soon
as the transaction retires (success, denial, expiry, cancel) the broker
unlinks them. An extension MUST tolerate either file appearing/disappearing
between polls; missing files mean "no active QR — clear any live pane".

The broker writes atomically (write to `<name>.tmp`, then `rename(2)`); the
extension may safely watch for `IN_MOVED_TO` on the directory.

## `current.json` schema

```json
{
  "tx_id":        "<uuid — broker transaction id, opaque to the extension>",
  "png":          "current.png",
  "uri":          "nostrconnect://<pk>?relay=…&secret=…&perms=…",
  "pairing_code": "A1B2-C3D4",
  "hint":         "Scan this with your Nostr signer app",
  "expires_at":   78000
}
```

- `tx_id` — the broker's transaction id. Purely for correlation in logs; the
  extension has no policy to enforce with it.
- `png` — the basename (never a path) of the sibling PNG in the same
  directory. Today it is always the literal string `current.png`; treat it
  as advisory so a future refresh cadence can add rotation.
- `uri` — the full `nostrconnect://` URI that produced the PNG. The extension
  MUST NOT log it; it embeds the pairing secret. It is safe to show on screen
  because the greeter is already visible (design §8.5) — the extension may
  render it as a copy-to-phone fallback if the PNG cannot be shown.
- `pairing_code` — a 9-character human-readable cross-check (`XXXX-XXXX`,
  derived from the first 8 hex chars of the ephemeral client pubkey per
  design §3.4). Loggable. Show it prominently so the user can confirm the
  signer app is talking about the same session.
- `hint` — a short one-line prompt. UTF-8, no markup. Render as-is.
- `expires_at` — the scan window in milliseconds. `0` means "unset".

## `current.png` format

- **Colour type**: greyscale (PNG colour type `0`), 8-bit.
- **Dimensions**: `(size + 4) * scale × (size + 4) * scale` pixels, where
  `size` is the QR version's module count and `4` is the quiet zone (2 modules
  on each side). Today the PAM module renders at scale=8 to a max version of
  9 (57×57 modules → 456×456 px).
- **Foreground**: dark modules are `0x00`, light modules are `0xff`. There
  are no partially-shaded pixels; nearest-neighbour scaling is faithful.
- The PNG uses only IHDR / IDAT / IEND chunks and stored-block deflate; the
  extension should decode it with any standard PNG library.

## Lifecycle notes

- The broker writes the drop between the `SELECT_PROVIDER` reply and the
  blocking `SUBMIT_UNLOCK`. It replaces the drop for every retry (the QR is
  minted fresh each attempt — never re-display a consumed QR, design §4.2).
- On tx retire the broker unlinks both files. If the broker crashes while a
  tx is live the drop may be stale; the extension SHOULD refresh at least
  once per second so a stale drop is visible for at most ~1 s. The `tx_id`
  changing between polls is the extension's signal to redraw.
- The producer never touches `current.json.tmp` / `current.png.tmp` once the
  rename completes; the extension can safely ignore `.tmp` files.
- The broker never writes into any file other than the two above. If the
  directory contains other files (e.g. earlier per-tx PNGs from a prior
  design revision) the extension should ignore them.

## Test seams

The broker uses the compiled-in default `/run/nostr-auth/greeter` for the
drop. Tests override the directory via `nh_broker_greeter_artifact_set_dir()`
so a headless test can point at a temp dir instead. Production must not use
that seam.
