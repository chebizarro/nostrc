# W4 shell panels: UX/HIG and consent-boundary review

**Date:** 2026-11-04
**Compared:** local `master` `8f8112334b2b42d127d75832e6541843a73b39b0` → pushed `origin/grotto/w4-shell-panels` / worktree HEAD `1dcc4e32bace0b33bafdd00d53e94ddcd886d808`. No fetch was performed.
**Worktree:** `/Users/bizarro/Documents/Projects/.repoprompt-worktrees/nostrc/rp-agent-f39db6db-grotto-w4-shell-panels-c16ff847`
**Tracking:** `nostrc-kd1in`.

## Scope, method and verdict

Six requested lenses only, against `docs/designs/grotto-settings-style-ux-2026-11-03.md` §§b.3–b.4, d.4–d.6, f and resolved Q1–Q4, plus the corrected permission model in `docs/investigations/grotto-approval-persistence-and-gates-2026-10-25.md:485–505`. Historical always-confirm conclusions do not override the correction. Q3 means retain the existing lock, not implement the superseded lock redesign.

**Verdict:** the Settings-style shell and trusted-host split are substantially wired, and the images genuinely depict that shell. However, W4 is not yet equivalent to G15's Permissions surface: a real migration decision is rejected, and effective-access presentation and bulk clearing are lost. The affected-test inventory also retains the deleted dashboard target/name.

**Confirmed** means directly supported by source or inspected committed images, not reproduced execution. **Plausible** identifies an unproven user-visible risk. P1 = high, P2 = medium, P3 = low. No implementation edits, builds, tests, screenshot regeneration, installed GNOME session, commit, push or merge were performed. The only repository-file change is this review. Hook installation was checked; the existing static hook was already installed. Review-only documentation changes require **no component version bump**.

## Ranked findings

### 1. P1 — A migrated `policy-review` entry makes the entire Permissions read fail

**Confirmed static regression; prerequisite:** a client has at least one G15-migrated dormant grant with `decision="policy-review"`.

- The daemon intentionally returns the stored decision unchanged, including `policy-review`, with `state="inactive"` / `reason="Pending policy review"`: `nips/nip55l/src/glib/signer_permissions.c:662–696`. This is the corrected model's migration state, not malformed input.
- The new Control1 query adapter accepts only `allow`, `deny` and `review`; anything else rejects the **whole model** as an invalid snapshot: `apps/grotto/src/grotto-panels-control1-service.c:334–350`.
- The UI then discards its permission model, empties the rows and shows “Permissions could not be loaded”: `gnome/grotto-panels/src/grotto-app-detail.c:357–382`. Refresh cannot fix a valid but consistently rejected decision. Other saved grants/denials for that app also become unavailable for inspection or per-row revocation through this page.
- The fixture uses `review`, not `policy-review`: `apps/grotto/tests/test-ui-layout.c:664–667`; the substantial editor tests at `:473–519` therefore miss the G15 migration path.

This fails closed; it is **not** evidence of a grant being activated or authorization bypassed. Preserve both daemon review states through the projection and demonstrate the inactive/needs-review presentation with an actual `policy-review` row, including a neighboring ordinary grant. Do not translate the migration state into an active allow.

### 2. P2 — “Clear remembered permissions” is no longer reachable

**Confirmed static regression.** The replacement Permissions page creates only the saved-row group; no scoped clear action is added (`gnome/grotto-panels/src/grotto-app-detail.c:397–411`). It offers individual-row removal (`:279–335`), but not G15's distinct bulk-clear operation.

Master supplied per-identity clear buttons (`master:apps/grotto/src/ui/page-client-detail.c:442–445`) and a confirmation explaining that explicit allows **and denies** are removed while the trust tier stays unchanged (`master:apps/grotto/src/ui/page-client-detail.c:129–142`). “Remove App…” is not an equivalent substitute: it revokes pairing and pending work too (`gnome/grotto-panels/src/grotto-app-detail.c:119–126`). Restore the scoped operation through the approver host, with its existing confirmation/revision semantics; do not repurpose app removal.

### 3. P2 — Permissions no longer distinguishes effective preset access from explicit saved rows

**Confirmed static regression.** `gnome/grotto-panels/src/grotto-app-detail.c:397–411` builds only “Saved Permissions”; `:364–378` renders `ListPermissions` records. That daemon API enumerates persisted records, not tier-derived access (`nips/nip55l/src/glib/signer_permissions.c:685–696`).

Master separated “Effective from Trust Level” and “Remembered Permissions” and explained precedence (`master:apps/grotto/src/ui/page-client-detail.c:158–166, 401–459`). In W4 a routine-access app with no explicit records has a blank saved-row area, despite still having preset access. The Access radio group one page back (`gnome/grotto-panels/src/grotto-app-detail.c:477–486`) does not preserve the requested Permissions-page distinction or explain the actual preset scope there.

Preserve the distinction and precedence explanation using daemon/host-provided facts. Reintroducing a GTK permission evaluator is not the remedy. For rows that are admitted, raw daemon `state`/`reason` and expiry are still displayed (`:366–375`); the problem is not a blanket loss of all inactive markers.

### 4. P2 — The affected-test inventory still selects the removed Home test and build target

**Confirmed static integration mismatch; execution consequence not run.**

- CMake now registers `signer/panel-inbox-model` / `test-panel-inbox-model` (`apps/grotto/tests/CMakeLists.txt:591–596`); the component inventory was renamed too (`apps/grotto/tests/TestInventory.cmake:52`).
- The committed execution policy still selects `signer/home-model` in its private-bus suite, including the Grotto sanitizer profile (`tests/selection/inventory.json:19921–19949`), and still requests the deleted `test-home-dashboard` build target (`:19985–19998`). Neither replacement name occurs in that file.
- This is live policy, not merely historical prose: `scripts/test-gate.sh:554–569` loads it to construct affected/broad execution plans.

When that suite is selected, it cannot provide the renamed test coverage as authored and requests a nonexistent build target. No failed build or silently skipped run is claimed here; the name/target mismatch itself is confirmed. Update the supported inventory/selection registrations together. This is exactly §f.2's “never leave a stale selected test name,” not a request for a selector-wide audit.

### 5. P3 — New management panels lack the focused RTL/keyboard coverage implied by the accessibility lane

**Confirmed coverage gap; plausible layout/focus risk, not a demonstrated RTL product defect.** `signer/ui-accessibility` runs the same real-window test with Spanish and the test accessibility backend (`apps/grotto/tests/CMakeLists.txt:626–628`). Its explicit RTL/keyboard Back checks cover Identity and Backup, then reset to LTR before visiting the W4 panels (`apps/grotto/tests/test-ui-layout.c:397–433`). Apps/detail/Permissions activation and editing mostly use emitted signals (`:436–459`); Activity/Remote Signing get size/capture checks (`:565–574`), not equivalent RTL/keyboard journeys.

Retain credit for the existing names, Spanish, focus bounds and Identity/Backup tests. The narrow missing evidence is keyboard Back and readable long/RTL content through Connected Apps → app → Permissions, and the new Activity/Remote controls. This does not require a new platform matrix, full translation programme or W5/W6 work.

### 6. P3 — Several regenerated screenshots contain a stale permissions-success toast

**Confirmed visual artifact issue.** Inspected committed `apps/grotto/data/screenshots/inbox-dark.png`, `inbox-360-light.png`, `permissions-light.png` and `activity-light.png` contain “Saved. Existing permissions are retained when access is lowered.” On narrow inbox and Activity captures it overlaps lower controls; it is unrelated to the surface being illustrated.

Source: the app-detail completion adds that toast (`gnome/grotto-panels/src/grotto-app-detail.c:71–79`); the layout test changes the tier before Permissions/Activity captures (`apps/grotto/tests/test-ui-layout.c:444–450, 565–574`). This is not a finding that the images are old Home screenshots—they visibly show the new shell. Capture a settled production state after the transient notification has completed, without editing the PNG or suppressing product feedback. The screenshot bytes, not a recreated run, are the evidence here.

## Six-lens assessment and preserved behavior

### 1. Consent ownership and G15 semantics

**No approver bypass found in the traced W4 UI paths.** The shared panel dispatch treats only cosmetic client-label editing as manager-class; security commands go to host hooks, not the ordinary read-service route (`gnome/grotto-panels/src/grotto-panels-service.c:73–77`; `gnome/grotto-panels/src/grotto-panel.c:105–145`). Grotto installs these hooks and checks the retained management lock and writable control model (`apps/grotto/src/ui/signer-window.c:821–858`). This is a GUI boundary; the daemon must still authorize each caller. No manager-role implementation or claim of W6 completion is inferred.

The real Grotto composite service supports cosmetic labels without pretending the not-yet-present manager capability map authorizes security changes (`apps/grotto/src/grotto-identity-service.c:27–31, 96–103`). Security RPC arguments remain a closed typed mapping, including `SetClientTrust`, `SetPermissions`, `RevokeClient`, `ConfigureBunker` and `ResetBunkerEndpoint` (`apps/grotto/src/grotto-panels-control1-service.c:22–29, 237–300`).

- **Tier picker:** the three G15 choices remain; old Full policy is not silently selected/upgraded, and the explicit upgrade dialog leaves saved grants/blocks intact (`gnome/grotto-panels/src/grotto-app-detail.c:99–113, 441–445, 477–486`). Lowering access explicitly says that saved permissions remain. A tier edit does not decide the open request.
- **Permissions/Conflict:** revision and daemon-owner generation accompany edits; errors refresh and require an explicit retry; a changed original entry blocks the editor (`gnome/grotto-panels/src/grotto-app-detail.c:182–249, 340–352`). Scope/identity selection, exact-kind/custom-number editing, durations, per-row removal, preserved concurrent denials and independent AUTH relays have meaningful real-widget/private-bus tests (`apps/grotto/tests/test-ui-layout.c:459–563`). Findings 1–3 are the missing G15 behavior.
- **Pairing:** Add App uses the trusted host callback and existing Grotto Connect widget (`apps/grotto/src/ui/signer-window.c:803–814`); URI input stays in Grotto, checks setup/management lock, and enters the existing pairing presenter, not the shared command enum (`apps/grotto/src/main_app.c:691–713`). This review does not requalify the underlying relay transport.
- **Pending:** real Grotto hosts the application-owned controller's inbox. Rows and count use that same pending model (`apps/grotto/src/ui/signer-window.c:626–654`). A row calls review only (`apps/grotto/src/approval-controller.c:1129–1167`); the controller instantiates the W1 trusted dialog and wires its explicit decision handlers (`:1085–1125`). No inbox Allow/Deny action is introduced. The generic shared inbox likewise calls only its host review hook (`gnome/grotto-panels/src/grotto-apps-panel.c:42–44, 73–86`).

### 2. Home retirement, routes and window chrome

**Home UI is retired, not merely hidden.** The comparison deletes `page-home.c/.h`, `page-home.blp/.ui` and `test-home-dashboard.c`; only Identity, Connected Apps, Activity and Remote Signing descriptors are mounted (`gnome/grotto-panels/src/grotto-apps-panel.c:23–32`; `apps/grotto/src/ui/signer-window.c:842–868`). No stat tiles, Home avatar strip or management-selection UI remains in that shell. Keeping `home-model.c` as an adapter and old daemon counters is not itself retaining the Home surface. Finding 4 covers the surviving executable/selected-test references.

| Entrypoint | Traced destination / behavior |
|---|---|
| Ordinary launch | Identity selected initially (`apps/grotto/src/ui/signer-window.c:616–620`); first successful pending reconciliation chooses Apps when nonempty, but later arrivals do not steal the current page (`:636–654`). |
| `app.review-request` with `""` | `present_pending()` opens Apps, then requests reconciliation; no arbitrary replacement ID is substituted (`apps/grotto/src/main_app.c:217–277`; `apps/grotto/src/approval-controller.c:337–341, 1057–1062`). |
| Exact/stale request ID | Review targets that ID; stale notices return to Apps and show the stale-action message (`apps/grotto/src/main_app.c:244–277`). Existing integration assertions were adapted, not removed (`apps/grotto/tests/test-approval-integration.c:435–451, 605–619`). |
| `--preferences` / hidden `--settings`; desktop `settings` | Both CLI options select Preferences (`apps/grotto/src/main_app.c:716–723`); action aliases share the handler (`:853–859`); the window presents `AdwPreferencesDialog` (`apps/grotto/src/ui/signer-window.c:794–802`). |
| `--new-identity` / app action | CLI activates the existing creation action, not management selection (`apps/grotto/src/main_app.c:719–720, 853`). Real-app test asserts Identity plus one reused onboarding assistant (`apps/grotto/tests/test-desktop-activation.c:159–190`). |
| `bunker:` / `nostrconnect:` opens | Trusted `on_open` → `open_pairing_uri` → pairing presenter (`apps/grotto/src/main_app.c:691–713`). Real activation tests open both schemes and close their dialog without a second management window (`apps/grotto/tests/test-desktop-activation.c:200–218`). |
| Shell indicator | Still sends `review-request` with one empty string argument (`gnome/nostr-shell-extension/lib/signer.js:185`), matching the retained action signature. |
| Internal aliases | `home` → Identity; `connect` → Apps; `settings`/`preferences` → dialog (`apps/grotto/src/ui/signer-window.c:884–910`). `win.connect` invokes Add App through its host (`:315–318`); Activity remains a panel route. |

No new dead end was found in those traced routes. This is not a claim that the reviewer executed them. The template has no header-bearing content wrapper: the sidebar owns its header and panels own theirs (`apps/grotto/data/ui/signer-window.blp:13–39`). Inspected wide/narrow screenshots show one appropriate titlebar/control set, not duplicate window chrome.

### 3. HIG / Settings idiom, preferences and lock

The shell uses `AdwNavigationSplitView` plus `AdwBreakpoint` at **600sp**, so it is collapsed at 360px; it does not wrongly wait until below 360px (`apps/grotto/data/ui/signer-window.blp:4–14`). This matches the [documented split-view/breakpoint pattern](https://gnome.pages.gitlab.gnome.org/libadwaita/doc/1.5/class.NavigationSplitView.html) and [HIG sidebar idiom](https://developer.gnome.org/hig/patterns/nav/sidebars.html). The actual layout test asserts collapse, selection and visible focus at 360 (`apps/grotto/tests/test-ui-layout.c:328–343`).

Boxed preference rows and adaptive dialogs replace the old dashboard. Shell/page/action titles use header capitalization while the unchanged tier-radio vocabulary remains sentence case, consistent with the design's Q4 and [HIG writing guidance](https://developer.gnome.org/hig/guidelines/writing-style.html). No custom color/high-contrast dependency is retained: both CSS files and their resources are removed in the comparison. Native semantic destructive/suggested styles are not custom colors.

The floor remains GTK 4.14 / libadwaita 1.5 in Grotto's CMake/Meson (`apps/grotto/CMakeLists.txt:29–34`; `apps/grotto/meson.build:30–34`) and the shared module has matching min/max API pins (`gnome/grotto-panels/CMakeLists.txt:10–13`). No newer-only widget was identified in the W4 templates; no floor build was executed here.

Accessible names exist for the shell, menu, sidebar, pending count and tier controls (`apps/grotto/data/ui/signer-window.blp:6, 27, 36`; `apps/grotto/src/ui/signer-window.c:633–634`; `gnome/grotto-panels/src/grotto-app-detail.c:480–486`). Preferences remains small, follows the system without appearance/language rows, and uses the existing session-manager timeout/lock bindings (`apps/grotto/src/ui/page-settings.c:22–26, 125–137`). The original lock-screen widget remains the locked stack child (`apps/grotto/data/ui/signer-window.blp:9–10`). The test explicitly asserts no Appearance/Language rows (`apps/grotto/tests/test-ui-layout.c:317–322`). See finding 5 for the limits of the accessibility/RTL evidence.

### 4. Remote Signing honesty

**Confirmed daemon provenance:** the service projects `GetStatus`'s `bunker_state`, `bunker_enabled`, relays and methods separately (`apps/grotto/src/grotto-panels-control1-service.c:119–140`). The daemon state mapping, not GTK, supplies `LISTENING` / `DEGRADED` / other values (`apps/grotto/daemon/grotto_control.c:92–103, 180`). The panel maps these to labels—`LISTENING` is “Ready”—and does not infer readiness from the enable switch (`gnome/grotto-panels/src/grotto-remote-panel.c:18–22, 100–119`).

The UI describes requests from remote apps and explicit endpoint relays. It does not claim publication, delivery, anonymity, universal NIP-46 compatibility, or a new bypass of the §5.9 runtime definition (`docs/plans/grotto-signer-ux-2026-10-12.md:280–296`). This review traces the supplied status; it does not independently prove EOSE/transport correctness or re-audit the daemon.

Reset Endpoint has a destructive, cancel-default confirmation explaining that remote pairings/links stop working while identities/local permissions remain. Only the affirmative response invokes the trusted host reset command (`gnome/grotto-panels/src/grotto-remote-panel.c:78–93`). No missing-confirmation defect found. The new management smoke test checks relay configuration but does not test reset cancel/confirm or all runtime-state labels (`gnome/grotto-panels/tests/test-management-panels.c:81–101`); that is a focused coverage limit, not proof of broken reset.

### 5. Screenshots, metainfo and named checks

Inspected **eight committed PNGs**: `identity-light`, `apps-light`, `activity-light`, `inbox-dark`, `inbox-360-light`, `permissions-light`, `remote-signing-360-light`, and `settings-light`. They visibly show the new sidebar/collapsed shell, inbox, shared panels and reduced Preferences. Git's comparison shows new Identity/apps/activity/inbox/remote captures and changed Client/Permissions/Preferences images; old Home/connect images are deleted. Thus the images are not simply old screenshots renamed. Their generation process was not rerun or independently attested here.

Metainfo's default is now Identity, adds the inbox and retains Permissions/Preferences (`apps/grotto/data/org.nostr.Grotto.metainfo.xml:51–70`). The provenance explicitly calls these private-bus fixtures on Ubuntu 24.04 arm64 / GTK 4.14.5 / Adw 1.5.0, **not** real-browser, live-relay, installed Shell/Orca or release evidence (`apps/grotto/data/screenshots/README.md:3–10, 25–29`). The stated generation environment is the branch author's claim, not reviewer execution. Its master-hosted URLs become current after integration, not just this branch push. Finding 6 is the visual-quality caveat.

| Named check | Static assessment; none run by this reviewer |
|---|---|
| `signer/ui-layout` / `signer/ui-accessibility` | Meaningful real production window/shared panels with a private fixture bus, narrow collapse, route/title/focus checks, permission Conflict/retry/migration editor tests and generated captures (`apps/grotto/tests/test-ui-layout.c:270–628`). Not just a mock widget assertion; not a live signer/desktop acceptance run. |
| `signer/panel-inbox-model` | Registered at `apps/grotto/tests/CMakeLists.txt:591–596`. Re-homes the old signal/owner-loss/failure/version-skew cases and adds shared adapter rows, deadline and read-only trust capability checks (`apps/grotto/tests/test-panel-inbox-model.c:54–85`). Shared adapter assertions do not cover permission-query migration. Finding 4 concerns the outer selector inventory, not absence of this CTest. |
| `signer/desktop-contract` | Checks desktop actions/CLI help, metainfo default/local PNG signatures and removed Home captures, then invokes the real-app activation worker on Linux (`apps/grotto/tests/test-desktop-contract.py:26–36, 59–65, 109–117`). Worker exercises aliases, empty review and both pairing schemes (`apps/grotto/tests/test-desktop-activation.c:153–218, 294–305, 331`). |
| `signer/blueprint-drift` | Pinned compiler 0.22.2, resource/build inventory checks, byte comparison and current shared-module resources (`apps/grotto/tests/test-blueprint-drift.py:13–35, 61–92`). Library-template coverage already existed and includes the new resource list; it is meaningful, but registration is conditional on the pinned compiler (`apps/grotto/tests/CMakeLists.txt:669`). No compiler was run in this review. |

### 6. Coverage against §f.2: credit and remaining gaps

- **Preserved, not missing:** production pending reachability, empty review action, notifications-unavailable banner, Not Now/no re-notification and inbox activation into the trusted dialog are exercised in the adapted integration test (`apps/grotto/tests/test-approval-integration.c:370–451`). Stale-action assertions are retained (`:605–619`). Removing Home did not remove those tests or replace inbox activation with a decision.
- **Preserved:** Identity inventory/metadata coverage remains separate from management selection; layout tests assert that inspecting another identity does not change the default (`apps/grotto/tests/test-ui-layout.c:294–316`). The new editor tests retain meaningful conflict and scope cases instead of weakening all assertions.
- **Missing from the inspected W4 coverage:** the actual `policy-review` decision (finding 1); effective-versus-explicit sections and per-identity bulk clearing (findings 2–3); the correct renamed test in the active execution inventory (finding 4); new-panel RTL/keyboard journeys (finding 5); Remote reset cancel/confirm and distinct degraded/offline/error presentation.
- The old CSS-class-only high-contrast tests were deleted with the CSS. Existing light/dark captures do not establish a system high-contrast or screen-reader result. This is a stated evidence limit, not a demand to reinstate custom CSS or fake class tests.
- W1 dialog redesign, W5 remaining dialogs and W6 Nostr Settings real-host mounting are not new W4 implementation obligations. Existing approval/repeat-permission tests should remain intact, but no passing results are inferred from their presence or from a closed implementation bead. No owner walkthrough or GNOME 46 acceptance was performed in this review.

## Final verification boundary

Performed: pinned-ref/status/diff inspection, focused source/registration tracing, current GNOME HIG/API lookup and visual inspection of committed images. Relevance filtering used Jev `find-lines` with focused follow-up reads; low-confidence broad queries were narrowed before drawing conclusions. No retained plan, build, CTest result, hosted CI receipt, screenshot-generation run or native acceptance receipt was produced. The findings distinguish confirmed source/artifact defects from plausible uncovered UI risks. No broader repository audit or remediation is included.
