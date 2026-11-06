# Profile view layout: UX, behavior and boundary review

Date: 2026-11-06. Review task: `nostrc-7h1sg`; implementation: `nostrc-u6v55.59`–`.63`.

## Boundary and verdict

Reviewed exactly `git diff c03558077...ed74fc624` on `grotto/profile-view-layout`:

- Reviewed HEAD: `ed74fc6248d8b15d9c5e3a918bcedb58c28a8293`.
- Worktree throughout: `/Users/bizarro/Documents/Projects/.repoprompt-worktrees/nostrc/rp-agent-9c78bc16-grotto-profile-view-layout-14ec9361`.
- Six changed files: profile widget, panel/host layout tests, boundary import allowlist and panels Spanish/template catalogs. No product code, tests, catalogs, Blueprint, versions or policy were changed by this review. Only this report is delivered.
- Scope: Details rows, avatar/banner geometry, multiline About, provenance alignment and editor actions. No full suite or general protocol/security audit.

**Verdict:** the requested rows, overlap geometry and action layout work; Save still reaches one host-owned MANAGEMENT review and publishes the reviewed event. All six requested focused checks passed after supplying one omitted reviewer build prerequisite. **Request changes for the explicitly requested About scrolling behavior (F1), which remains broken for long text.** This is a demonstrated retained defect, not a newly introduced regression. Catalog regeneration also drops a live plural message (F2). Neither finding justifies new confirmation policy, version bumps or a wider qualification exercise.

Evidence labels: **confirmed/executed** means run in this review; **confirmed/static** means traced in source, including the production caller; **confirmed/visual** means inspected freshly rendered container screenshots. S2 denotes usability and S3 localization workflow severity, not exploit severity. File references are relative to reviewed HEAD unless explicitly marked base. Container/Xvfb/private-bus evidence is not native desktop, hosted CI, live-relay, owner acceptance or release proof.

## Findings, ranked

### F1 — S2, confirmed/executed — Requested-check failure: multiline About does not keep the caret visible

**Where:** `gnome/grotto-panels/src/grotto-profile-view.c:332-351`; enclosing page at `:288,299`. Coverage at `apps/grotto/tests/test-ui-layout.c:896-921` checks traversal and Discard but not caret visibility during long-text editing. Tracked separately as `nostrc-lapf9`.

The text view expands with its contents inside the preferences page's outer scroller. Moving its insertion point does not scroll that outer page to reveal the caret. The redesigned framing, name and Tab behavior work, but do not solve this interaction.

**Failure scenario:** at actual content width 360 px, open Edit Profile, enter a long biography, focus About, press Ctrl+End and type. A diagnostic populated 80 lines, then delivered actual X11 Ctrl+End and `a` input with `xdotool`, allowing 30 frame ticks after each operation. The text view grew to 1389 px; after typing, the caret was at window y=1755, while the page clip was y=96…710. The outer scroll adjustment remained 0. The visible page still showed the first lines, not the insertion point. Ctrl+Home returned the caret to the visible first line. This is not a Tab trap or accidental Save: Return inserted a newline, and Save calls remained zero.

**Regression attribution:** an isolated alternative library replaced only the production profile-view object with its source from `c03558077`, retaining the other built objects. The same keyboard probe reproduced the failure: caret y=1736 outside y=96…710, outer adjustment 0. Its missing placeholder and `accepts-tab=1` confirmed that the base widget, rather than the new widget, was loaded. An earlier executable-only symbol-interposition attempt did not load the base widget and is **not** counted as baseline proof. The defect predates this diff, but it directly fails the scrolling behavior the owner explicitly asked this review to verify in the touched About field.

Keep the caret visible during editing and add a focused assertion through the presented editor; do not expand this into unrelated baseline repair. The [GNOME keyboard guidance](https://developer.gnome.org/hig/guidelines/keyboard.html) requires usable keyboard interaction, not merely a focusable control. No fix was made here.

### F2 — S3, confirmed/static and executed extraction — New catalog regression: a still-used plural is removed

**Where:** deletion gap at `gnome/grotto-panels/po/grotto-panels.pot:288-294` and `gnome/grotto-panels/po/es.po:289-295`; live caller at `gnome/grotto-panels/src/grotto-identity-panel.c:174`. Tracked as `nostrc-hzc3r`.

The regenerated catalogs remove `%u app uses this identity` / `%u apps use this identity`, even though the Identity panel still calls `g_dngettext(GETTEXT_PACKAGE, …, m->app_count)`. Explicit extraction with `xgettext --language=C --keyword=_ --keyword=g_dngettext:2,3 --from-code=UTF-8` on that current source recovers the plural and both plural translation slots. Catalog syntax checks and the existing drift check pass despite this omission.

**Failure scenario:** translators working from the new template cannot translate the identity-usage count; refreshing translations from it removes the entry from the active catalog. The old Spanish plural translations were already empty, so this is **not** a newly lost completed Spanish translation or a runtime plural-selection regression. Preserve extraction of the live call while updating the task's strings. This is a small, nonblocking localization-workflow correction; no catalog was edited in this review.

## Owner-policy and regression checks

- **Profile Details / provenance (`.59`, `.62`):** `detail_row()` at `grotto-profile-view.c:234-241` consistently creates non-markup, selectable, unlimited-line `AdwActionRow` subtitles. Both Publication/Discovery rows and field details use it (`:387-391,468-472`); editor provenance binds `subtitle` to `subtitle`. The panel test checks row types/selectability/markup and the presented host layout checks the editor's rows. These replace free-standing provenance labels rather than adding a second misaligned content system.
- **Avatar/banner (`.60`):** `profile_header()` at `:242-261` reserves half an avatar below a visible banner and a whole avatar when there is none; both Identity (`:455-459`) and editor preview (`:306-309`) use it. Independent measurements below confirm the requested midpoint geometry, not just widget ancestry. No-banner headers are exactly 96/64 px high, with centred, fully allocated avatars and no vacant banner strip.
- **About (`.61`):** the field belongs to the same boxed list as Name/Display Name and has a persistent caption, 12 px row insets, 6 px text top/bottom margins, a visible empty-state placeholder and explicit accessible name `About` (`:322-350`). `GTK_A11Y=test` asserted that name. From focused Name, two actual Tab presses reached About; Tab/Shift+Tab leave it without inserting tabs. Return increased its line count from one to two without invoking Save. The placeholder is non-targetable and hidden from the accessibility tree; the field retains its explicit name. This is not a claim that the placeholder is independently announced by a screen reader. **Long-text caret scrolling fails as F1.**
- **Actions / HIG (`.63`):** Cancel is packed at logical start and Save at end (`:290-298`), with exactly one **mapped** suggested action. Eight hidden internal AdwEntryRow apply buttons also have that CSS class, but are not visible affirmative actions. Measured order reverses correctly in RTL. Window title buttons are disabled on the editor header. Republish is absent; Discard is an activatable row with an undo icon (`:396-400`), not stacked bottom buttons. This matches the [HIG action-dialog and button-order rules](https://developer.gnome.org/hig/patterns/feedback/dialogs.html#action-dialogs).
- **Cancel / Discard / Escape:** idle Cancel closes and drops the local draft; reopening starts from saved info. It does **not** prompt about unsaved changes, save them or publish them. Escape from a dirty multiline field likewise closes and drops the draft, with zero Save calls. This is the existing Cancel contract (`:209-215,269-280`), not a newly added unsaved-change safeguard. Discard resets fields, bot and image previews while keeping the editor open, and warns that already uploaded images remain public (`:222-232`). The focused tests verify dirty Cancel/reopen and keyboard activation of Discard. While busy, Cancel becomes Stop and cancels the host operation; it does not promise to recall sent bytes (`:120-122,209-212`). No new confirmation ceremony is recommended.
- **Save / MANAGEMENT:** the production path remains Save → `request(GROTTO_PROFILE_SAVE)` → `grotto_panel_profile_async()` (`profile-view.c:194-215`; `grotto-panel.c:179-197`) → injected host callback. `signer-window.c:849-865` checks lock/current `GrottoPanelsService` identity metadata and host writability before calling the host profile service. The host calls Control1 `SignProfileEvent` (`apps/grotto/src/grotto-profile-service.c:163-164`), then publishes only the signed event and waits for publication verdict (`:621-664`). Executed `signer/profile-integration` opens the actual panel editor, clicks Save, asserts MANAGEMENT/profile-management/non-rememberable review metadata (`test-profile-integration.c:78-81`), exactly one review/decision, a valid signed kind-0 event and equality between the reviewed and published event IDs/content (`:555-578`). This is real-daemon/local-relay fixture proof, not only a mocked panel callback or a live external relay run.
- **Panel boundary / approve-once:** new imports are exact GTK/libadwaita layout, buffer and accessibility symbols. No secret store, network transport, direct IPC or policy evaluator is added to the panels library. It retains the service-bound host bridge, including rejection of detached-service completion (`grotto-panel.c:167-197`), rather than moving signing authority into the widget. The no-secrets/admission and management-panel checks pass. No new always-confirm class, per-kind re-prompt or permission-policy change exists in this six-file diff.
- **Blueprint / i18n:** no `.blp` or committed `.ui` changed. Nevertheless the pinned Blueprint 0.22.2 drift check passed, including panels. All five shipped Grotto `.po` catalogs and panels `es.po` pass `msgfmt --check --check-format` and contain **zero fuzzy entries**. The `.pot` has a conventional generated fuzzy **header** at line 6; it is a template, not a shipped fuzzy translation. New placeholder/Publication/Discovery entries are translated in Spanish. F2 is an extraction omission, which syntax checks do not detect; this is not a claim of complete translation coverage.

## Geometry measured, not inferred

An isolated diagnostic linked to the rebuilt, unchanged production panels library, constructed the actual Identity panel through its public service/host interfaces, and opened its actual editor. A local synthetic 3:1 paintable supplied the banner without networking. The diagnostic asserted **actual window content widths** 360 and 798 px after compensating for libadwaita's 10 px outer inset; simply setting the default surface width had initially allocated only 350 px. Each final capture allowed 30 frame ticks for sheet/expander animations, rather than inspecting a partly opened sheet.

All combinations of light/dark × LTR/RTL × 360/798 × banner/no-banner were measured for **both** headers: 32 header allocations. Coordinates below are local to each header and were identical across theme/direction combinations. `Δ` is avatar centre minus banner bottom-edge midpoint.

| Content width | Header | Banner bounds / avatar bounds (x,y,w,h), px | Avatar centre / Δ, px |
|---|---|---|---|
| 360 | Identity | banner `(0,0,336,120)`; avatar `(120,72,96,96)` | `(168,120)` / `(0,0)` |
| 360 | Preview | banner `(0,0,308,90)`; avatar `(122,58,64,64)` | `(154,90)` / `(0,0)` |
| 798 | Identity | banner `(0,0,569,120)`; avatar `(236,72,96,96)` | `(284,120)` / `(-0.5,0)` |
| 798 | Preview | banner `(0,0,458,90)`; avatar `(197,58,64,64)` | `(229,90)` / `(0,0)` |

Thus the vertical centre is **exactly on the bottom edge** in every measured case. At the odd 569 px Identity width, integer pixel allocation places the horizontal centre half a pixel left of the mathematical midpoint; this is normal rounding, not the avatar being placed below the banner or aligned to one side. No-banner heights are respectively 96/64 px, with avatar y=0. Inspected final light/LTR and dark/RTL narrow/wide captures show the intended overlap and no-banner fallback. These are GTK widget bounds and local fixture pixels, not downloaded image content or native desktop screenshots.

## Verification actually run

Linux image: `nostrc-linux-ci:arm64`, image ID `sha256:3d1c8f2383c7159a44e3c8f1714476ca749333d34c887b780d6efc144eab35e2`; GTK 4.14.5 / libadwaita 1.5.0. Source was mounted read-only and copied into a new task-owned volume; only the copied build/diagnostic outputs were writable. `xdotool` was installed in the disposable container, not on the developer machine. Builds used the libsecret backend and tests used isolated homes/private buses; no developer keychain command was run.

| Command/check | Actual result |
|---|---|
| `scripts/install-hooks.sh` | PASS; static-only pre-push hook already installed |
| Configure Debug with Grotto/panels/signer tests enabled, Groundhog off; `cmake --build /work/build --parallel 3 --target test-grotto-panels-smoke test-ui-layout test-profile-integration` | PASS; focused targets and dependencies built from the reviewed source copy |
| `xvfb-run -a ctest --test-dir /work/build --parallel 1 -R "^(grotto-panels/(profile-panel\|management-panels\|no-secrets)\|signer/(ui-layout\|ui-accessibility\|profile-integration))$" --output-on-failure` | Initial command **exit 8**, 6/7 including the homes fixture: profile-panel, management-panels, profile-integration, ui-layout, ui-accessibility and fixture passed; no-secrets could not run its missing admission executable. Total 134.39 s |
| Build omitted `test-grotto-panels-admission`, then `ctest --test-dir /work/build --parallel 1 -R "^grotto-panels/no-secrets$" --output-on-failure` | PASS, 1/1, 3.98 s. Initial failure was reviewer provisioning, not a code regression; no test was skipped, edited or weakened |
| Independent settled widget/keyboard/geometry probe (`probe-settled.c`) | Geometry, name, traversal, Return, button order, Escape/reopen assertions PASS; measured caret data exposes F1. The probe's exit status is **not** a pass for long-text scrolling |
| Settled long-text probe against current library and isolated base-profile library | F1 reproduced on both. Base library differs only in the profile-view object; this is diagnostic baseline comparison, not full base-suite execution |
| `python3 apps/grotto/tests/test-blueprint-drift.py apps/grotto` | PASS; all production templates match pinned 0.22.2; kind catalog matches registry |
| `msgfmt --check --check-format -o /dev/null <catalog>` and fuzzy-entry enumeration for six shipped `.po` files | PASS; zero fuzzy entries in each `.po`; template-header caveat above |
| Explicit `g_dngettext:2,3` extraction from current Identity source | Recovers missing plural; confirms F2 |
| `git diff --check c03558077...ed74fc624` | PASS |

All six requested checks therefore have passing executions, but the first combined CTest invocation is retained as failed, not relabelled green. No full suite, broad gate, sanitizer suite, native desktop/Orca, external relay/keyring, hosted CI or release acceptance was run. `grotto-panels/smoke-host` was deliberately **not executed**: `nostrc-qdgge` is the owner's accepted baseline, not a finding from this review. Building the smoke executable supplies its two focused test paths; it is not a smoke-host execution claim.

Local evidence: `/tmp/grotto-profile-layout-review-20261106/` contains `configure.log`, `build.log`, `focused.log`, `focused.exit`, `build-admission.log`, `no-secrets-equipped.log`, `catalogs.log`, `blueprint.log`, extraction output, diagnostic C sources/build commands, `probe-settled.log`, `scroll-probe.log`, `scroll-base-library.log`, caret measurements and screenshots. Earlier non-root apt failure, diagnostic include-path failures, the 350 px allocation assertion and the overly broad count of hidden suggested buttons are retained too; they are harness corrections, not product defects. The first short-frame captures were replaced by settled captures, not used as final visual proof. The task container/volume are removed after evidence export; pre-existing containers, volumes and worktrees are untouched. Beads synchronization is unavailable because its configured remote list is empty; no remote was invented. Jev was used only to locate source ranges: 14 requests / 139780 input tokens / approximately USD 0.00587; findings were checked directly and executed, not delegated to its judgments.

## Version and delivery decision

This report is documentation-only: **no bump** for any component. The requested panels presentation corrections remain folded into unreleased **0.1.0**; Grotto changes in the scoped diff are test-only and it stays **0.1.1**, as explicitly directed. `VERSION_MANIFEST.md:34-35` and the CMake/Meson version sources agree. No public secret/policy API, storage/wire format, release column or tag is changed by this review. Only this report is committed and pushed on the existing branch; the assigned worktree/branch is retained for its coordinator, not merged or deleted.

## Merge recommendation

**Request changes on the named About scrolling requirement before accepting `.61` as fully verified.** Keep the framed field, corrected Tab behavior, rows, midpoint overlap and single Save action. Address caret visibility with a focused editor-level check (F1); its baseline provenance must remain explicit. Preserve the live plural during catalog regeneration (F2, nonblocking). Do not reopen the accepted smoke-host baseline, add an always-confirm class, add an unsaved-change confirmation merely for ceremony, bump the owner's frozen unreleased versions or turn this into a full-suite exercise. Everything else in the requested layout/action boundary has the focused evidence above. No fix was made in this review.

## Re-check (16bf21139)

Date: 2026-11-06. Re-check task: `nostrc-u5vb1`. `git pull --ff-only` confirmed the clean assigned branch at `16bf21139eb728461b907678e5b9b09e9eea56d6`. Reviewed only its five-file remediation diff against `84a5bd7c2`, the original findings and related placeholder/fuzzy-header nits. No product source, tests, catalogs, versions or policy were changed by this re-check; only this section is appended.

### F1 — Partially addressed; caret visibility still fails

**Where:** `gnome/grotto-panels/src/grotto-profile-view.c:262-291,367-368`. The new coalesced, weak-widget-bound `after-paint` callback clamps the existing page adjustment; there is no nested scroller or timer-based polling. It fixes the original Ctrl+End/typing failure, but does not reliably preserve visibility after newline insertion or keyboard focus re-entry.

An independent diagnostic linked to the rebuilt production panels library opened the actual Identity panel editor through its existing public service/host bridge. At actual content widths **360 and 798 px**, it reached About from Name with two real Tab presses, populated the same 80-line biography, and delivered real `xdotool` input. Each operation allowed **30 frame ticks**, including after Return and focus re-entry. Coordinates below include the whole caret height, against the actual page viewport clip, not merely the text view's much taller allocation.

| Operation | 360 px: caret y / page clip, px | 798 px: caret y / page clip, px | Result |
|---|---|---|---|
| Ctrl+End, then type `a` | `687…704` / `96…710` | `657…674` / `76…680` | PASS; outer adjustment now follows the caret |
| Return at the end | `698…715` / `96…710` | `668…685` / `76…680` | FAIL; bottom 5 px clipped at both widths |
| Ctrl+Home | `102…119` / `96…710` | `82…99` / `76…680` | PASS |
| Ctrl+End, Tab out, Shift+Tab back | `1479…1496` / `96…710` | `1459…1476` / `76…680` | FAIL; TextView regains focus, but the end caret is offscreen |

**Failure scenario:** edit a long About, move to its end, Tab to the next control, then Shift+Tab back. GTK root focus is again the About `GtkTextView`, but the outer adjustment has returned to 293 and the retained insertion point lies hundreds of pixels below the visible page. This is still the requested caret/focus behavior, not a new audit scope or a new confirmation requirement. `nostrc-lapf9` is reopened with the measured residuals. The precise callback/layout ordering cause was not established; this report does not claim one.

The committed tests pass, including their immediate Return caret assertion (`apps/grotto/tests/test-ui-layout.c:945`). Their key helper waits three frames, and their Tab checks separately grab About before exiting forward/backward (`:954-962`), rather than checking visibility after an actual Tab-out/Shift+Tab-back round trip with the caret at the end. The independent settled-frame diagnostic therefore supplies additional failure evidence, not grounds to relabel those passing tests as failed.

**Other requested behavior passes:** exactly one `GtkScrolledWindow` ancestor exists between About and its preferences page. Wheel input over About moves that outer adjustment from 293 to 365.2 at 360 px, and to 364.5 at 798 px; the TextView's internal vertical adjustment stays **0**. There is no demonstrated nested-scroll trap. Tab/Shift+Tab can enter/leave the field. `GTK_A11Y=test` verifies both accessible name `About` and accessible placeholder `Write something about yourself…`, matching the visible empty-state placeholder (`grotto-profile-view.c:381-382`); the Spanish host accessibility test also passes. Return inserts a newline without invoking Save. Dirty Escape closes without publishing, and reopening restores the saved empty About. These successful checks do not cancel the caret-visibility failures above.

### F2 and related nits — Resolved

The live identity-usage singular/plural is restored in `gnome/grotto-panels/po/grotto-panels.pot:292-297` and translated in `po/es.po:293-298`. Spanish declares `nplurals=2; plural=(n != 1)` (`po/es.po:15`). Compiled `GNUTranslations.ngettext` returns `1 aplicación usa esta identidad` and `2 aplicaciones usan esta identidad`; explicit `g_dngettext:2,3` extraction recovers the same source-live messages and both slots. `msgcmp --use-untranslated` confirms panels Spanish covers the template. All six shipped `.po` files pass `msgfmt --check --check-format`; there are **no fuzzy entries in any of them or in the panels `.pot`, including its header**. This resolves the original template-header caveat as well as F2 (`nostrc-hzc3r`). The accessible-placeholder nit is resolved by the explicit field property, not by making the decorative overlay label separately accessible. No other nits were left outstanding by the original report.

### Verification rerun and limits

A fresh task-owned container/volume used the same `nostrc-linux-ci:arm64` image ID recorded above, GTK 4.14.5/libadwaita 1.5.0, read-only source mount, private copied build, libsecret backend and isolated test homes/buses. `xdotool` was installed in that container only. All four prerequisites were built **before** execution: `test-grotto-panels-smoke`, `test-grotto-panels-admission`, `test-ui-layout`, `test-profile-integration`. The five remediation input files were byte-compared with this worktree after execution; all matched.

The original six-check CTest regex shown above was rerun with `--parallel 1 --output-on-failure`: **7/7 PASS including the required homes fixture, exit 0, 138.97 s**. Individual results: profile-panel 2.18 s; management-panels 0.26 s; no-secrets 4.29 s; profile-integration 12.40 s; ui-layout 59.79 s; ui-accessibility 59.99 s. There was no provisioning failure, skip or retry of this suite. The real-daemon/local-relay integration again passed its one-review, reviewed-versus-published event assertions; its fixture kind-0 event ID was `63b83ced480574bdd5f0905770d31046e269227434732b71b311e65a5eff2a80`.

The independent strict caret probe **failed with exit 134** after Return at 360 px. A separate continuation diagnostic retained the checks and collected both widths: **exit 1, four caret-visibility failures**, with wheel, traversal, accessible properties, Return/no-Save and Escape/reopen checks passing. Neither diagnostic is presented as green. The pinned Blueprint 0.22.2 drift check and `git diff --check 84a5bd7c2..16bf21139` also pass. No `.blp` changed, and the unchanged layout/action/service boundaries do not acquire new policy, secrets or an always-confirm class. Versions remain panels **0.1.0** / Grotto **0.1.1**; this documentation-only appendix requires no bump.

Evidence is retained at `/tmp/grotto-profile-layout-recheck-16bf21139/`: `focused.log`, `focused.exit`, `LastTest-focused.log`, build/configuration/dependency logs, `catalogs.log`, `static-extras.log`, diagnostic sources, `probe.log`/`.exit` and `probe-continue.log`/`.exit`. The initial diagnostic assertion failure is preserved separately from the continuation run. Only task-owned container/volume resources are removed after export; the assigned branch/worktree and sibling resources are retained. Tracker remote synchronization is still unavailable because no Beads remote is configured. No full suite, accepted-baseline smoke-host, native desktop/Orca, hosted CI, external relay or release acceptance was run or claimed.

**Final verdict: request changes; F1 is only partially resolved.** Keep the successful single-scroller fix and accessible placeholder, but make the caret remain visible after settled newline layout and keyboard focus re-entry, with focused coverage of those two cases. F2 and the catalog/placeholder nits are resolved. The six serial container checks are green; they do not override the independently reproduced residual failure. No fix was made in this re-check.
