# Dialog sizing sweep: UX, coverage and regression-test review

Date: 2026-11-07. Review task: `nostrc-t76sy`; implementation: `nostrc-u6v55.69`.

## Boundary and verdict

Reviewed exactly `git diff 5086a5270...f04538e9c` on `grotto/dialog-sizing-sweep`:

- Base: `5086a5270d3a5e200979077eab85cf7c078b2650`; reviewed HEAD: `f04538e9c98b87f5ae211cdcfeff55583af8e4fe`.
- Worktree throughout: `/Users/bizarro/Documents/Projects/.repoprompt-worktrees/nostrc/rp-agent-565dfe07-grotto-dialog-sizing-sweep-ef8fc9da`.
- Scope: Connection Link/QR, Recovery, Retention and narrow-width label corrections; sweep completeness, allowlist and deliberate-regression sensitivity; conventions, consent, versions, catalogs and Blueprint drift.
- No production source, committed tests, resources, catalogs or versions were changed. The negative control modified one source copy and relinked a cloned build inside the disposable container, never this worktree or the original build.

**Verdict:** the shipped fixes work in the tested fixtures, and all eight requested checks pass. **Request changes before merging the sweep unchanged:** reverting the Recovery collapse fix still produces a green executed sweep, and complete coverage is not established. This is not evidence of a new authorization defect or a reason to undo the UI fixes.

Evidence labels: **confirmed/executed** means run during this review; **confirmed/static** means established from source/caller inventory. S2 denotes material usability/regression-verification defects; S3 denotes a lower-risk guard gap. References are repository-relative at the reviewed HEAD. ARM64 container/private-bus/Xvfb evidence is not native desktop, hosted CI, live-service, owner-acceptance or release proof.

## Findings, ranked

### F1 — S2, confirmed/executed — Blocker: the executed sweep accepts the deliberately restored Recovery collapse

**Where:** `apps/grotto/tests/test-dialog-sizing.c:402-415`, especially `:408-412`; production fix at `apps/grotto/src/ui/page-identity-detail.c:309`.

The test treats a positive `adw_dialog_get_content_height()` after presentation as an intentional ceiling whenever `follows-content-size` is false. That getter is not independent evidence of an intentional viewport: in the unfixed Recovery path it reports the initial, collapsed height. The expected height therefore shrinks to the defect instead of remaining `min(natural, host available)`. The [content-height documentation](https://gnome.pages.gitlab.gnome.org/libadwaita/doc/1.5/property.Dialog.content-height.html) describes content height, not a maximum-height constraint; [content following](https://gnome.pages.gitlab.gnome.org/libadwaita/doc/1.5/property.Dialog.follows-content-size.html) tracks changing natural content.

**Failure scenario actually executed:** copy `/build` to `/scratch-build`; copy production `page-identity-detail.c`; remove only `adw_dialog_set_follows_content_size(dialog, TRUE);`; compile that copy with the original target command and original-source quote-include directory; rebuild the cloned core archive and relink the unchanged test objects. Run `signer/dialog-sizing` against the clone with fresh diagnostic homes. The real Recovery opener still waits for RPC-populated words (`test-dialog-sizing.c:159-167`). Results:

```text
recovery-phrase at 920x640: content=400x251 natural=371 available=251 required=251
recovery-phrase at 360x640: content=360x360 natural=371 available=251 required=251
100% tests passed, 0 tests failed out of 2
```

The test's host availability is actually 580/610 px, not 251 px. At 920 px the restored sheet is 120 px shorter than its natural content, but exceeds the 200 px floor and passes. All **62 presentations completed**. This directly fails the requested executed negative-control criterion.

The static checker **does** reject the same mutated source for missing PreferencesPage content-following (exit 1). This is an independent runtime-oracle defect, not a claim that both guards miss this one-line reversion. Distinguish deliberately specified viewports from post-presentation dimensions in test expectations and retain this negative control. No test was fixed or weakened in this review.

### F2 — S2, confirmed/static and executed parser probe — Should-fix: a custom production dialog is absent, while the alert exception hides its subtree

**Where:** runtime registry `apps/grotto/tests/test-dialog-sizing.c:304-336` and registry-only loop `:457-460`; blanket markup exception `apps/grotto/tests/test-dialog-sizing.py:28-32,112-113` and C constructor selection `:195-203`. Omitted production surface: `apps/grotto/src/grotto-profile-service.c:914-924,982-1004`.

The 31-row table is genuinely data-driven, but not exhaustive. `grotto_profile_upload_confirmation()` creates **Crop and Upload Profile Image**; production `preview_ready()` installs a custom 180 px preview, guidance and three labelled crop sliders. No table row opens this surface. The static guard skips all `AdwAlertDialog` markup descendants and does not inspect alert factories in C. The custom content is application-owned, not merely toolkit message/response layout.

**Failure scenario:** an undersized scroller or narrow-width overflow is introduced into that custom content. The runtime sweep never opens it, and the class-wide exception prevents static inspection. A container scratch-root probe containing an `Adw.AlertDialog` with an unsized, non-propagating `ScrolledWindow` returned **0 violations / exit 0**; the equivalent ordinary `Adw.Dialog` scroller was diagnosed by the parser. This demonstrates a blind spot, not an executed claim that today's crop dialog clips.

Source inventory also found no runtime rows for Block App (`ui/approval_dialog.c:421`), Upgrade Access Policy/Remove App Block (`gnome/grotto-panels/src/grotto-app-detail.c:110,142`), panels Resume Signing (`grotto-apps-panel.c:156`), Reset Remote Connections (`grotto-remote-panel.c:105`), application error/resume/background alerts (`apps/grotto/src/main_app.c:260,458,680`), About, Shortcuts, or the standalone approval host (`approval-controller.c:1199-1203`). Bare toolkit surfaces can have justified exceptions, but the static allowlist is not an executed-coverage inventory. Add the missing custom surface and explicitly justify remaining executed exceptions; do not advertise 31 rows as every production dialog. No product layout failure is asserted for these unpresented surfaces.

### F3 — S3, confirmed/executed parser probe — Guard gap: C GtkWindow factories/subclasses bypass the default-size rule

**Where:** `apps/grotto/tests/test-dialog-sizing.py:159,195-203`.

Subclass and factory recognition include only Adwaita dialog/window types, not `GTK_TYPE_WINDOW`, `GTK_TYPE_APPLICATION_WINDOW`, `gtk_window_new()` or `gtk_application_window_new()`. Markup can flag an unsized `GtkWindow`, so C and markup coverage differ silently.

**Failure scenario actually probed:** a scratch production C file declaring `G_DEFINE_TYPE(ProbeWindow, probe_window, GTK_TYPE_WINDOW)` with an empty initializer passed the complete script and its four parser self-test groups: **1 production file, 0 violations, exit 0**. Direct `gtk_window_new()` likewise returned no parser diagnostics; unsized `adw_window_new()` is covered by existing self-tests. Include the requested GTK window families and their self-tests. Independent grep found no currently shipped direct GTK window factory/subclass in these components, so this is a future-regression guard gap, not an existing collapsed window.

## Owner-policy and regression checks

- **Independent static inventory:** searched production C/headers and all `.blp`/`.ui` in both components. The five template classes are `GnostrApprovalDialog`, `SheetQrDisplay`, `SheetQrScanner`, `OnboardingAssistant` and `SignerWindow`; all five Blueprint/generated-UI pairs are inspected. All eight plain C `adw_dialog_new()` sites and the standalone `adw_window_new()` site are in scanned files. The constructor/type searches found no further direct/generic construction site. The **120** scanned files comprise Grotto's 80 C + 11 BLP + 11 UI and panels' 10 C + 4 BLP + 4 UI. File coverage is not semantic completeness (F2/F3).
- **Allowlist:** excluding fixtures, toolkit Shortcuts/About sizing, and viewport-filling main/assistant pages is reasonable. `AdwPreferencesDialog` is also implicitly unrecognized by the C matcher despite being absent from the named allowlist; its Preferences surface is exercised at runtime. Alert-subtree exemption is too broad for custom crop/upload content (F2). The custom Remove Identity alert is exercised despite that exception.
- **Runtime mechanics:** the registry loops two calibrated **920×640 / 360×640** hosts and logged 62 distinct measurements. Openers use production widgets/actions; the existing private service supplies fixture state, not replacement widgets. Connection Link crosses readiness; Recovery waits for populated words. Assertions inspect natural content through scrollers, root bounds, mapped nonzero controls/header ancestors and narrow bottom-sheet ancestry. Scroll content, popovers and unrevealed containers intentionally stop containment recursion (`test-dialog-sizing.c:339-389`). This is useful geometry coverage, not proof of all inner scrolled content/states; F1 invalidates its generic natural-height guarantee.
- **Floors:** 200 px for action/info content and 120 px for alerts are modest lower bounds. At 360 px wide, short bottom sheets actually measure 360 px tall through toolkit layout; ready Connection Link is 505 px, Recovery 371 px and QR display 439 px. Retention measures 200 px at 920 and 360 px at 360. Ask-by-Default uses the 580/610 px available heights and scrolls. Remove Identity measures 547×279 wide and 300×330 narrow without asserted overflow. These floors are sensible at the requested 640 px height, but cannot detect F1's 251 px collapse. A 360×294 host, OSK, larger text and native compositor were not tested.
- **Connection Link/QR:** Blueprint adds content following and a 200/560 px scroller floor/ceiling (`sheet-qr-display.blp:6,14-16`), not a hard dialog height. At 920 px loading/empty content measures 246 px and ready content 479 px; narrow measurements are 360/505 px. AUTO still presents a bottom sheet. Close remains neutral/start; Copy remains icon/end. No suggested Done or new confirmation appears.
- **Buttons and consent:** Recovery retains disabled-until-ready/acknowledged Confirm Backup and existing disclosure/clipboard behavior. Retention and Ask-by-Default retain Cancel-start/suggested-Save-end and default-widget behavior (`ui/dialog-actions.h:7-18`). Ask-by-Default changes label wrapping, not validation or saved `require-confirmation-kinds` semantics. Remove Identity changes wrapping only: full-npub equality, initially disabled destructive Remove, default Cancel and close/Escape Cancel remain (`signer-window.c:775-801`). This preserves the [HIG dialog convention](https://developer.gnome.org/hig/patterns/feedback/dialogs.html).
- **Authority boundary:** the diff does not alter approval dispatch/footer, daemon authorization or panel authority. Deny-first/Return-not-Allow/Escape-deny-once semantics are unchanged by these edits. No always-confirm class, permission reset, new secret API or policy transfer into panels was found. This is a static diff/caller conclusion plus requested layout/a11y checks, not a separately executed approval-policy suite. No-secrets passed.
- **Catalogs/drift:** all five Grotto `.po` files and panels Spanish passed `msgfmt --check --check-format`; all eight PO/POT files have no fuzzy or obsolete entries. Pinned Blueprint **0.22.2** drift passed, including panels. Catalog changes are references/timestamps rather than new message text; this is not complete-translation acceptance.

## Verification actually run

Image: `nostrc-linux-ci:arm64`, ID `sha256:3d1c8f2383c7159a44e3c8f1714476ca749333d34c887b780d6efc144eab35e2`; `uname -m`: `aarch64`; GTK **4.14.5**, libadwaita **1.5.0**. A disposable container mounted this worktree read-only at `/src`; `xdotool` was installed only there. Configured fresh Ninja `/build` with `BUILD_GNOSTR_APP=OFF`, `SIGNET_ENABLE=OFF`, `BUILD_RELAYD=OFF`, `NIP55L_SECRET_BACKEND=libsecret`. Built only prerequisites for `test-ui-layout`, `test-qr-scanner`, `test-grotto-panels-smoke` and `test-grotto-panels-admission`. No developer keychain commands were run.

Exact requested serial command:

```sh
dbus-run-session -- xvfb-run -a -s '-screen 0 1280x800x24' \
  ctest --test-dir /build -j1 --no-tests=error --output-on-failure -V \
  -R '^(signer/(dialog-sizing(-static)?|blueprint-drift|ui-layout|ui-accessibility|qr-scanner)|grotto-panels/(management-panels|no-secrets))$'
```

| Command/check | Actual result |
|---|---|
| `signer/dialog-sizing` | PASS, 29.80 s; independently counted 62 measurements |
| `signer/dialog-sizing-static` | PASS, 3.16 s; 120 files, 0 violations; four parser self-test groups |
| `signer/blueprint-drift` | PASS, 1.36 s; pinned Blueprint 0.22.2 |
| `signer/ui-layout` | PASS, 79.24 s |
| `signer/ui-accessibility` | PASS, 79.17 s |
| `signer/qr-scanner` | PASS, 0.28 s |
| `grotto-panels/management-panels` | PASS, 0.73 s |
| `grotto-panels/no-secrets` | PASS, 4.63 s |
| Required test-homes fixture | PASS, 0.05 s; requested total **9/9, exit 0, 198.45 s** |
| Six shipped catalogs: `msgfmt --check --check-format -o /dev/null` | PASS; eight PO/POT files also free of fuzzy/obsolete entries |
| Cloned-build negative control: only Recovery's content-following setter reverted | **Unexpected PASS**, 62 presentations; runtime + fixture 2/2, 30.22 s; confirms F1 |
| Static checker on that reverted source in a scratch component root | Expected FAIL, exit 1, one missing content-following diagnostic |
| Scratch GTK window subclass and alert/custom-scroller probes | Unexpected static PASS, exit 0; F2/F3 guard omissions, not product execution |
| `git diff --check` | PASS before report delivery |

Receipts: `/tmp/grotto-dialog-sizing-review-20261107/{requested.log,catalogs.log,c-inventory.log,markup-inventory.log,toolkit-inventory.log,static-probes.log,allowlist-probes.json,static-negative-controls.log,collapse-probe.py,collapse-build.log,collapse-clean-homes.log,collapse-static.log}` plus setup/build logs. The initial setup stopped at an already-existing image `/build`; using `mkdir -p` corrected setup only. The first negative attempt inherited the copied fixture database and aborted on duplicate account insertion before presentation (`collapse.log`): **not geometry evidence**. Only the clone's diagnostic home was reset before the meaningful run. These earlier failures remain retained. Task-owned container/build artifacts were removed after copying receipts.

No full suite, affected-lane qualification matrix, native desktop/Orca, live relays/keyring, sanitizers or owner acceptance was run. Existing layout warnings were not suppressed. No guard or timeout was weakened to manufacture a pass.

## Version and delivery decision

This review is documentation-only: **no bump** for Grotto or grotto-panels; no other releasable component changes. Reviewed sizing fixes are PATCH-class corrections folded into the explicitly retained unreleased **Grotto 0.1.1 / grotto-panels 0.1.0**. `VERSION_MANIFEST.md:34-35`, both CMake declarations and both Meson project versions agree and are unchanged across the diff. No API/ABI, storage/wire format, release column or tag changed. Beads evidence is recorded locally; `bd dolt remote list` reports no configured synchronization remote. Git report delivery uses the existing branch upstream.

## Merge recommendation

**Request changes, retaining the UI fixes.** Correct the runtime expected-height calculation and demonstrate that restoring Recovery's collapse fails the executed test (F1). Complete the finite source-to-runtime inventory, exercise custom crop/upload content, and narrow or explicitly justify toolkit exceptions (F2). Add C GTK-window recognition as a small guard correction (F3). Rerun affected focused checks and retain the negative-control receipt. Passing the current eight checks does not establish the regression-detection claim; no extra consent step, product policy or repository-wide test programme is required.
