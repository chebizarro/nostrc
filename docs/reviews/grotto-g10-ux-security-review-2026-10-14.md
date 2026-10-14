# G10 UX-contract / security review — 2026-10-14

## Context / Scope

**Verdict: REQUEST CHANGES.** No confirmed private-key/pairing-token disclosure or silent persistent-grant creation was found in G10's normal Home/client/Connect rendering paths. The remaining blockers are UX-contract correctness, keyboard accessibility and incomplete cleanup, not a demonstrated authorization bypass.

- Requested baseline: `0988d81565bf545af8c31ebecb2a46df029c9a7c`.
- Reviewed pushed tip/worktree HEAD: `3f80ac8191722fb5588a3b4af6883a76d069489f` (`origin/grotto/g10-home-ia`).
- First-parent range contains integration merge `0cda349ab` and **one authored G10 commit, `3f80ac819`**. Attribution uses `git diff 3f80ac819^ 3f80ac819`, not the combined baseline-to-tip diff. G11 `823925632`/`295a029b4` and the integration merge are excluded. G13 is already in the requested baseline (`b9842a32b`, `673ed12a1`, `0988d8156`); no Shell findings are included.
- Contract: plan §5.1, §5.4, §5.11, §6.3 and the three requested §9 rows. Unchanged dependency code is cited only to establish behavior of G10's callers.
- Paths below are relative to this worktree. Code remained unmodified; this report is the only added worktree file. No tracker updates, commits, pushes or hook installation were requested/performed. Documentation-only deliverable: **no component version bump**.
- “Confirmed” means directly established by source or the stated runtime check; “plausible” means a user-visible consequence still requiring acceptance testing.

## Findings — severity ordered

### F1 — Medium / Confirmed: the stock-GTK workaround drops keyboard focus across a breakpoint

**Evidence:** `apps/grotto/src/ui/signer-window.c:60-72`, `:632-642`; `apps/grotto/tests/test-ui-layout.c:134-141`.

The version guard is real: focus clearing runs only on GTK 4 below 4.23.1. However, a breakpoint-region change clears the window focus without restoring the still-valid control or selecting a sensible replacement. Root navigation also clears it even when the requested page is already visible. This does not meet “no focus loss visible to users.”

**Runtime proof:** on stock Ubuntu GTK **4.14.5**, using the real layout fixture and unchanged production objects, a temporary copy of the test added one assertion immediately after its 920→760 resize/frame barrier, before the existing re-focus call. It printed `focus preserved=0, focus null=1` and failed. The shipped test passes because line 137 explicitly grabs focus again; only the subsequent same-region 760→740 resize asserts retention. Temporary harness/log: `/tmp/grotto-g10-review-focus.py`, `/tmp/grotto-g10-review-focus.log`. The source/build volume was read-only for this counterexample.

The inline upstream reference is accurate: [GTK commit a6e1c8a57e523fcf734d5b81a5e1d49c70457f4b](https://github.com/GNOME/gtk/commit/a6e1c8a57e523fcf734d5b81a5e1d49c70457f4b) replaces an overwritten owned deferred-focus reference. Its old assignment exists in [4.23.0](https://raw.githubusercontent.com/GNOME/gtk/4.23.0/gtk/gtkwindow.c), and the replacement exists in [4.23.1](https://raw.githubusercontent.com/GNOME/gtk/4.23.1/gtk/gtkwindow.c). The workaround body is a no-op on those fixed version numbers; fixed-runtime keyboard acceptance was not run.

**Recommendation:** retain the version gate and unconditional finalization checks, but preserve/restore valid focus after actual transitions, use a deliberate destination when the old control disappears, and avoid clearing for no-op navigation. Assert post-transition focus without manually re-grabbing it, including both breakpoints and a fixed GTK runtime.

### F2 — Medium / Confirmed: permission Conflict feedback is behind the page the user is viewing

**Evidence:** `apps/grotto/src/ui/page-client-detail.c:41-47`, `:110-136`, `:195-196`, `:291-315`; `apps/grotto/data/ui/page-client-detail.blp:10-16`.

Revision-bearing writes and re-reading are present, so this is **not** a lost-update bypass. But the editor closes immediately after submitting. All mutation failures, including typed Conflict, become the same generic message in `self->message`, which belongs to the client overview—not the pushed Permissions page. A stale snapshot discovered during loading is reported to the same hidden label; ordinary read failures disable editing without an explanation there. A rejected edit can therefore look like a closed/saved editor followed by silently refreshed old data.

**Recommendation:** distinguish Conflict from other errors and display the result on the visible Permissions page/editor (or a visible toast), explicitly state that the edit was not saved, and re-read before allowing another write. Test a real Conflict response and assert visible feedback, not merely the refreshed model.

### F3 — Medium / Confirmed source contract gap: wildcard-identity needs-review rows cannot be converted to scoped Allow

**Evidence:** `apps/grotto/src/ui/page-client-detail.c:174-181`, `:198-218`, `:233-235`. Dependency contract: `nips/nip55l/src/glib/signer_permissions.c:89-98`, `:278-288`, `:682-699`.

Needs-review rows are rendered as non-executable, have Remove, and default to ten minutes when edited. However, the editor pins the original identity and exposes it only as a label. A migrated operation-wide event allow for identity `*` can legitimately become a needs-review row. Choosing an exact kind and Allow still submits identity `*`; the daemon correctly rejects an event allow with wildcard identity. There is no identity picker or alternate “create scoped permission” path in this page. Thus §5.4's Allow-with-explicit-scope resolution is unavailable for this supported migration case. **Remove/confirmed clearing remain available; no unsafe wildcard allow is executed.** This specific GUI path was source-traced, not exercised end-to-end.

**Recommendation:** let the user select an explicit identity when resolving such a review row, create only the selected exact scope with revision protection, and remove the legacy review only after successful resolution. Add wildcard and concrete-identity review fixtures.

### F4 — Medium / Confirmed: newly assembled dynamic UI is not gettext-ready, and display English drives a branch

**Evidence:** `apps/grotto/src/ui/page-home.c:49-72`, `:117-127`; `apps/grotto/src/ui/page-connect.c:46-56`, `:74`, `:87`; `apps/grotto/src/ui/page-client-detail.c:89-108`, `:188-235`, `:277-286`, `:333`; `apps/grotto/src/ui/page-settings.c:48`, `:66-68`, `:118-123`; `apps/grotto/src/ui/page-history.c:12-22`.

Blueprint labels use translation markers, but dynamic confirmations, errors, expiry choices, transport/trust captions and accessible names use bare English string literals. The explicit-permission count uses an English `%s` plural suffix instead of gettext plural forms. Most importantly, `page-client-detail.c:222` chooses its catalogue fallback by comparing a display caption to **“Custom event”**. Translating the caption would change logic.

**Recommendation:** mark/extract runtime strings and plural forms, translate protocol values through presentation mappings, and decide catalogue fallback by semantic kind membership rather than an English caption. Exercise a non-English locale; do not make translation correctness depend on matching UI text in logic.

### F5 — Low / Confirmed: subtraction leaves orphan templates/support code and retired translation entries

**Evidence:** `apps/grotto/data/ui/sheets/sheet-select-account.ui:9-15`; `data/ui/widgets/app-row.ui:9-14`, `identity-row.ui:9-14`, `relay-row.ui:9-18` (all under `apps/grotto/`); `apps/grotto/src/ui/widgets/identity-row.c:9-14`; `apps/grotto/src/ui/sheets/sheet-select-account.c:1-8`; `apps/grotto/po/id.po:31-38,88-89,169-176` and corresponding `es.po` entries.

The production resource inventory has 15 UI files, but 19 remain in `data/ui`: the account-selector and three old row templates are unlisted, with corresponding Blueprint/support source still present. CMake's recursive Blueprint glob and ALL target (`apps/grotto/CMakeLists.txt:130-154`) still compile these orphan Blueprints even though neither production UI source inventory/resource list uses them. The language catalogues retain non-obsolete “Connect Hardware Wallet”, “Sessions”, “Delegations”, “Create Profile” etc. The updated POT/POTFILES cleanup does not remove those entries from shipped PO inputs.

**This is incomplete G10 subtraction, not proof that the retired UI is reachable.** The explicitly removed pages/sheets were deleted; no new reachable inert route or `win.show-sessions` action was found.

**Recommendation:** validate/remove the remaining orphan source/Blueprint/UI triples, narrow compilation to the production inventory, and reconcile all language catalogues. Extend inventory checking beyond files already named in gresource so unlisted leftovers cannot pass unnoticed.

### F6 — Low / Plausible a11y consequence: numeric tiles have no explicit value-to-name relationship

**Evidence:** `apps/grotto/data/ui/page-home.blp:38-52`; `apps/grotto/src/ui/page-home.c:89-96`; `apps/grotto/tests/test-ui-layout.c:123-131`.

Each statistic is a number label and a separate caption inside an unnamed Box, without an accessible label/relation tying them together. The layout test checks only copy/QR button role/tooltips, not the statistics or dynamic client rows. Missing explicit associations are confirmed in source; whether Orca's sequential traversal is sufficiently intelligible was **not** tested, so a broken spoken experience is plausible rather than claimed as observed.

**Recommendation:** expose each statistic as a meaningfully named value/group, verify dynamic row name/state/transport/trust information, and assert accessible properties rather than tooltip text alone. Include watch-only/selected identity state in the accessibility check.

## Contract checks that held

1. **Home information:** `page-home.c:77-84` copies the selected identity and calls `sheet_qr_display_set_npub`; that helper produces only `nostr:<npub>` (`src/ui/sheets/sheet-qr-display.c:311-320`). There are no nsec/mnemonic reads in the new pages. Pending rows render client + operation/kind, not preview/reason/arguments (`src/approval-controller.c:111-125,1069-1084`). Client activity renders operation/kind/outcome (`page-client-detail.c:350-357`). Connect forwards link material to the separate pairing flow and wipes clipboard/file buffers; the landing page displays runtime counts, not a token (`page-connect.c:10-15,35-48,80-88`). Explicit backup/pairing dialogs are not misclassified as Home content.
2. **Daemon statistics:** `page-home.c:89-96` reads `signed_today/client_count/pending_count` from GetStatus. The daemon supplies them (`daemon/grotto_control.c:170-174`); successful signatures are counted in retained daemon activity (`src/event_history.c:1053-1064`). Model refresh is signal-driven (`home-model.c:51-76`), not a GUI timer/counting approximation. Per-client pending badges count the authoritative request snapshot; that is separate from the three tiles.
3. **Permission safety:** tier toggles call Control1 `SetClientTrust`, not SetPermissions (`page-client-detail.c:62-65`). Effective-from-tier rows are read-only (`:246-263`); editing uses a captured revision and unique-owner checks (`:69-75,198-202,298-313`). Clearing is explicitly identity-scoped and confirmed destructive (`:95-108`). Forever displays a confirmation sentence naming decision, client, identity, operation and event scope before writing (`:186-194`); this is a displayed sentence, not a typed-sentence requirement. Lowering trust leaves explicit grants alone.
4. **Removed/inert production IA:** both production source lists and gresource exclude the retired Applications/Permissions/Sessions, dashboards, old home/settings/accounts, hardware/multisig/delegation/social sheets (`CMakeLists.txt:159-166`, `meson.build:207-225`, `data/resources.gresource.xml:6-20`). Deleted paths are absent on disk. Active window type ensures reference only retained page types (`signer-window.c:201-205`). Navigation/actions use Home/Activity/Connect/Settings; Settings buttons target live routes, and the timeout toast is actually added to the overlay (`signer-window.c:324-336,444-447`). F5 qualifies the overall cleanup claim.
5. **Floor / Blueprint:** both build systems require GTK 4.14, Adwaita 1.5, GLib 2.80 and set max-allowed macros (`CMakeLists.txt:19-24`, `meson.build:29-33,300-304`). No forbidden newer production API was identified; real new templates instantiate on stock 4.14.5/1.5.0. Both pin an installed compiler to 0.22.2; the Linux CI image downloads the checksummed official compiler (`scripts/linux-ci.Dockerfile:32-45`). CMake still allows committed UI fallback if the compiler is absent (`CMakeLists.txt:117-126`), while the drift test requires it. Resources depend on generated UI in both builds. Drift passed. Shortcuts are `GtkShortcutsWindow` in Blueprint and C (`data/ui/shortcuts-window.blp:2`, `signer-window.c:285-291`).
6. **Layout:** the window requests 360 px and uses start/end margins and adaptive breakpoints (`data/ui/signer-window.blp:4-8,34-55`). Real English fixtures fit the requested mobile width. Static navigation, copy/QR and trust-picker accessible labels exist. No new explicit LTR-only layout was identified; RTL correctness remains unproven, not a confirmed layout failure.

## Verification / coverage against §9

| Required row | Evidence obtained | Gaps still material to this bounded review |
|---|---|---|
| signer/ui-layout | Fresh pass against existing stock Linux build; real templates, four root pages, light/dark, 360 px, copy→QR tab order, trust write, revision-bearing explicit-deny edit, strict widget/application finalization, GtkShortcutsWindow. | Cross-breakpoint focus is masked by manual re-focus (F1). No visible Conflict assertion, needs-review/wildcard fixture, clear/Forever accept/cancel test, QR payload decode, populated pending group, RTL/non-English/Orca, permission-editor mobile capture, or long/many-identity/client stress fixture. |
| signer/home-model | Fresh pass; all four change signal names, changed counters/arrays, owner replacement, unsupported version, read failure/recovery and stop. | Pending **widget** appearance/disappearance is not covered: model-array assertions are not group-rendering assertions; layout fixture always returns zero pending and never attaches main_app's controller group. No signal-during-read same-owner interleaving assertion or real-daemon counter cross-check in this suite. |
| signer/blueprint-drift | Fresh pass with 0.22.2; committed production UI equals Blueprint, POTFILES paths exist, shortcuts class and registry-derived catalogue checked. | Check iterates gresource entries (`tests/test-blueprint-drift.py:17-35`), so orphan UI/Blueprint files escape it (F5). |

### Screenshots: actually produced, with limits

Twelve PNGs exist in this worktree's `_build/screenshots/`; Home mobile, client, Permissions, Connect and Settings light captures were visually inspected. A fresh Linux layout run also produced all twelve in Docker volume `nostrc-grotto-g10-linux`, `/work/build/screenshots/`: `home-{light,dark}` at 910×630 and `home-360`, `client`, `permissions`, `connect`, `settings` light/dark at 350×630 (client-area dimensions of the requested 360 px window).

The Home and client structure follows the Clave inventory in §6.3: identity/avatar/public card, numeric tiles, clients, and tier radios. This is a comparison to the spec's screenshot descriptions; the original owner-supplied image attachments were not available here. Existing captures do **not** show explicit rows below the long Medium effective list, edit/Forever/clear/Conflict states, nonempty Pending, or RTL. They are real renders, but not full visual-contract acceptance.

### Commands / provenance

- Native macOS existing build: `ctest --test-dir _build --output-on-failure -R '^signer/(home-model|blueprint-drift)$'` — both passed; `signer/ui-layout` explicitly **skipped** under the configured macOS UI-test environment.
- Fresh stock Linux rerun: `xvfb-run -a ctest --test-dir /work/build --output-on-failure -R '^signer/(home-model|ui-layout|blueprint-drift)$'` — three suites plus isolation fixture passed. Runtime linked `/lib/aarch64-linux-gnu/libgtk-4.so.1`; pkg-config reports GTK 4.14.5 / Adwaita 1.5.0.
- Existing floor source/build was reused, not rebuilt. SHA-256 comparison of all 53 surviving G10-changed application files matched except `po/grotto.pot`; all changed code, templates, build definitions and tests matched the reviewed worktree. No clean full-repository build or installed-system acceptance is claimed.
- Temporary focus counterexample failed as described in F1; it modified only a copied test under container `/tmp`, not production code or the tracked test.
- `git diff --check 3f80ac819^ 3f80ac819` passed. No keychain configuration operations were used.
- Jev was used only for source-range/search triage; findings were checked directly against source. Visible telemetry accounts for at least 37 requests / 293,295 input tokens (about $0.0124); one small search result's telemetry was truncated. Range suggestions located the spec/build/model seams, but broad API and leftover-string filtering omitted useful matches, so negative judgments were not trusted. Its relevance scores were not treated as approval/security evidence.

## Recommendations

Resolve F1–F4 before accepting G10's UX contract; complete F5's subtraction and verify F6's accessible statistics. Add the specific missing §9 assertions above, preserving stock-runtime finalization checks and the daemon's fail-closed revision/scope validation. Keep the review bounded: no G11 flow-policy or G13 Shell remediation is requested.

**REQUEST CHANGES**
