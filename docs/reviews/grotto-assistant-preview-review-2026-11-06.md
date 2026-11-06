# Assistant header and app-link preview: UX, behavior and regression review

Date: 2026-11-06. Review task: `nostrc-xctwv`; implementation: `nostrc-u6v55.65` and `nostrc-u6v55.66`.

## Boundary and verdict

Reviewed exactly `git diff e37bc5e3f...1a2efe1ec` on `grotto/add-identity-paste-link-fixes`:

- Base: `e37bc5e3f`; reviewed HEAD: `1a2efe1ec9f6c1737a8975cc2c3e40d9eea0626d`.
- Worktree throughout: `/Users/bizarro/Documents/Projects/.repoprompt-worktrees/nostrc/rp-agent-a12adb00-grotto-add-identity-paste-link-f-7e719dd4`.
- Scope: the reused onboarding assistant's header, navigation, keyboard/i18n/layout behavior; Add App → Paste App Link preview sizing, invalid input and pairing boundaries; their changed templates, catalogs and tests. No implementation, tests, catalogs, screenshots or versions were edited by this review. Diagnostic copies lived outside the checkout; the container's source mount was read-only.

**Verdict:** the shared header and larger preview are implemented, and all six requested checks pass serially. **Request changes for the remaining 360 px preview acceptance gap (F1).** The required keyboard examination also finds inherited Return and Spanish mnemonic defects (F2/F3); these are not newly introduced import or authorization regressions. The header-placement portion of .65 is sound. No import cancellation, destination change, always-confirm class or pairing-authority change was found.

Evidence labels: **confirmed/executed** means observed during this run; **confirmed/static** means source/caller tracing, not live acceptance. S2 denotes a usability/coverage defect and S3 localization polish, not exploit severity. File references are repository-relative at the reviewed HEAD. Container/Xvfb/private-bus evidence is not native desktop, hosted CI, live pairing, owner acceptance or release proof.

## Findings, ranked

### F1 — S2, confirmed/executed and static — The 360 px preview still overflows when an identity is selected

**Where:** `apps/grotto/src/ui/pairing-flow.c:44-46,154-158,180-183`; incomplete selection coverage at `apps/grotto/tests/test-ui-layout.c:1289-1306`.

The identity model displays full npubs in an unconstrained `GtkDropDown`. The new natural-height bounds do not constrain that selected label. With one real-format fixture npub, the dropdown's measured minimum/natural width changes from **92/92 px** before selection to **593/593 px** afterward. The enclosing dialog then reports **minimum 629 px, natural 520 px**. In both English and Spanish, the diagnostic aborts on GTK's invalid-measurement warning (exit 133). This is not evidence that a normal non-fatal-warning application crashes; it is direct evidence that its selected-identity minimum cannot fit 360 px.

**Failure scenario:** Add App → Paste App Link → valid connection link → choose an existing identity. Review becomes sensitive, but the width requirement is now larger than the narrow parent. The committed tests exercise the real clipboard entrypoint, but never select the returned identity or wait for a selected-label allocation; their passing disabled-Review screenshots do not cover the necessary next step.

**Attribution:** a controlled relink using the base pairing implementation, base assistant implementation and base assistant UI reproduces the same 593/629 px measurements and warning. This is an **inherited defect in the explicitly requested .66 workflow**, not a new regression caused by natural-height propagation. The .66 claim that the workflow works at 360 px remains incomplete. Constrain the selected identifier's presentation without changing the identity value or pairing/approval semantics.

### F2 — S2, confirmed/executed and static — The new default affirmative is not activated by Return in the input fields

**Where:** `apps/grotto/src/ui/onboarding-assistant.c:465-467`; `apps/grotto/data/ui/onboarding-assistant.blp:57,65,71`; signal-based interaction helper at `apps/grotto/tests/test-onboarding.c:49-55`.

Setting the window's default widget does not make the name `AdwEntryRow` or secret-key `GtkEntry` activate it. The template does not enable their activates-default behavior. Actual X11 Return events from the name field leave the assistant in Create with **no StoreKeyWithMnemonic call**. Return after entering a valid fixture key leaves it in Import with **no ImportIdentity call**; the entry reports `activates-default=0`. This reproduced in first-run and running-session assistants, in both English and Spanish. Clicking the same affirmative afterward sends the expected call.

**Failure scenario:** type a name or paste a key, then press Return expecting the header's Create Identity / Import Identity action. Nothing happens; keyboard users must navigate to the button or use its mnemonic. This misses the [HIG default-action convention](https://developer.gnome.org/hig/patterns/feedback/dialogs.html#general-guidelines) requested for review. The fields also lacked activates-default at the base, so this is an **inherited keyboard gap left unaddressed by the new default-widget wiring**, not newly broken import processing. The changed tests emit `clicked` and do not cover this case.

### F3 — S3, confirmed/executed and static — Spanish failed-check actions share Alt+F

**Where:** `apps/grotto/po/es.po:3187-3188,3211-3212`; simultaneous actions selected by `apps/grotto/src/ui/onboarding-assistant.c:97-98,108-111`.

After a denied check, open More Setup Options. The mapped menu action `_Finalizar más tarde` and header affirmative `_Finalizar con advertencia` both advertise **F**. A mapped-control mnemonic collector reports the collision in both assistant modes. Other examined English/Spanish pages, including the expanded backup actions and backup finish menu, have distinct assigned keys.

**Failure scenario:** a Spanish keyboard user opens the failed-check menu and encounters two actions advertising the same access key, contrary to the requested per-page uniqueness and [HIG access-key guidance](https://developer.gnome.org/hig/guidelines/keyboard.html#access-keys). This establishes duplicate advertised keys, not that GTK dispatches an incorrect authorization action. Both existing handlers finish with a warning; neither deletes the identity. These translations and their simultaneous availability predate the diff; moving one into a menu did not resolve the inherited collision.

## Owner-policy and regression checks

### Assistant page walk

The diagnostic reused the onboarding fixture's private D-Bus service and linked the unchanged production objects/resources. First-run instances used `onboarding_assistant_new()`; running-session instances used the real `onboarding_assistant_present()` entrypoint. Actual X11 Tab/Return/Space/Escape events were sent with xdotool. Both English and Spanish were walked at a requested 360 px window width, including denied-check and expanded-backup states omitted from the new width assertions.

| Page/state | First run: start / end | Running session: start / end | Observed keyboard/layout behavior |
|---|---|---|---|
| Welcome | Skip Setup / Get Started | Not an Add Identity entry page | Return on affirmative reaches Choice; both controls reachable |
| Choice | Back / no affirmative | Cancel / no affirmative | Direct choice rows reachable and activated by Return; no stacked footer actions |
| Create | Back / Create Identity | Same | Name and both header controls reachable; field Return fails as F2 |
| Import | Back / Import Identity | Same | Label/key and both header controls reachable; field Return fails as F2 |
| Connect | Back / Open Connected Apps | Same | Both direct choice rows and header actions reachable; Return on Back returns to Choice |
| Check waiting/testing | Disabled options menu / no affirmative | Same | Busy guard prevents finishing; content disclosure remains reachable; no misleading enabled primary action |
| Check failed | Options → Finish Later / Finish with Warning | Same | Retry, menu, menu item and affirmative reachable; Spanish open-menu collision is F3 |
| Backup | Options → Finish Without Backup / insensitive Confirm Backup until acknowledged | Same | Checkbox and expanded Copy/Open Backup/Try Again reachable; Space acknowledges; Return on Confirm reaches Ready |
| Ready, including successful import | No start action / Open Grotto | Same | Affirmative reachable; import reaches Ready without requesting a seed export |

Header actions remain inside one `AdwHeaderBar`, before/after its title in start/end order; only the end affirmative is suggested. The old stacked bottom buttons are gone. Titles track the current page and ellipsize rather than displacing the buttons. At the narrow size the action labels remained visible; the measured assistant horizontal minimum was 320 px on most pages, 360 px on Spanish Welcome, 354 px on Spanish Connect and 331 px on Spanish Check Failed. Client allocation was 350 px because this unmaximized X11 window included decoration margins. Inspected English/Spanish captures showed title ellipsization, not lost action labels. Expanded backup content scrolls while its header remains available. This is not an Orca or physical-phone session.

- **Import Skip, `150b89d44`:** storage still precedes the check; successful import goes directly to Ready (`onboarding-assistant.c:254-280`), and Skip only calls `finish(FALSE)` (`:380-384`). The executed onboarding suite includes import Done/Skip/close persistence, absence of forced seed export, and no removal calls (`test-onboarding.c:258-292`). All passed.
- **Self-test review opens, `17f67593e`:** source still attaches pending requests to onboarding and routes review immediately (`main_app.c:202-209,265-275`). An additional real-daemon approval-integration run opened review on the assistant, closed/reopened it through the inbox, approved it and reached Backup (`test-approval-integration.c:332-378`). Its log explicitly records completion of that sequence. The overall extra check later failed during shutdown; see verification below, not a full integration pass.
- **Destinations:** `finish()` still resolves the management window after its first-run completion callback and preserves `requested_page` (`onboarding-assistant.c:140-173,352-354`). The executed layout suite includes the first-run Connect lifecycle, creates management only after completion, and asserts the Apps panel (`test-ui-layout.c:1458-1499`). Onboarding's successful-import destination case also passed. Open Grotto / Open Connected Apps were not relabelled to different destinations.
- **Pairing semantics:** the source diff does not change `identity_selected`, `identities_ready`, `request_review` or `accepted`: selection gates Review, AcceptConnectURI receives the original URI and selected identity, and closing an accepted in-flight pairing still cancels its exact returned request ID (`pairing-flow.c:28-83`). No websocket send, relay authorization, permission grant or automatic connection was added by previewing. No protocol audit or live relay pairing was warranted or claimed for these presentation edits.
- **No always-confirm class:** approval/policy implementations and permission UI are outside this diff. The real self-test review prefix also asserted non-rememberable MANAGEMENT/self_test metadata and hidden reusable permission controls. Nothing here recommends new confirmation ceremonies or changing import custody.

### Preview size, invalid input and templates

- The real Add App clipboard tests passed in English and Spanish at 360 px, for valid and invalid text, with details collapsed/expanded. Header Cancel/Review ancestry, secret non-echo, invalid title/error, disabled identity selector and insensitive invalid Review are asserted (`test-ui-layout.c:1283-1318`). The source returns before identity loading on invalid input (`pairing-flow.c:89-97`). Selecting a valid identity enables Review in the diagnostic before F1's width warning.
- The scroller has natural-height propagation, **240 px minimum content height / 520 px maximum**. In a separate diagnostic at an outer 360×600 size, the parent client allocation was 350×590 and the automatic presentation used **AdwBottomSheet**. Its scroller allocated 350×314; measured vertical minimum/natural sizes were 240/254 px for the English collapsed valid preview, 240/356 expanded, and 240/240 invalid. Spanish natural sizes were 271 and 373 px. These are actual measurements, not just property checks.
- Short-height limit: at an outer 360×294 size, the parent client height was 284 px while the sheet's floor was 286 px (240 content + 46 header). Libadwaita warned that the sheet exceeded available height by 2 px. Do not turn the 360 px width evidence into an unconditional short-height/bottom-sheet fit claim. The geometry diagnostic completed those observations, then failed its private-bus teardown because its registered-but-not-run diagnostic application retained a connection; it is not a passing product test or a proven product leak.
- Pinned Blueprint **0.22.2** drift checking passed, including generated UI/resource/catalog consistency. All five changed Grotto catalogs passed `msgfmt --check --check-format`; none contains a fuzzy entry. Spanish coverage was verified with an explicit translated-string assertion. Early diagnostic attempts labelled es actually used the C locale: those logs were retained, rejected as Spanish evidence, and replaced by a corrected diagnostic locale configuration, not by product changes.
- Tests were updated additively and deliberately: header ancestry/style/title assertions, opening the relocated finish menu, natural-height properties/allocations, and the production clipboard path. Existing persistence checks and width assertions were retained. Their limitation is selected-identity allocation (F1), field Return (F2), and failed-check/open-menu mnemonics (F3), not wholesale invalidation of the six passing suites.

## Verification actually run

Image: `nostrc-linux-ci:arm64`, ID `sha256:3d1c8f2383c7159a44e3c8f1714476ca749333d34c887b780d6efc144eab35e2`. Hooks were installed first. xdotool was installed only in the task-owned disposable root container. Fresh Debug build at `/build`, read-only worktree at `/src`; focused targets explicitly built with libsecret, private test homes/buses and the existing isolation guards. No developer keychain settings were touched.

| Command/check | Actual result |
|---|---|
| Build `test-onboarding test-ui-layout test-identity-management test-qr-scanner`, `-j4` | PASS; prerequisites built, not unrelated tests |
| `xvfb-run -a ctest --test-dir /build -j1 -R '^signer/(onboarding\|ui-layout\|ui-accessibility\|identity-management\|blueprint-drift\|qr-scanner)$' --output-on-failure` | PASS: six requested tests plus home fixture, 7/7, zero skips; total 163.87 s |
| Individual requested checks | onboarding 5.88 s; ui-layout 76.03 s; ui-accessibility 76.82 s; identity-management 3.53 s; blueprint-drift 1.33 s; qr-scanner 0.23 s |
| `msgfmt --check --check-format -o /dev/null` on es/fa/id/ja/pt_BR; fuzzy scan | PASS, all five; no fuzzy |
| English/Spanish, first-run/running-session diagnostic page walks | Completed all four walks; actual key events establish F2/F3; not additional committed tests |
| Selected-identity preview diagnostic, en/es | FAIL, exit 133 on GTK width warning; F1. Scoped base-code/resource control reproduces the same warning |
| Geometry diagnostic, en/es | Normal/expanded/invalid and short-height measurements obtained; later diagnostic bus teardown FAIL, exit 133, as qualified above |
| Additional `signer/approval-integration` | FAIL, CTest exit 8: self-test opening/approval and later GUI flow completed, then GUI worker signal 11 at coordinator assertion `test-approval-integration.c:1170` |
| Controlled base-code/resource integration relink | PASS, exit 0; **not a whole-base build**. Only assistant/pairing objects and assistant UI were replaced by base versions |
| GDB-worker diagnostic of integration shutdown | Worker exited normally; no stack. Debugger's subsequent `bt` returned “No stack”/exit 1 and the coordinator failed. Does not reproduce, explain or erase the original signal 11 |
| `git diff --check e37bc5e3f...1a2efe1ec` | PASS |

The intermittent extra integration shutdown failure is tracked separately as **`nostrc-4iz0k`**. The differing base-control/debugger outcomes do not establish its cause or prove an assistant regression; no retry-to-green, guard weakening, suppression or fix was used. All failed receipts remain retained. This does not invalidate the executed self-test-opening prefix, nor permit calling the entire integration check green.

Receipts and diagnostic sources: `/tmp/grotto-assistant-preview-review-20261106/`, including `focused-tests.log`, `catalogs.log`, `deps.log`, build/configuration logs, `make-probe.py`, `pairing-probe.inc`, `build-baseline.py`, `gdb-worker.py`, and `container-evidence/{walk-*,preview-*,selftest-integration,baseline-integration,baseline-select,gdb-worker}.log`. Inspected captures include valid/invalid/expanded clipboard previews and English/Spanish assistant headers, Connect, failed check, backup and Ready. Fixture keys/recovery phrases are disposable, not operator credentials. The task container/build was removed after copying receipts; no pre-existing containers or builds were touched.

No full suite, broad/attested gate, sanitizer run, native/Orca session, live relay/keyring acceptance or deployment was run. The accepted panels profile-smoke baseline was not exercised or added as a gate.

## Version and delivery decision

The review changes documentation only: **no bump**. The implementation's corrections remain folded into the owner's unreleased Grotto **0.1.1**; grotto-panels is unchanged at **0.1.0**. CMake/Meson and `VERSION_MANIFEST.md:34-35` agree; the scoped diff contains no version-source, release-column or tag edits. No API/ABI, storage/wire or authority change is introduced by this report.

## Merge recommendation

**Request changes before accepting .66 as fully usable at 360 px:** cover and constrain the selected-identity label (F1), not just the unselected preview. Preserve the new header placement and import/pairing semantics. F2/F3 are explicit keyboard-review failures but inherited, not newly introduced regressions; address them as finite input/default and translation corrections or obtain an explicit scope waiver rather than presenting the assistant as keyboard-clean. Track the intermittent extra integration teardown separately (`nostrc-4iz0k`); its cause remains unresolved. No repository-wide audit, full-suite gate, new permission policy or version bump is requested.
