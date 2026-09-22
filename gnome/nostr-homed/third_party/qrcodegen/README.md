# qrcodegen (Nayuki, MIT) — vendored

Vendored per design `docs/designs/nip46-qr-login-greeter.md` §6 / decision **D2**.
Linked ONLY into `pam_nostr.so` and the QR encoder unit tests; the root broker
does not link this code.

- Upstream: https://github.com/nayuki/QR-Code-generator (directory `c/`)
- Pinned commit: see `VERSION`
- License: MIT (see `LICENSE.qrcodegen`)
- Modifications: none

To refresh the vendored copy, replace `qrcodegen.[hc]` verbatim from the pinned
upstream commit and bump `VERSION`. Do not edit qrcodegen.[hc] in-tree; keep the
copy verbatim so upstream diffs stay trivial.
