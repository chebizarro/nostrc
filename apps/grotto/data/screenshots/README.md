# Screenshot provenance

Captured 2026-10-14 from **real Grotto templates** by `signer/ui-layout`, not a
mock window or generated illustration. Base integration: `a28b31869` plus G14
Preferences terminology and header capitalisation (nostrc-u6v55.16). The private-bus fixture supplies disposable identities,
Jank client, trust/permissions and Signed today=117. These are **fixture values**,
not claims of real account history or live browser/relay acceptance. No private
keys, seed words or live pairing tokens are shown.

Environment: stock Ubuntu 24.04 arm64 container, GTK 4.14.5
(`4.14.5+ds-0ubuntu0.10`), libadwaita 1.5.0 (`1.5.0-1ubuntu2`), GLib 2.80.0,
Blueprint 0.22.2, Xvfb 1280x800. No patched GTK or external desktop session.

Reproduce from the monorepo after a testing build:

```sh
dbus-run-session -- xvfb-run -a -s '-screen 0 1280x800x24' \
  ctest --test-dir _build -R '^signer/ui-layout$' --output-on-failure
```

The test writes `_build/screenshots/` (or `<build>/screenshots/` for another build
folder). Copy the light/dark Home, 360px Home, Client, Permissions, Connect and
Preferences PNGs (the internal `settings-*.png` filenames are unchanged) here without image editing. Dynamic fixture timestamps can change
bytes on regeneration. AppStream references `home-light.png`,
`permissions-light.png`, `settings-dark.png` from this directory on master;
those remote URLs become available only after landing, not merely a branch push.
`signer/desktop-contract` checks that every referenced local image exists.

These captures are **not installed Ubuntu/Shell/Orca release evidence**. Manual
light/dark onboarding, compact approval, identities/backup and degraded-bunker
captures remain NOT RUN in `docs/testing/grotto-manual-acceptance.md` (.14.1).
