# Review dialog fixtures

These images use the production `GnostrApprovalDialog` and its compiled Blueprint
with synthetic public request data. They are **not** GNOME Shell screenshots,
live signing evidence, or owner acceptance on GNOME 46.

Capture environment: native aarch64 Ubuntu 24.04 container (`nostrc-linux-ci:arm64`),
GTK 4.14.5, libadwaita 1.5.0, private D-Bus session and Xvfb. Animations are disabled
by the test. No profile, relay or image is fetched. Alice is a local fixture label;
its full-length public npub encodes the standard secp256k1 generator point.

`approval-360-*` and `approval-420-*` name the requested host-window widths; the
captured client surfaces are respectively 350×690 and 410×690 on this backend.
The `-scope` companions scroll the request summary to its bottom. Scope itself
is now fixed with the commit buttons and stays visible in **both** views. This
follows the design's explicit visibility requirement over its scrolling sketch.
Light and dark are theme-provided.

The capture test invalidates the complete scene after scrolling and snapshots
the dialog at its paintable's intrinsic size, avoiding host-window shadow/scaling
coordinates. It checks scope and both actions are within the viewport, and compares
each rendered button's interior pixel to a fresh same-theme button composited over
the footer background (normal buttons have a translucent fill). A mapped widget
alone is not capture proof. Every saved PNG is logged with its SHA-256. The retained capture
files and the committed files must have identical hashes; do not substitute an
older `w1-screens` directory or a previous run's receipt.

## Reproduce

Build the existing test target, then run on Linux:

```sh
cmake --build <build> --target test-approval-dialog
GTK_A11Y=test GROTTO_APPROVAL_SCREENSHOT_DIR="$PWD/approval-captures" \
  dbus-run-session -- xvfb-run -a \
  <build>/apps/grotto/tests/test-approval-dialog \
  -p /signer/approval-dialog/snapshots
sha256sum approval-captures/*.png
```

For a visual comparison on the owner's GNOME 46 desktop, run that test against
the session display without `xvfb-run`, with the same screenshot environment
variable. This remains a fixture capture. Review an actual pending request in
Grotto separately to accept the live workflow: once/persistent contact-list
consent, full relay scope for relay sign-in, Not Now, and the still-pending
request after Discard Permission.
