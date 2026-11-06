# Profile cache review: implementation follow-up

This is the implementation response to the independent
[request-changes review](grotto-profile-cache-review-2026-11-06.md), not a replacement
for its failed receipt or an independent approval.

## F1: missing images with fresh metadata

Fixed in `grotto-profile-service.c`: an ordinary FETCH still returns cached profile
and image data first. If permitted images are missing, it schedules a single-flight
background image-only job even when metadata is fresh. This job does not contact
relays, perform discovery/NIP-05 work, or renew the metadata freshness stamp. A
failed download may be retried on a subsequent FETCH, without an automatic retry
loop. FETCH_CACHED remains strictly network-free. Images-off and Tor mode prohibit
downloads; existing HTTPS, redirect, size and raster validation remain in force.

Metadata freshness is unchanged: 900 seconds since a successful EOSE fetch;
missing, invalid or future freshness stamps are stale. Stale metadata may refresh
in the background. Newer-event selection and bootstrap REQ-only semantics remain
unchanged.

A diff-scoped Oracle review also identified a same-revision completion race:
a delayed cache snapshot with missing pixels could erase successful hydration.
The panels service now retains loaded textures only for the same timestamp,
publication state and unchanged image URL. Both view completion paths render the
service's winning snapshot. A changed URL does not inherit the previous texture.
This changes no overlap/About layout code or public ABI.

Regressions exercise the production GTK view and real TLS image endpoint:

- Re-enable Load Profile Images while metadata is fresh: cache-first display,
  live avatar/banner hydration, no relay requests, and unchanged freshness stamp.
- Retry previous HTTP failures under the same fresh-metadata conditions.
- No downloads while disabled, in Tor mode, or for FETCH_CACHED.
- Reuse decoded texture pointers after hydration.
- Hold equal-revision cache-only and FETCH replies until after hydration; both
  images survive, while a different URL correctly returns to fallback.

## Accessibility teardown: cause remains unconfirmed

The original ordinary run failed after all three GTest cases completed, with
`Error while sending AddMatch() message: The connection is closed`.
The original receipt remains at
`/tmp/grotto-profile-review-byhwg/focused.log`; a copy is retained below.

Using the same arm64 container environment and ordinary CTest entrypoint:

| Source | Result |
| --- | --- |
| Unmodified reviewed branch `743aaf903` (code from `a9ebcdbcb`) | accessibility passed; 70.96 s including fixture |
| Pre-cache master `96e96a4cd` | accessibility passed; 70.38 s including fixture |
| Final fix after merging master `561a52c97` | all seven requested checks passed serially; 170.66 s including fixture |

A separate fresh-home, pre-cache first-run case under GDB also exited normally.
That is diagnostic evidence, not a substitute for the ordinary failed receipt.
The private-bus teardown exists before this branch, but that fact and these
passing runs do **not** prove a pre-existing cause or rule out a branch defect.

[GLib's AddMatch implementation](https://github.com/GNOME/glib/blob/2.80.0/gio/gdbusconnection.c#L3135-L3159)
identifies the failed bus-match request, not the original caller or responsible
object lifetime. The failed run supplied no caller stack/core; no trap recurred
in the scoped reproductions. There is no evidence justifying a texture-finalizer
or weak-reference fix, nor enough evidence to label the failure unrelated.
`nostrc-rmea8` tracks the unresolved attribution. Fatal diagnostics remain enabled;
no suppression, timing workaround or teardown change was introduced.

## Verification and retained evidence

`nostrc-linux-ci:arm64`, image
`sha256:3d1c8f2383c7159a44e3c8f1714476ca749333d34c887b780d6efc144eab35e2`:

- Serial `ctest -j1`: signer/profile-integration, signer/ui-layout,
  signer/ui-accessibility, grotto-panels/profile-panel, management-panels,
  no-secrets and control1-service all passed; required test-home fixture also
  passed (8/8). Final source hashes match the tested container copy.
- Blueprint 0.22.2 drift check passed. Both catalogs regenerated with xgettext,
  no-fuzzy msgmerge, no-obsolete msgattrib and msgfmt --check; no fuzzy entries or
  merge markers. Upstream translations retained during catalog merge resolution.
- Receipts, source hashes and image identity:
  `/tmp/grotto-profile-cache-reviewfix-evidence.ol8NFP`. Initial failures are
  retained: the original teardown failure and an integration counter assertion
  that needed to observe the newly permitted image retry before measuring later
  image-off/Save no-contact boundaries. Those privacy assertions remain intact.

This is local container proof, not native-desktop, hosted CI or release acceptance.
No full suite, smoke-host (`nostrc-qdgge`) or VM deployment was run. Versions remain
panels 0.1.0 / Grotto 0.1.1, folding these fixes into the unreleased components as
directed by the owner.
