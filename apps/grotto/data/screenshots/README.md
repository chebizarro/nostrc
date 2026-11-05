# Screenshot provenance

Captured from **real Grotto and grotto-panels templates** by `signer/ui-layout`
for the Settings-style window and remaining setup/sheets (nostrc-u6v55.23.5).
The private-bus fixture supplies
disposable identities, a Jank client, permissions, activity metadata and a pending
request. These are fixture values, not live browser or relay acceptance. No
operator keys, recovery phrases or live pairing tokens are shown. The onboarding
backup capture uses the published “abandon … about” test vector, not an operator's
recovery material. Setup navigation and acknowledgement use production handlers;
the fixture answers storage and verification calls. These images are not proof
of cryptographic verification or real keyring storage.

Environment: Ubuntu 24.04 arm64 container, GTK 4.14.5, libadwaita 1.5.0,
GLib 2.80.0, Blueprint 0.22.2 and Xvfb. No patched GTK or external desktop session.

After building the test, reproduce with:

```sh
xvfb-run -a ctest --test-dir _build -R '^signer/ui-layout$' --output-on-failure
```

The test creates its private bus and writes `<build>/screenshots/`. Copy the
Identity, Connected Apps, inbox, Activity, Remote Signing, Client, Permissions
and Preferences light/dark PNGs here without image editing. Also retain the
`onboarding-*-360`, Backup, pairing and public-key QR fixtures. Setup fixtures use
dark mode; `-es` selects Spanish and `-rtl` reverses widget direction. They cover
every setup step, encrypted-import passphrase naming, disclosure defaults, and
the scanner's translated return action. The `360` captures
exercise the collapsed sidebar; Preferences keeps the `settings-*.png` filename.
Before each capture the test waits for production toasts to dismiss naturally;
it does not suppress feedback, dismiss toasts, or retouch pixels. Keyboard tests
return focus from transient toast controls to the panel before this wait, since
focused toasts correctly pause their timeout. RTL captures also cover Connected
Apps, Permissions, Activity and Remote Signing at 360 px.
Preferences are captured with discovery details closed by default; the separate
`settings-details-*` capture shows the expanded configuration. Opening Backup
does not reveal recovery material; raw QR text stays behind Details. During W5, original
panel and welcome captures from base `9d1273f70` were retained locally at
`_build/screenshots/before/`; current captures are at `_build/screenshots/`.
Only current captures are committed. Static intermediate setup-stage previews
from the before run are not used as workflow evidence.

Dynamic fixture timestamps can change bytes on regeneration. The separate
`approval/` captures retain their own provenance.

AppStream references Identity, inbox, Permissions and Preferences. Its master
URLs become available only after integration, not merely a branch push.
`signer/desktop-contract` checks the referenced local files and default image.

These captures are **not installed GNOME Shell/Orca or release evidence**.
