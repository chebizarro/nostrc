# Grotto Flatpak — experimental, release blocked

These manifests are research inputs, not a supported signer distribution. A build
or sandbox launch does not demonstrate safe caller identification or key custody.
Use the native `grotto` package; a Groundhog Flatpak may talk to a native signer.
Do not use real identities in sandbox smoke tests or publish these on Flathub yet.

## Mandatory release gates (§5.11)

All five need source-grounded and installed-system negative/positive evidence:

1. **Host caller attestation**: identify the real host caller across the sandbox
   boundary without trusting caller-supplied names; preserve origin isolation.
2. **Trusted approver identification**: prove only the installed trusted GUI can
   approve, export or resume; do not relax absolute-path/device/inode checks.
3. **Secret-portal custody**: prove durable, isolated custody, unlock cancellation,
   migration, export and transport-key non-enumeration using the intended portal.
   `--talk-name=org.freedesktop.secrets` alone is not this proof.
4. **Activation ownership**: prove one session daemon and one approver, host ↔
   sandbox activation, restart and version-skew behavior without name collisions.
5. **Browser-host behavior**: prove native messaging host activation, actual
   executable attestation and independent browser-origin authority end to end.

No gate has installed acceptance attached in G12. See `nostrc-mevv`, `nostrc-9z5w`,
the [packaging plan](../../../../docs/designs/groundhog-packaging-plan-2026-10-05.md)
and [manual matrix](../../../../docs/testing/grotto-manual-acceptance.md).
AppImage additionally has a changing mount path; `nostrc-h3kh` remains deferred.
Snap is only on owner demand. An experimental build is not permission to bypass
any identity/custody check, add a fallback signer, or claim publication success.
