# Grotto dialog-button convention: UX, behavior and boundary review

Date: 2026-11-06. Review task: `nostrc-0hppe`; implementation: `nostrc-u6v55.57`.

## Boundary and verdict

Reviewed exactly `git diff c03558077...3c8ef853e` on `grotto/dialog-button-convention`:

- Base: `c03558077`; reviewed HEAD: `3c8ef853eaf8ea18aa302376cdec2f858cac13be`.
- Worktree throughout: `/Users/bizarro/Documents/Projects/.repoprompt-worktrees/nostrc/rp-agent-9680bfa2-grotto-dialog-button-convention-fe460ac3`.
- Scope: changed Grotto sheets, their production callbacks, generated UI, catalogs and deliberately updated tests. The separately owned panels profile-editor change is not in this diff. No implementation, tests, catalogs, generated UI or version sources were changed by this review.
- Yardsticks: the requested `~/.claude/skills/gnome-hig/references/patterns/feedback/dialogs.md` and `patterns/controls/buttons.md`, generated from upstream HIG commit `97028eac3792`.

**Verdict:** the header convention is substantially wired, and **all seven requested suites pass serially** in the equipped ARM64 container. It is **not merge-ready unchanged**: declaring a default widget does not make Return work from the form entries, and the QR Copy callback still assumes the former text button, breaking the new icon convention and overflowing the Spanish header. The Spanish recovery affirmative also exceeds the narrow sheet, and the new passphrase sheet initially focuses Cancel instead of its input. Approval consent semantics and the requested version boundary remain intact.

Evidence labels: **confirmed/executed** means observed during this review; **confirmed/static** means established from source and its caller; **confirmed/visual** means inspected a freshly generated screenshot. S2 denotes a user-visible keyboard/layout defect; S3 denotes focus polish. References are repository-relative at the reviewed HEAD. Container/Xvfb/private-bus evidence is not native desktop, hosted CI, owner acceptance or release proof.

## Findings, ranked

### F1 — S2, confirmed/executed and static — Blocker: Return does not activate enabled form affirmatives

**Where:** `apps/grotto/src/ui/dialog-actions.h:16`; consumers at `apps/grotto/src/ui/page-settings.c:50-55,137-141`, `apps/grotto/src/ui/page-identity-detail.c:334-347`, `apps/grotto/src/ui/pairing-flow.c:157-160`, and `apps/grotto/data/ui/sheets/sheet-create-bunker.blp:17`. The secure entry forwards its own activation at `apps/grotto/src/ui/widgets/gn-secure-entry.c:373-380`, but the backup dialog does not connect it.

The [HIG dialog-button guidance](https://developer.gnome.org/hig/patterns/feedback/dialogs.html#general-guidelines) says, “Assign the return key to activate the affirmative button,” except for unsafe actions. Setting `AdwDialog.default-widget` alone does not route the focused entries' activation to that button. These entries/rows do not opt into default activation or explicitly forward their activation.

**Failure scenario:** enter a valid backup passphrase, custom event types or app link, or choose a valid Create Pairing Link identity and leave focus in App Name; Return leaves the sheet open without the enabled action running. Both English and Spanish production-widget diagnostics recorded **zero** affirmative deliveries for Ask-by-Default custom input, retention spin input, encrypted QR/file passphrase input and the valid Create Pairing Link name field. A standalone production-callback probe independently showed valid Paste Link + Return staying on Add App; clicking the same Preview button immediately opened Connect an App. Focused affirmative buttons did receive Return, ruling out a broken xdotool route. The empty passphrase stayed insensitive and delivered zero actions.

Connect the appropriate entry activation/default behavior for these safe actions, without applying an automatic affirmative to destructive confirmations or Review Request. The added tests inspect sensitivity and placement, and mostly emit `clicked` directly; they do not establish these Return paths.

### F2 — S2, confirmed/executed, static and visual — Blocker: QR Copy resets into a wide, literal-underscore text button

**Where:** new icon at `apps/grotto/data/ui/sheets/sheet-qr-display.blp:11`; unchanged callbacks at `apps/grotto/src/ui/sheets/sheet-qr-display.c:51-57,76-83`; new coverage at `apps/grotto/tests/test-ui-layout.c:975-979` stops before clicking Copy.

The new header button correctly starts with `edit-copy-symbolic` and a translated tooltip. Its production callback calls `gtk_button_set_label("Copied")`, replacing the icon. After the feedback timer, it sets `Cop_y to Clipboard`, but the new icon button has `use-underline=false`: the underscore is now displayed literally, and the icon never returns. This contradicts this task's explicit secondary-header-icon convention. The [HIG buttons guidance](https://developer.gnome.org/hig/patterns/controls/buttons.html#general-guidelines) also requires short labels that account for localized width.

**Failure scenario:** open Share Public Key and click Copy. Clipboard contents are correct (`nostr:` followed by the displayed npub), and immediate translated Copied/success feedback works. After reset, English displays `Cop_y to Clipboard` and squeezes the title; Spanish displays `Cop_iar al portapapeles`, requires **392 px** in a **350 px** content area of the requested 360 px window, and clips the action at the right edge. The standalone GUI screenshot confirms the clipping. GTK also reports minimum width 392 versus natural width 360; the assertion-bearing diagnostic aborts on that warning. This is not a claim that the normal application invariably crashes.

Keep the icon through feedback/reset and provide feedback through an appropriate tooltip/toast/icon state. Retain the existing clipboard bytes, lease/close cleanup and translated feedback. Exercise the actual click and reset, not only the initial tooltip.

### F3 — S2, confirmed/executed and static — Should-fix: the Spanish recovery affirmative exceeds the narrow sheet

**Where:** `apps/grotto/src/ui/page-identity-detail.c:310-314,323`; `apps/grotto/po/es.po:3077-3079`.

Moving `Confirm _Backup` to the header makes its Spanish `Confirmar _respaldo` compete with Cancel and the title in a non-wrapping action area. Opening the production Recovery Phrase dialog in the Spanish private-bus fixture repeatedly reports `AdwBottomSheet ... requested 374 px, 350 px available`. This occurs **before** using QR Copy, so it is separate from F2. The [HIG buttons guidance](https://developer.gnome.org/hig/patterns/controls/buttons.html#general-guidelines) explicitly calls for short labels and consideration of localized widths.

**Failure scenario:** at the same narrow window size used by the layout checks, the Spanish recovery sheet requests more horizontal space than is available. The existing Spanish layout test visits the Backup page but does not open this recovery sheet. Use a fitting localized affirmative/header layout while retaining a unique access key and the existing acknowledgement gate. This is executed sizing evidence, not an inspected recovery screenshot or an Orca finding.

### F4 — S3, confirmed/executed and static — Nit: the new passphrase sheet initially focuses Cancel

**Where:** `apps/grotto/src/ui/page-identity-detail.c:309-315,334-347,373-375`; Cancel is inserted first at `apps/grotto/src/ui/dialog-actions.h:11-13`.

The [HIG dialogs guidance](https://developer.gnome.org/hig/patterns/feedback/dialogs.html#general-guidelines) requires initial focus on the component users are expected to operate first. The passphrase sheet adds a focusable header Cancel before its input without assigning input focus. The production-widget diagnostic reports initial focus `GtkButton`, label `_Cancel` / `_Cancelar`, in English/Spanish; the password field is correctly masked.

**Failure scenario:** open Encrypted Backup and start typing the required passphrase: focus is not in that field, so keyboard users must find it first. Set initial focus to the secure input, preserving Cancel-first visual order and Esc dismissal. This does not justify auto-export or any extra confirmation step.

## Owner-policy and regression checks

| Surface/boundary | Evidence and result |
|---|---|
| QR display | Opened through its production template in both locales; Close is first, Copy is a tooltip-equipped secondary header icon initially; no window-close controls. Actual clipboard and feedback checked, with F2 after reset. Informational Close is not incorrectly styled as a suggested commit. |
| QR scanner | Opened initial/result/back states. Import/Continue is the sole suggested header action, insensitive before a valid accepted result, enabled afterward, disabled on return to scanning. Secondary camera/paste/file/back controls use `AdwActionRow.activated`, matching their new types. Initial Esc, real keyboard Back then Esc, and affirmative Return/success callback were exercised. Camera hardware was not used. |
| Create Pairing Link / Add App | Production Add App action opens the sheet; its header Create Link opens the real create sheet. Required remote LISTENING state and selected identity gate creation (`sheet-create-bunker.c:17-20,45-54,74-84`); the diagnostic supplied those fixture conditions and observed disabled then enabled states. Add App's always-enabled affirmative navigates to that form; it does not itself mint a link. F1 concerns Return from App Name, not that validation. |
| Encrypted Backup / Recovery Phrase | Opened QR passphrase, file passphrase, recovery, copy warning and standalone backup shell through their owning page callbacks. Empty passphrase and missing acknowledgement disable affirmatives; file success hides Save and switches the header/default to Confirm Backup. The requested onboarding suite retains file cancellation/failure, file bytes/mode, acknowledgement, secret lifetime and clipboard cleanup checks. F3/F4 are the sizing/focus exceptions. |
| No secret export on opening the passphrase form | `secret_dialog()` builds controls; `export_commit()` reaches `export_call()` only on the affirmative; `export_call()` refuses an empty passphrase, invokes `ExportIdentity`, then wipes the entry (`page-identity-detail.c:224-231,292-297,304-375`). An encrypted QR is constructed only in the export reply (`:200-204`). The executed form diagnostics left the export callback blocked solely while counting keyboard delivery; they are not end-to-end export proof. The unmodified onboarding suite separately exercises the real file-export callback. |
| Ask-by-Default / retention | Both opened with Cancel first and suggested Save last. Invalid custom kind 65536 disables Save; empty custom input is valid. Restore Defaults stages controls rather than writing immediately, and Cancel preserves the saved policy. Retention changes remain staged until Save; Esc preserves the original value. Production settings writes remain in the existing owning component. F1 is the missing entry-to-Save Return path. |
| Pairing paste / preview | Invalid/empty link disables Preview; parsed app link enables it. Preview Review remains disabled until an identity is selected; selection enables it and an in-flight request disables the identity selector. Both use the shared header and explicit initial input focus. The standalone probe verified Preview click and preview Esc; no live pairing/relay operation was performed. |
| Confirmations | Raw-key copy, recovery-copy, replacement and identity removal remain `AdwAlertDialog`, with Cancel first, final destructive/suggested response and Cancel default/close response. The recovery-copy warning actually cancelled on Return. No destructive affirmative was introduced as a default. |
| Access keys / icon tooltips | Source/catalog inspection finds no duplicate assigned English/Spanish access keys per changed dialog, including recovery/retry and scanner states. Mapped-key assertions pass in both locale suites; the extra recovery/copy-warning checks also found no collision. Copy and password-visibility icons have tooltips. The fixture has no usable camera, so running-camera wording was checked statically, not via hardware. This is not complete translation or assistive-technology acceptance. |
| Review Request consent | `approval_dialog.c`, `approval-dialog.blp` and `approval-dialog.ui` are byte-unchanged across this diff; the helper is not applied there. All 40 approval-dialog cases pass, including the existing no-default-widget and Deny-focus assertions (`test-approval-dialog.c:400-410`). No permission model, consent footer or approve-once semantics were changed. |
| Blueprint/catalogs/tests | Pinned 0.22.2 drift check passes, including generated UI/resource/catalog inventories. All five shipped Grotto catalogs plus panels Spanish pass `msgfmt --check --check-format`. No shipped `.po` has a fuzzy flag; the `.pot` has only its normal template-header fuzzy marker. Spanish new action translations were inspected, not trusted merely because msgfmt passed. Test changes deliberately track mnemonic labels and row activation and add validation/cancel/header checks; existing safety assertions were not removed or weakened. Their coverage omissions explain F1–F3, not a recommendation to remove assertions. |

## Verification actually run

Linux image: `nostrc-linux-ci:arm64`, ID `sha256:3d1c8f2383c7159a44e3c8f1714476ca749333d34c887b780d6efc144eab35e2`; GTK **4.14.5**, libadwaita **1.5.0**, Blueprint **0.22.2**. A new disposable container mounted **this worktree read-only** at `/src`; its fresh build was `/tmp/grotto-review-build`. `xdotool`/libxdo were installed only in that container. Test homes/private buses were isolated; no developer keychain command was run.

```sh
cmake -S /src -B /tmp/grotto-review-build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON -DBUILD_APPS=ON -DBUILD_GNOSTR_APP=OFF \
  -DBUILD_NOSTR_GTK=OFF -DBUILD_LIBMARMOT=OFF -DBUILD_MARMOT_GOBJECT=OFF \
  -DSIGNET_ENABLE=OFF -DNOSTR_WITH_GLIB=ON -DNIP55L_SECRET_BACKEND=libsecret \
  -DGROUNDHOG_STORE_KEY_DEFAULT_BACKEND=secret-service
cmake --build /tmp/grotto-review-build --parallel 4 --target \
  grotto test-ui-layout test-qr-scanner test-identity-management \
  test-onboarding test-approval-dialog
xvfb-run -a ctest --test-dir /tmp/grotto-review-build -j1 \
  -R '^signer/(ui-layout|ui-accessibility|qr-scanner|identity-management|onboarding|blueprint-drift|approval-dialog)$' \
  --output-on-failure
```

Configure/build passed. Test execution was serial (`-j1`), exit **0**, **136.20 s**:

| Requested check | Actual result |
|---|---|
| `signer/ui-layout` | PASS, 3 cases, 59.40 s |
| `signer/ui-accessibility` | PASS, 3 cases with Spanish/GTK test accessibility, 59.42 s |
| `signer/qr-scanner` | PASS, 3 cases, 0.28 s |
| `signer/identity-management` | PASS, 12 cases, 3.57 s |
| `signer/onboarding` | PASS, 32 cases, 6.08 s |
| `signer/blueprint-drift` | PASS, 1.40 s |
| `signer/approval-dialog` | PASS, 40 cases, 5.99 s |
| Required `grotto-tests-test-homes` setup fixture | PASS, 0.05 s; not an eighth requested signer suite |
| `msgfmt --check --check-format -o /dev/null <catalog>` | PASS for all six shipped `.po` catalogs |

**Additional diagnostics, not a substituted green gate:** a temporary copy of the layout fixture was linked to unchanged production objects/resources. Its keyboard counter blocked only the mutating affirmative callback while observing real xdotool Return delivery; a 600 ms event-loop deadline allowed button activation to complete. It also opened file/recovery states and used the actual QR Copy callback/clipboard. The bounded English run stopped at a synthetic scanner Back activation with focus left on the disabled header action; the Spanish run stopped on F2's GTK size warning (both exit 133). These are **partial runs**, not passing suites. A subsequent small standalone GUI probe, with ordinary application logging and unchanged production callbacks, verified normal keyboard Back/Esc, scanner success Return, paste Return versus click, preview Esc, QR Esc, and the Spanish Copy clipping. It exited 0 in both locales but **observed defects**; its exit status is not a claim that the reviewed UI passes.

The first temporary-probe compile lacked the fixture-header include path; the first executable probe incorrectly expected a bare npub rather than the production `nostr:npub` clipboard form (exit 134 in both locales). Those harness errors were corrected without changing product code, and their receipts remain. The standalone probe's first compile also had a C symbol-name collision; its failed compile receipt remains (the first invocation consequently could not run). No required suite was rerun to replace a failure, no warning was suppressed in the required tests, and no production/test assertion was weakened.

Local receipts and reproduction sources: `/tmp/grotto-dialog-review-20261106/`, including `configure.log`, `build.log`, `tests-driver.log`, `ctest-LastTest.log`, `catalogs.log`, `make-probe.py`, `review-probe.c`, `build-probe.py`, `run-probe.py`, `probe-*-initial.log`, `probe-*-bounded.log`, `mini-probe.c`, `mini-{en,es}.log`, `mini-keyboard.log`, and fresh Copy-reset screenshots. The English and Spanish Copy-reset PNGs were directly inspected. The task-owned container/build was removed after copying the receipts; the host receipts are retained. No full suite, affected-lane programme, native desktop/Orca session, live relay/pairing, camera device, hosted CI or release qualification was run.

Context retrieval used advisory Jev `find-lines` queries to narrow long files; the absent onboarding QR/keyboard query redirected inspection to the layout fixture. Source was read independently for findings; no AI relevance judgment was treated as behavioral proof. Retrieval usage: 38 requests, 280,980 input tokens, approximately $0.0118; no threshold result authorized a mutation.

## Version and delivery decision

Grotto remains **0.1.1**, grotto-panels remains **0.1.0**, both unreleased (`VERSION_MANIFEST.md:34-35`). CMake/Meson/package/metainfo/version sources and the entire panels tree have no net change in this review range; the manifest adds only the owner's fold note. The fresh configure also declares Grotto 0.1.1. This report is documentation-only: **no bump** for either component, no release-column/tag update and no other component change. Only this report is committed for the review; the branch is not merged into master by this task.

## Merge recommendation

**Request changes before merging unchanged.** Correct Return routing for the safe forms (F1), preserve the QR icon through its actual Copy/reset lifecycle (F2), and fit the Spanish recovery header (F3); set useful initial passphrase focus as a small polish correction (F4). Keep the shared Cancel-first header convention, validated affirmatives, staged cancellation, alert confirmations and untouched Review Request footer. Verify those finite corrections through their real widgets/callbacks in both locales; there is no basis here for new consent policy, extra secret-reveal ceremony, version bumps or a full-suite prerequisite. No implementation fix was made by this review.
