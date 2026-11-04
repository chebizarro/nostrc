# Screenshot provenance

Captured from **real Grotto and grotto-panels templates** by `signer/ui-layout`
for the Settings-style window (nostrc-u6v55.23.4). The private-bus fixture supplies
disposable identities, a Jank client, permissions, activity metadata and a pending
request. These are fixture values, not live browser or relay acceptance. No
private keys, recovery words or live pairing tokens are shown.

Environment: Ubuntu 24.04 arm64 container, GTK 4.14.5, libadwaita 1.5.0,
GLib 2.80.0, Blueprint 0.22.2 and Xvfb. No patched GTK or external desktop session.

After building the test, reproduce with:

```sh
xvfb-run -a ctest --test-dir _build -R '^signer/ui-layout$' --output-on-failure
```

The test creates its private bus and writes `<build>/screenshots/`. Copy the
Identity, Connected Apps, inbox, Activity, Remote Signing, Client, Permissions
and Preferences light/dark PNGs here without image editing. The `360` captures
exercise the collapsed sidebar; Preferences keeps the `settings-*.png` filename.
Dynamic fixture timestamps can change bytes on regeneration. The separate
`approval/` captures retain their own provenance.

AppStream references Identity, inbox, Permissions and Preferences. Its master
URLs become available only after integration, not merely a branch push.
`signer/desktop-contract` checks the referenced local files and default image.

These captures are **not installed GNOME Shell/Orca or release evidence**.
