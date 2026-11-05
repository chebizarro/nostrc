# W5 dialogs/onboarding: UX, behavior and boundary review

Date: 2026-11-04. Review task: `nostrc-9rh4t`; implementation: `nostrc-u6v55.23.5`.

## Boundary and verdict

Reviewed exactly `git diff 3e38c0137...d40759b5b` on `grotto/w5-dialogs-onboarding`:

- Base: `3e38c0137`; reviewed HEAD: `d40759b5bab2b4df1718ff427ae220e3f85fd42b`.
- Implementation: `4c8a510f5`, followed by the master merge `d40759b5b`.
- Worktree throughout: `/Users/bizarro/Documents/Projects/.repoprompt-worktrees/nostrc/rp-agent-d6d74995-grotto-w5-dialogs-onboarding-942ac605`.
- Scope: changed dialogs/onboarding, Backup, Preferences, notifications, identity/app details, Remote Signing, translations, accessibility, templates and screenshots, against design §a.5/§d.6 and the owner's current policy. No implementation, tests, catalogs, screenshots or version sources were changed by this review.

**Verdict:** the decluttering largely follows the requested direction, and the requested focused tests pass with the container's missing keyboard-test dependency supplied. It is **not merge-ready unchanged**: two downstream approval tests are newly inconsistent with W5 copy, the backup acknowledgement loses its accessible name, and first-run Connect does not honor its new destination label. No new always-confirm class, per-kind re-prompt, mandatory import-backup step or secret/policy transfer into the panels library was found.

Evidence labels: **confirmed/executed** means run during this review; **confirmed/static** means established from source and its production caller, not a live click-through; **confirmed/visual** means inspected committed PNGs. S2 denotes usability/verification defects and S3 localization polish, not exploit severity. References are repository-relative at the reviewed HEAD. Container/Xvfb evidence is not native desktop, hosted CI, owner acceptance or release proof.

## Findings, ranked

### F1 — S2, confirmed/executed — Blocker: W5 copy breaks two directly affected approval tests

**Where:** `apps/grotto/src/approval-controller.c:179-180,233`; unchanged expectations at `apps/grotto/tests/test-approval-controller.c:550-561` and `apps/grotto/tests/test-approval-integration.c:354`.

W5 changes the self-test operation label from `Verify stored identity` to `Check identity`, and the notification action from `Deny once` to `Deny Once`, without updating these consumers. After explicitly rebuilding their targets, `signer/approval-controller` aborts at line 561 because its notification action search never finds the old label. `signer/approval-integration` aborts at line 354: actual `Grotto · Check identity`, expected `Grotto · Verify stored identity`. Its management-notification wait also still expects the old wording (`test-approval-controller.c:550-551`). These are direct consequences of the changed strings, not the accepted profile-smoke baseline.

**Failure scenario:** running the affected approval checks on this branch fails even though the six checks listed in the implementation handoff pass. The self-test integration case cannot reach its later inbox-reopen/approve/completion assertions. Keep the security/behavior assertions; reconcile their expected copy and rerun these two checks. This is a test-contract regression, not evidence that Deny dispatch or authorization changed. No test was fixed or bypassed in this review.

### F2 — S2, confirmed/executed and static — Should-fix: the backup acknowledgement checkbox has no accessible name

**Where:** `apps/grotto/data/ui/onboarding-assistant.blp:108-110`; completion depends on the checkbox at `apps/grotto/src/ui/onboarding-assistant.c:336-340`. The new accessibility coverage at `apps/grotto/tests/test-ui-layout.c:927-931,941` checks the password entry but merely toggles this checkbox.

The previous `CheckButton.label` is replaced by a custom wrapping child `Label`, without an accessibility label or labelled-by relation. GTK explicitly makes the caller responsible for naming a checkbox with a custom child ([GTK API](https://docs.gtk.org/gtk4/method.CheckButton.set_child.html)); descriptive names are required by the [GNOME HIG](https://developer.gnome.org/hig/guidelines/accessibility.html#accessible-names).

**Failure scenario:** a screen-reader user reaches the control that enables Confirm Backup but cannot identify what is being acknowledged from the checkbox's accessible name. An isolated diagnostic copy of the onboarding fixture, linked to the unchanged production objects/resources under GTK 4.14.5 with `GTK_A11Y=test`, reached the actual Backup state and reported `label=(null)`, `child=GtkLabel`, `accessible-label-present=0`, `labelled-by-present=0`. An assertion for the displayed acknowledgement text failed with value `undefined` (exit 134). This confirms the missing metadata; an Orca session was not run. Associate the displayed text with the checkbox without adding any new confirmation step or making backup mandatory for imports.

### F3 — S2, confirmed/static — Should-fix: first-run “Open Connected Apps” instead opens Identity

**Where:** destination label at `apps/grotto/src/ui/onboarding-assistant.c:89`; navigation at `:331-334`; first-run caller at `apps/grotto/src/main_app.c:630-635`; completion callback at `:129-144`; default panel at `apps/grotto/src/ui/signer-window.c:619-622`.

The new Connect action navigates only an existing transient `SignerWindow`, then calls `finish()`. In the production first-run path, onboarding is an application window without a transient parent. Its completion callback creates the management window only afterward, and that window defaults to Identity. Unlike the successful-import branch of `finish()` (`onboarding-assistant.c:131-140`), Connect does not locate the newly created window and navigate it.

**Failure scenario:** first launch with no pending requests → Get Started → Connect an App → Open Connected Apps closes setup and presents Identity, not the advertised pairing destination. The existing-parent Add Identity path works; a pending request can also independently select Apps, masking this case. The new layout test captures Connect but leaves through `connect_import` rather than activating its footer (`apps/grotto/tests/test-ui-layout.c:922-923`), so it does not cover this handoff. Preserve the user's destination through first-run completion. This finding is a source-confirmed caller trace, not a live first-run reproduction.

### F4 — S3, confirmed/static — Nit: one new Backup action still falls back to English in Spanish

**Where:** `apps/grotto/src/ui/page-identity-detail.c:327`; `apps/grotto/po/es.po:2983-2984`.

The new encrypted-backup commit label is correctly marked `_("Show QR Code")`, but its Spanish translation is empty. Opening Backup → Show QR Code and reaching the passphrase dialog therefore leaves this action in English. The panels catalog has a translation for the same words, but it is a different gettext domain and does not fill the Grotto catalog. This is residual localization work, not a missing extraction marker or an authorization defect. Other incomplete catalog entries, including Preferences, predate W5 and are not reclassified as new regressions here.

## Owner-policy and regression checks

- **G18 / approve once:** the diff does not change `approval_dialog.c`, `approval-dialog.blp`, or the daemon permission implementation. The changed approval-controller code is display/translation code, not decision dispatch. App access controls retain the existing policies and explicitly say saved permissions survive lowered access; capitalization does not create an always-confirm class. `signer/permission-repeat` passed. Nothing in this review recommends adding per-kind prompts or extra approval steps.
- **Import Done/Skip, fix `150b89d44`:** imported identities still become stored before self-test, successful import goes directly to Ready rather than mandatory Backup (`onboarding-assistant.c:227-252`), and Skip calls `finish()` without deleting the stored identity (`:354-357`). The executed onboarding suite retains Done/Skip/close persistence and imported-profile destination cases (`test-onboarding.c:768-770,778`). All passed.
- **Self-test review, fix `17f67593e`:** production still attaches the pending group to onboarding and routes `app.review-request` immediately (`main_app.c:202-219,265-275,633-635`). Importantly, the failing integration run already passed the assertion that an approval dialog opened on the onboarding window, checked its Grotto identity and non-reusable permission UI, and closed it with Not Now (`test-approval-integration.c:334-345`). The later inbox reactivation/approval/completion assertions were **not reached** because of F1; this is not a full integration pass or evidence that the opening regression returned.
- **Panel boundary:** changes in `gnome/grotto-panels` are presentation/metadata rendering and an unavailable-banner reset. They add no secret import/export/QR payload API, credential handling or policy evaluator; service/host dispatch remains through `GrottoPanelsService` and existing host callbacks. Both requested no-secrets/Control1 checks passed. Trusted import, backup and pairing-token presentation remain Grotto-owned.
- **HIG/layout:** onboarding now uses per-page navigation headers and direct choice rows; actions are purposeful. Technical QR/pairing text, Backup advanced actions and Remote Signing methods/reset use collapsed disclosures. The QR sheet uses neutral Close, not suggested Done. Visible host wording remains Preferences; `settings-*` filenames/internal routes are not user-facing renames. F2/F3 are the identified accessibility/navigation exceptions, not reasons to restore dense lists.
- **i18n/RTL:** added task strings are gettext-marked, QR sources are added to `POTFILES.in`, and request counts use `ngettext`. `msgfmt --check --check-format` passed for all five Grotto catalogs and the panels Spanish catalog. Spanish/RTL layout and password-entry-label assertions passed; F4 and inherited untranslated entries mean this is not a claim of complete Spanish coverage or a Persian translation pass.
- **Blueprint/UI drift:** the pinned 0.22.2 drift script passed. Its implementation checks both Grotto and grotto-panels resources against committed `.ui`, as well as resource/build inventories, catalog consistency and the generated kind catalog.
- **Screenshots:** directly inspected committed Spanish/RTL onboarding Import, Connect, Check and Backup; Spanish Backup, pairing preview and public-key QR; and English Remote Signing, app details and Preferences. They show shortened copy and collapsed disclosures, with reachable footer actions on onboarding. The Check capture has no actual pending review row and is not proof of self-test review behavior; that comes from the executed integration prefix above. Fixture keys/tokens and the published recovery vector are not operator secrets. README provenance correctly calls these container/private-bus fixtures, not live-relay/keyring acceptance.

## Verification actually run

Linux image: `nostrc-linux-ci:arm64`, image ID `sha256:3d1c8f2383c7159a44e3c8f1714476ca749333d34c887b780d6efc144eab35e2`; existing `_build` mounted at `/src`. The focused targets were explicitly rebuilt; production tracked files were unchanged. `xdotool` and its dependency were installed only in the disposable container, not on the developer host. Test homes/private buses remained isolated. No developer keychain commands were run.

| Command/check | Actual result |
|---|---|
| `python3 apps/grotto/tests/test-blueprint-drift.py apps/grotto` | PASS; pinned Blueprint 0.22.2, including panels |
| Requested `xvfb-run -a ctest --test-dir _build -R "^signer/(onboarding\|identity-management\|qr-scanner\|ui-layout\|ui-accessibility\|blueprint-drift)$" --output-on-failure` | Initial run: onboarding, identity, QR, drift and home fixture passed; layout/a11y aborted because the image lacked the compiled-in `/usr/bin/xdotool` executable |
| Rebuilt, equipped-container run with the same regex extended by `approval-controller\|approval-integration\|permission-repeat` | All six requested signer tests PASS; permission-repeat PASS; home fixture PASS; approval-controller and approval-integration FAIL as F1. Overall CTest exit 8, **not a green run**; 8/10 including the fixture |
| `ctest --test-dir _build -R "^grotto-panels/(no-secrets\|control1-service)$" --output-on-failure` | PASS, 2/2, on both runs |
| `msgfmt --check --check-format -o /dev/null <catalog>` for every shipped Grotto/panels `.po` | PASS |
| Added isolated backup-name diagnostic in ignored `_build/w5-review`, against production template/objects | Expected assertion FAIL, exit 134; confirms F2. Not a committed test or a product fix |
| `scripts/test-gate.sh --base 3e38c0137 --head d40759b5b --explain` | PASS, **plan only**: 20 checks, 9 Linux lanes; includes `grotto.panels -> gnome.nostr_settings` compatibility. Receipt `594682a567a64ae6983876f51080d7ea`; no claim those lanes executed |

Local receipts: `_build/w5-review/{blueprint-drift.log,signer.log,signer-equipped.log,panels.log,panels-equipped.log,build.log,container-deps.log,container-deps-root.log,backup-name-probe.log,affected-explain.log}`. The initial non-root dependency-install attempt failed on apt directory permissions; the subsequent root-container installation succeeded. Earlier failures are retained, not replaced by later results. Containers were disposable and removed; the pre-existing build tree was preserved.

The full repository suite, full affected-lane execution, sanitizers, native desktop/Orca, live relays/keyring and owner acceptance were not run. `grotto-panels/smoke-host` was deliberately not run or counted as a W5 failure: its Ubuntu 24.04 arm64 profile crash is the owner's accepted pre-existing baseline (`nostrc-qdgge` / `nostrc-u6v55.32`). No guard, suppression or timeout was weakened.

## Version and delivery decision

This report is documentation-only: **no bump** for Grotto or grotto-panels; no other independently releasable component is changed by the review. W5's presentation/i18n corrections are PATCH-class within the owner's existing unreleased Grotto **0.1.1** / grotto-panels **0.1.0** wave; the review adds no API/ABI, storage/wire or authority change. `VERSION_MANIFEST.md:34-35` still declares those unreleased versions. No source version, release column or tag was edited.

## Merge recommendation

**Request changes before merging W5 unchanged.** Keep the decluttered layout and approve-once semantics, but reconcile and rerun the two broken approval tests (F1), name the existing backup checkbox accessibly (F2), and honor Connected Apps after first-run completion (F3); translate the new Backup action as a small follow-up (F4). The six requested focused checks and both boundary checks pass, imported identities survive Done/Skip, and the self-test review opens, so there is no basis here for a new permission policy, extra confirmation ceremony or repository-wide qualification exercise. After these finite corrections, rerun their directly affected checks and hand the result back for the owner's normal acceptance; the accepted profile-smoke baseline remains separate.
