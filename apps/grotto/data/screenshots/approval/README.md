# Review dialog fixtures

These images use the production `GnostrApprovalDialog` and its compiled Blueprint
with synthetic public request data. They are **not** GNOME Shell screenshots,
live signing evidence, or owner acceptance on GNOME 46.

Capture environment: native aarch64 Ubuntu 24.04 container (`nostrc-linux-ci:arm64`),
GTK 4.14.5, libadwaita 1.5.0, private D-Bus session and Xvfb. Animations are disabled
by the test. No profile, relay or image is fetched. The identity and app are fixtures.

`approval-360-*` and `approval-420-*` name the requested host-window widths; the
captured client surfaces are respectively 350×690 and 410×690 on this backend.
The `-scope` companions scroll the same dialog to the full permission and scope
rows. Both views retain the bottom commit bar. Light and dark are theme-provided.

## Reproduce

Build the existing test target, then run on Linux:

```sh
cmake --build <build> --target test-approval-dialog
GROTTO_APPROVAL_SCREENSHOT_DIR="$PWD/approval-captures" \
  dbus-run-session -- xvfb-run -a \
  <build>/apps/grotto/tests/test-approval-dialog \
  -p /signer/approval-dialog/snapshots
```

For a visual comparison on the owner's GNOME 46 desktop, run that test against
the session display without `xvfb-run`, with the same screenshot environment
variable. This remains a fixture capture. Review an actual pending request in
Grotto separately to accept the live workflow: once/persistent contact-list
consent, full relay scope for relay sign-in, Not Now, and the still-pending
request after Discard Permission.
