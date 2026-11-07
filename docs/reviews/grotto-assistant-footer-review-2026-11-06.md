# Setup assistant bottom actions: UX, behavior and boundary review

Date: 2026-11-06 (verification completed across local midnight into 2026-11-07). Review task: `nostrc-cjj0t`; implementation: `nostrc-u6v55.70`.

## Boundary and verdict

Reviewed exactly `git diff 5086a5270...d7b91d4ea` on `grotto/assistant-bottom-actions`:

- Base: `5086a5270`; reviewed HEAD: `d7b91d4ea569659b7028a92ccb896967240c770d`.
- Worktree throughout: `/Users/bizarro/Documents/Projects/.repoprompt-worktrees/nostrc/rp-agent-287e56d1-grotto-assistant-bottom-actions-16a9a15b`.
- Scope: the shared assistant footer, first-run and in-session Add/New/Import entrypoints, retained .65/.53 behavior, English/Spanish access keys, HIG buttons/dialogs, catalogs, Blueprint output and the deliberately updated tests. No product code, committed tests, catalogs, screenshots or version sources were changed by this review.

**Verdict: approve the scoped footer correction, with one separately tracked inherited keyboard defect (F1), not a blanket HIG clean pass.** Both modes fit at 360 and 480 px. All 92 rendered page/state observations have equal footer insets at a given width, the affirmative fills the clamped footer, enabled affirmative actions are Tab-reachable, and the five requested serial checks pass. Import persistence, Return submission, self-test review opening and the advertised destinations remain intact. The requested affirmative-above-secondary arrangement deliberately differs from the HIG's general Cancel-first ordering; it is not grounds to undo the owner's explicit layout direction. Escape does not activate the available cancellation action; this omission predates the scoped diff and is filed as `nostrc-lcs47`, not silently counted as a pass or made a new prerequisite for .70.

Evidence labels: **confirmed/executed** means run during this review; **confirmed/static** means established from source and production callers; **confirmed/visual** means inspected this review's runtime captures. S2 denotes a usability/accessibility defect, not exploit severity. File references are repository-relative at the reviewed HEAD. ARM64 container/Xvfb evidence is not native desktop, hosted CI, Orca, owner acceptance or release proof.

## Findings, ranked

### F1 — S2, confirmed/executed and static — Inherited: Escape does not invoke available Cancel/Skip

**Where:** `apps/grotto/src/ui/onboarding-assistant.c:82-90,365-368,455-478`; the newly shared secondary control is at `apps/grotto/data/ui/onboarding-assistant.blp:137-148`.

**Failure scenario:** open in-session Add an Identity, where Cancel is visible, and press Escape. The assistant stays mapped on Choice instead of cancelling. Real `xdotool` Escape produced the same no-op at 360 and 480 px in English and Spanish. First-run Welcome with Skip Setup, and the saved-identity failed-check view with Finish Later, likewise stayed open: 16 observations in total, with unchanged state and visibility. Cancel remains Tab-reachable; this is not a keyboard trap or evidence that an import was deleted.

The [GNOME HIG dialog-button rules](https://developer.gnome.org/hig/patterns/feedback/dialogs.html#general-guidelines) say: “Ensure that the Esc key activates the cancel button, if there is one.” The assistant installs button-click and window-close handlers, but no Escape mapping. The base revision has the same `AdwWindow` template type and omission in its initializer; neither is added by the footer move. That inheritance determination is static; a base binary was not run.

**Disposition:** follow-up `nostrc-lcs47`. A future correction should route Escape through the appropriate existing cancellation semantics, respecting busy operations and retaining stored identities. No correction, shortcut, handler or test was added by this review.

## Owner-policy and regression checks

- **Real entrypoints:** first run constructs `onboarding_assistant_new()`, installs the completion callback and pending group, then presents it (`apps/grotto/src/main_app.c:624-638`). Add/New/Import all reach `onboarding_assistant_present()` (`apps/grotto/src/ui/signer-window.c:1031,1042,1053`), which sets running-session mode and starts at Choice (`onboarding-assistant.c:495-506`). Source traces and executed template/caller tests agree; the temporary walk used these constructors, not a substitute footer widget.
- **Shared placement:** `onboarding-assistant.blp:8-17,124-150` owns one toolbar bottom area outside the navigation content. Its suggested pill precedes a flat secondary row. The old header finish-menu/popover and header affirmative were removed, not left as duplicate actions. Header content is the bound title, with native close enabled in-session (`onboarding-assistant.c:79`); first run retains no header action. Screenshots show that distinction on every page.
- **No initial in-session Back:** the running-session Choice page has Cancel and no affirmative/Back (`onboarding-assistant.c:76-83`). Both widths and locales exercised this assertion. Later Create/Import/Connect show Back; after storage, no Back can return into an unsaved-identity form.
- **Return submits Create/Import:** the unchanged entry activation wiring and default-widget selection remain in effect (`onboarding-assistant.blp:44,52,58`; `onboarding-assistant.c:103`). The requested English and Spanish suites actually send Return through `xdotool` from the create name, import name and secret-input fields in both modes, and assert exactly one matching create/import call (`apps/grotto/tests/test-ui-layout.c:1560-1606`). These are input events, not merely emitted `clicked` signals.
- **Import Skip never cancels the import, fix `150b89d44`:** the executed onboarding cases retain successful Done, Skip-after-denial and close persistence. They check the stored identity, zero remove calls, reloaded account presence and honest pending setup state (`apps/grotto/tests/test-onboarding.c:258-291,830-832`). Imports still reach Ready without a mandatory mnemonic export (`onboarding-assistant.c:263-269`). The review did not replace those assertions with visibility checks.
- **Self-test review opens, fix `17f67593e`:** additionally rebuilt and ran the directly relevant `signer/approval-integration`, serially after the requested checks. Its real daemon test opens the approval on the assistant via `app.review-request`, verifies Grotto identity/non-reusable permissions, dismisses with Not Now, reopens from the assistant's pending row, approves, and reaches Backup (`apps/grotto/tests/test-approval-integration.c:304-372`). It passed and printed the corresponding completion receipt. This is stronger evidence than the layout fixture's synthetic self-test reply; it still uses private test services, not an operator keyring or live relay.
- **Destinations:** Ready retains Open Grotto; Connect retains Open Connected Apps (`onboarding-assistant.c:98-102,347-349,357`). The existing executed first-run destination case creates the management window only in the completion callback and asserts the Apps panel afterward (`test-ui-layout.c:1630-1661`), matching the production callback (`main_app.c:129-147`) and post-callback navigation (`onboarding-assistant.c:143-157`). Successful identity completion remains scoped to the saved identity. No destination behavior changed in this diff.
- **English/Spanish access keys:** every observed page, including failed check and expanded backup options, was traversed with the catalog active and mapped button/row mnemonics checked for duplicate keys. Both diagnostic walks and both requested layout variants passed. Confirm Backup/Finish Without Backup and Finish with Warning/Finish Later are distinct in both languages. This establishes uniqueness of declared keys, not a claim that every label has a mnemonic or that every Alt combination was separately pressed.
- **HIG styles and cancellation placement:** runtime recursion found at most one mapped suggested/destructive action per observation; Choice and active Check have none. Flat secondary buttons, a full-width pill and imperative destination labels follow the [HIG button styles](https://developer.gnome.org/hig/patterns/controls/buttons.html#button-styles). The [dialog guidelines](https://developer.gnome.org/hig/patterns/feedback/dialogs.html#general-guidelines) generally put cancellation before the affirmative; the explicit .70 design instead places the affirmative above the secondary row, and actual Tab order follows that visual order. Record this owner-directed exception rather than claim strict conformance or recommend returning actions to the header. F1 is independent of that owner-directed layout exception.
- **Tests deliberately updated, not weakened:** the scoped test diff replaces header/popover ancestry assertions with shared-footer ancestry, margins, spacing, fill, pill/flat classes and header-control assertions. It adds running-session page walks and retains Return dispatch/counts, import persistence, unique mnemonics, accessible backup acknowledgement and first-run Apps destination checks (`test-ui-layout.c:523-556,1483-1557,1590-1613`; `test-onboarding.c:164-176,738-751`). No new skip, timeout relaxation or removed behavior assertion was found. The committed walk still covers 360 px only; the independent 480 px diagnostic supplies this review's evidence, not permanent regression coverage.

### Executed page walk, measurements and keyboard order

The temporary diagnostic reused the layout suite's private-bus fixture and linked the unchanged production objects/resources. It walked both modes at X11 surface sizes **360×640** and **480×640**, in English and Spanish, taking and inspecting captures for every listed page/state. Welcome exists only in first run. There were **92 observations**: 12 first-run and 11 in-session states × two widths × two locales. Pending-review dialog opening is proved separately by the real-daemon integration above; no pending review row was fabricated in the measurement fixture.

Declared footer margins are start/end **20/20**, top/bottom **12/16**; vertical spacing is **8**, and the homogeneous secondary row's configured spacing is **12**. Rendered measurements below are relative to the clamp, in logical pixels, ordered **left/right/top/bottom**. Clamp centering adds horizontal inset at 480 px; fractional allocation rounds the right side one pixel larger at both widths. These are stable measured allocations, not incorrectly reported as exactly 20/20 at every window width.

| Page/state | Modes | 360 px rendered insets | 480 px rendered insets | Affirmative → visible secondary gap |
|---|---|---|---|---|
| Welcome | First run | 20/21/12/16 | 40/41/12/16 | 8 |
| Choice | Both | 20/21/12/16 | 40/41/12/16 | No affirmative |
| Connect | Both | 20/21/12/16 | 40/41/12/16 | 8 |
| Import, encrypted-input fields visible | Both | 20/21/12/16 | 40/41/12/16 | 8 |
| Create | Both | 20/21/12/16 | 40/41/12/16 | 8 |
| Check: waiting | Both | 20/21/12/16 | 40/41/12/16 | No affirmative |
| Check: testing | Both | 20/21/12/16 | 40/41/12/16 | No affirmative |
| Check: failed | Both | 20/21/12/16 | 40/41/12/16 | 8 |
| Backup: collapsed options | Both | 20/21/12/16 | 40/41/12/16 | 8 |
| Backup: expanded options | Both | 20/21/12/16 | 40/41/12/16 | 8 |
| Backup: acknowledged/enabled | Both | 20/21/12/16 | 40/41/12/16 | 8 |
| Ready | Both | 20/21/12/16 | 40/41/12/16 | No visible secondary action |

Every cell was asserted equal to the same-width baseline across pages, modes and locales, and independently summarized in `measurements.json`. The button border box fills the footer's **309 px** at 360 and **389 px** at 480, with **44 px** height; the secondary row is **34 px** when populated. The toolkit's content dimensions are 350/470 px because of window decoration; the asserted X11 surface dimensions were 360/480, not silently enlarged windows. Footer height changes with status wrapping and hidden actions; equal margins do not mean identical absolute button Y positions or a visible affirmative on Choice/active Check. Long Spanish labels remain inside the window. Backup content scrolls while footer actions stay outside the scroller.

Real X11 Tab events traversed each observed view and asserted reachability of every mapped, sensitive affirmative. Representative sequences, consistent across widths/locales:

- Create: name entry → Create Identity → Back.
- Import: name → secret-input row/entry → secure passphrase input/reveal → Import Identity → Back.
- Connect: create/import choices → Open Connected Apps → Back.
- Failed check: Details → Try Check Again → Finish with Warning → Finish Later.
- Backup: recovery-word children → acknowledgement → optional backup controls → Confirm Backup (only after acknowledgement) → Finish Without Backup.
- Ready: Open Grotto. Choice has its direct choice rows followed by Back (first run) or Cancel (in-session).

Disabled backup confirmation and busy Check controls were correctly absent from the affirmative-reachability requirement. Escape was explicitly sent and its no-op recorded as F1, not inferred from source alone. No native compositor shortcut or screen-reader session was tested. Screenshots use the published recovery-vector fixture; the uninstalled application icon appears as a missing-icon placeholder in this disposable build, not a new footer defect.

## Verification actually run

Fresh Debug build in `nostrc-linux-ci:arm64`, image ID `sha256:3d1c8f2383c7159a44e3c8f1714476ca749333d34c887b780d6efc144eab35e2`; `uname -m` reports `aarch64`. GTK **4.14.5**, libadwaita **1.5.0**, pinned Blueprint **0.22.2**. The assigned worktree was mounted read-only at `/src`; the fresh build lived only inside the task container at `/build`. Installed `xdotool` there before configuring; installed Pillow there only to assemble screenshot contact sheets. Private HOME/XDG directories, test buses and memory GSettings remained isolated. No developer keychain command was run.

Configuration: `cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_GNOSTR_APP=OFF -DBUILD_TESTING=ON -DBUILD_SIGNER_TESTS=ON -DNIP55L_SECRET_BACKEND=libsecret -DGROUNDHOG_STORE_KEY_DEFAULT_BACKEND=secret-service`. CMake warned that the Groundhog backend variable was unused because Groundhog was not built; no macOS backend was involved. Built only `test-onboarding`, `test-ui-layout`, `test-identity-management`, then the relevant `test-approval-integration` and their prerequisites, not the full test tree.

| Command/check | Actual result |
|---|---|
| `xvfb-run -a ctest --test-dir /build -j1 -R "^signer/(onboarding\|ui-layout\|ui-accessibility\|identity-management\|blueprint-drift)$" --output-on-failure` | **PASS 6/6** including required home fixture, zero skips, 171.61 s. Identity 3.57 s; onboarding 6.38 s; layout 80.16 s; Spanish accessibility 80.11 s; drift 1.32 s |
| Same regex, initial attempt as image's default user | Infrastructure failure before any test: could not create `/build/Testing/Temporary` in the root-owned build. Retained receipt; subsequent explicit root-container run above succeeded with private test homes |
| Temporary `/build/footer-probe`, English then Spanish, serially | Both diagnostic tests PASS, 26.06/26.11 s; 92 rendered equality/style/focus observations and 16 recorded Escape no-ops. Not a newly committed test or an Escape pass |
| `xvfb-run -a ctest --test-dir /build -j1 -R "^signer/approval-integration$" --output-on-failure` | **PASS 2/2** including fixture, zero skips, 12.16 s; real-daemon assistant self-test action/inbox review and completion executed |
| `msgfmt --check --check-format -o /dev/null <catalog>` for all five Grotto catalogs plus panels Spanish; scan for fuzzy flags | PASS all six, no fuzzy entries. Blueprint drift also validates catalog/resource consistency; not a claim of complete translations |
| `git diff --check 5086a5270...d7b91d4ea` | PASS |

Local receipts: `/tmp/grotto-footer-review-2026-11-06/{configure.log,build.log,serial-five.log,LastTest.log,catalogs.log,probe-en.log,probe-es.log,measurements.json,footer-probe.c,build-probe.py,approval-integration.log,approval-integration-detail.log,sheet-*.png}`. The initial CTest permission failure and two diagnostic instrumentation failures are retained. The latter initially confused GTK content width with decorated surface width, then failed to account for clamp centering at 480; the final probe measures border bounds, asserts surface size and compares per-width rendered allocations. No production assertion or requested suite was weakened to pass the probe. The report is the only tracked file added; task-owned container/build removed after collecting evidence, other containers/worktrees untouched.

Jev was used for eight `find-lines` calls to narrow long C/CMake reads (one partial CMake result required a targeted follow-up search); no judgment was used as a substitute for execution. Usage: 33 requests, 265,726 input tokens, approximately $0.011161. No failed API judgments; only relevant source ranges were read.

No full suite, broad/attested gate, sanitizers, native GNOME/Orca, live-relay/operator-keyring exercise, hosted CI or release acceptance was run or claimed. No finding here authorizes an implementation fix, policy change or scope expansion.

## Version and delivery decision

This review is documentation-only: **no bump** for Grotto, grotto-panels or any other component. The reviewed UI correction is PATCH-class within the existing unreleased Grotto **0.1.1** wave recorded by .70; grotto-panels is unchanged at **0.1.0** (`VERSION_MANIFEST.md:34-35`). No API/ABI, storage/wire, version source, published release column or tag was changed. The review and follow-up evidence are recorded in beads rather than a new tracking ledger.

## Merge recommendation

**Approve `5086a5270...d7b91d4ea` for the requested bottom-action restoration.** Both entrypoint modes, widths, rendered footer equality, affirmative keyboard access, Return submission, unique English/Spanish mnemonics, import persistence, self-test review and destinations have current evidence. Keep F1 as the separately tracked inherited Escape defect (`nostrc-lcs47`); do not present it as fixed, or convert it into a new prerequisite for this finite correction. Retain the explicitly requested bottom arrangement while acknowledging its Cancel-order HIG exception. No additional confirmation ceremony, mandatory import backup, full-suite qualification or product fix is recommended as part of this report-only delivery.
