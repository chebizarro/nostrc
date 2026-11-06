# Approval dialog polish: UX, behavior and boundary review

Date: 2026-11-06. Review task: `nostrc-qgwom`; implementation: `nostrc-u6v55.48` and `nostrc-u6v55.54`.

## Boundary and verdict

Reviewed exactly `git diff ca1a0b746...681e72dcc` on `grotto/approval-dialog-polish`:

- Base: `ca1a0b746`; reviewed HEAD: `681e72dcc428030eb795975ee80e7aa0a93a787c`.
- Worktree throughout: `/Users/bizarro/Documents/Projects/.repoprompt-worktrees/nostrc/rp-agent-317007d6-grotto-approval-dialog-polish-56b36e6c`.
- Scope: the approval dialog C, authoritative Blueprint/generated UI, focused dialog tests and six gettext files in that diff. Production callers were traced only to verify local-name provenance and consent wiring. No implementation, tracked tests, catalogs or version sources were changed by this review; container sources were mounted read-only.

**Verdict:** the changed permission choices and scope footers use the same local identity name as Signing As, and the compact four-row AUTH review fits with Details collapsed. All five requested checks pass serially. Consent/default/close behavior and G18 declared-set semantics are preserved. However, the brief's stronger claim that **no permission text still embeds an npub is not true**: the visible inactive saved-permission status retains a raw key (F1). This path predates the diff; it is residual requested copy coverage, not a newly introduced authorization regression. Request that one narrow copy follow-up before calling the full wording criterion complete. There is no consent or layout blocker.

Evidence labels: **confirmed/executed** means run during this review; **confirmed/static** means source/caller evidence; **confirmed/visual** means inspected screenshots generated during this review. References are repository-relative at the reviewed HEAD. Local Linux arm64 container/Xvfb evidence is not native desktop, hosted CI, owner acceptance or release proof.

## Findings, ranked

- **Blocker:** none found.
- **Should-fix:** F1 below, tracked separately as `nostrc-empeg`.
- **Nit:** none additional in the scoped diff.

### F1 — S2, confirmed/executed and static — Should-fix: inactive saved-permission text still displays an npub instead of the local name

**Where:** `apps/grotto/src/ui/approval_dialog.c:969-972`; related identity-drift text at `:950-952`. Visibility is enabled for policy review or identity drift at `:808-812`; the status is outside Details in `apps/grotto/data/ui/approval-dialog.blp:55-61`.

The new footer uses `identity_label`, but the visible saved-permission subtitle still formats `field(info, "identity")` directly. The identity-drift branch likewise inserts `field(row, "identity")` for another identity. Both statements already exist at base `ca1a0b746:953,970-971`; this finding must not be described as a new regression from this patch. It is relevant to the current explicit request to verify that no permission text still embeds an npub, rather than an unrelated baseline repair.

**Failure scenario:** a pending operation has `policy_review=true`, a matching inactive permission row and local identity name `UTM`. Signing As shows `UTM · <short key>`, and selecting Activate saved permission gives `Activate the saved permission for UTM. Original expiry: Until revoked`. Nevertheless, Saved Permission Is Inactive displays `Access policy changed. Identity: <full npub>. Original expiry: Until revoked`. An isolated diagnostic linked to the unchanged production objects/resources asserted that this status was mapped, contained the full public fixture npub and did not contain `UTM`; those assertions passed. Thus the existing green suite does not prove the blanket wording claim. The different-saved-identity branch is source-confirmed, not separately exercised by that diagnostic.

Use the relevant identity's local name in this permission-status copy, keeping raw-key technical presentation in Signing As/Details. For drift, resolve the saved identity's own name rather than substituting the current identity's name. Preserve key-based permission matching, expiry and authorization semantics. This is a presentation/completeness defect, not an approval bypass or grounds for a new confirmation class. No fix was made.

## Owner-policy and regression checks

- **Local-name provenance — confirmed/static and executed:** production `main_app.c:318-331,353` looks up the matching account's stored `AccountEntry.label`. `approval-controller.c:1069-1077,1092-1103` adds that presentation field to the daemon snapshot without changing its identity. In `approval_dialog.c:789-794`, Signing As combines that exact label with the shortened key. Permission options use the same label at `:919-921`, flow scope at `:902`, and `permission_changed()` uses the same field/fallback at `:305-306`. Ordinary once/24-hour/until-revoked, activate-saved, management and declared-trust footers do not concatenate the identity key. Missing labels use the same translated `Identity` fallback. F1 is the remaining visible permission-status exception; keys appropriately remain in technical details and callback identity data.
- **Deliberate test-contract changes:** `test-approval-dialog.c:425-449,550-576,699-725` now require the local name and reject the npub in flow/ordinary scope text. The scope/reset test additionally requires `Alice · npub-pinned-by-daemon` in Signing As and checks the full generated Always allow option. Commit-label, pinned callback identity, TTL, deny and reset assertions were retained; the removed scope-key assertions were contrary to the requested display change, not weakened authorization assertions.
- **Deny first / Allow suggested / Return / Esc — confirmed/static and executed:** Blueprint `:130-131` retains Deny before Allow, `suggested-action` on Allow and `receives-default: false`. C `:530-531` retains initial Deny focus and no default approve widget. The normal dialog is `AdwDialog`, not an alert with a response ID: `closed()` at `:429-438` returns `(allow=false, remember=false, ttl=0)` once. Not Now remains a distinct deferral (`:245-254`). The separate Block App alert still has both default and close response `cancel` (`:422-425`). Existing callback/default-focus/no-primary-consent tests passed. Additional real X11 input probes confirmed Return on initial Deny never allows; Return on Details, selectable scope text and the permission chooser never approves; Escape dismisses the chooser first, then Escape on the dialog denies once without persistence. Explicitly activating a focused Allow control remains intentional consent; no new global Return approval binding was added. This follows the irreversible-action exception in the [HIG dialog guidance](https://developer.gnome.org/hig/patterns/feedback/dialogs.html#general-guidelines).
- **G18 / no always-confirm class:** the diff changes no decision handlers, permission evaluator, declared-set construction or request eligibility. C `:209-235,905-913` preserves one lasting declared-set decision, no once/24-hour alternatives for declared sets, and the existing explicit trust action. Existing declared-set, declared-set-trust, pairing, extension, inactive-permission and ordinary once/TTL tests all ran within approval-dialog. No new per-kind re-prompt or confirmation ceremony is proposed.
- **Four rows + one Details expander — confirmed/executed:** Blueprint `:14` retains configured content height 560; `:40-47` compacts only the avatar/title/caption header. Rows at `:50-53` and the single Details expander at `:96` remain above the pinned footer. New `test-approval-dialog.c:1030-1077` tests allocated **920×640 and 360×640**, light/dark, once/24-hour/until-revoked, scroll position zero. It asserts all five widgets are mapped, wholly inside the real scroller and above the scope/footer, with visible decisions. Details bottom was **352 px**; viewport was **408 or 421 px**. Outer allocated-size assertions intentionally include GTK's CSS margin; screenshot/content dimensions are 910×630 and 350×630, not evidence that the requested outer sizes were silently shrunk. Pixel comparisons at `:736-769,781-823` also verified the actual buttons were painted.
- **HIG floor and visual limits:** the [HIG desktop floor is 1024×600](https://developer.gnome.org/hig/guidelines/adaptive.html#small-size-handling); a 640-high host alone would not prove that floor. An uncommitted diagnostic copy of the same bounds test, with only host dimensions changed, passed at **1024×600** and additionally **360×600**, still configured at content height 560, all three modes and both styles. Desktop Details bottom was **352**, viewport **378 or 391**; narrow viewport was **408 or 421**. Generated light-desktop/dark-narrow contact-list snapshots were visually inspected: local-name footer, key in Signing As, collapsed Details and both decision buttons are visible. Those captures have three primary rows because no relay row applies; the four-row AUTH claim comes from the bounds tests, not those screenshots. This is not a claim of 360×294 support or native GNOME Shell work-area qualification.
- **i18n / drift:** `msgfmt --check --check-format` passed for all five changed `.po` catalogs; no fuzzy flags remain in them or the POT. Semantic catalog diff inspection found source-location/date churn, an unchanged About translation moved, the unused empty identity fallback removed and the until-revoked sentence replaced. The replacement is untranslated in all five catalogs, but its previous equivalent was also untranslated: no previously translated sentence was lost. Spanish GTK accessibility execution passed; this is not complete translation, approval-dialog RTL, large-text or Orca acceptance. Pinned Blueprint 0.22.2 drift checks passed, including generated UI/resource/catalog consistency.

## Verification actually run

Image: `nostrc-linux-ci:arm64`, ID `sha256:3d1c8f2383c7159a44e3c8f1714476ca749333d34c887b780d6efc144eab35e2`; Linux `aarch64`, GTK **4.14.5**, libadwaita **1.5.0**, Blueprint **0.22.2**. `xdotool` **1:3.20160805.1-5build1** and `libxdo3` were installed only in the task's disposable containers. Sources were this worktree mounted read-only at `/src`; `/build` was fresh and container-local. Builds/tests ran as the image's `ci` user with the repository's private test homes/buses. No developer keychain commands or host package installation occurred.

Fresh configuration used Ninja/Debug, `BUILD_APPS=ON`, `BUILD_GNOSTR_APP=OFF`, `BUILD_GROUNDHOG=OFF`, `BUILD_TESTING=ON`, `BUILD_SIGNER_TESTS=ON`, `BUILD_NATIVE_HOST=ON`, `NIP55L_SECRET_BACKEND=libsecret`, `GROUNDHOG_STORE_KEY_DEFAULT_BACKEND=secret-service`, `BUILD_RELAYD=OFF`, `SIGNET_ENABLE=OFF`, `ENABLE_TCP_IPC=ON`, `NIP55L_TEST_TRUST_ENV=ON` and compile-command export. Only `test-approval-dialog`, `test-approval-integration` and `test-ui-layout` plus their build prerequisites were built; building a prerequisite did not select its unrelated tests.

Executed selection, with `ctest -N` first confirming all five registrations:

```sh
xvfb-run -a ctest --test-dir /build -j1 \
  -R '^signer/(approval-dialog|approval-integration|ui-layout|ui-accessibility|blueprint-drift)$' \
  --output-on-failure -V
```

| Command/check | Actual result |
|---|---|
| Fresh configuration and targeted `cmake --build /build --parallel 4 --target test-approval-dialog test-approval-integration test-ui-layout` | PASS |
| `signer/approval-dialog` | PASS, 5.76 s; includes the new compact-auth-layout case and existing consent/G18 cases |
| `signer/approval-integration` | PASS, 12.09 s; real daemon/controller/dialog fixture, not an isolated string helper |
| `signer/ui-layout` | PASS, 59.07 s |
| `signer/ui-accessibility` | PASS, 59.05 s; Spanish catalog, `GTK_A11Y=test` |
| `signer/blueprint-drift` | PASS, 1.36 s; exact compiler pin |
| Test-home fixture and total | PASS; **6/6 including fixture**, no skipped checks, CTest/container exit 0, **137.38 s** total |
| `msgfmt --check --check-format -o /dev/null <catalog>` for `es`, `fa`, `id`, `ja`, `pt_BR`; fuzzy-flag scan of these and POT | PASS, five catalogs; zero fuzzy entries |
| Isolated production-linked `review-keyboard`, `review-initial-return`, `review-floor-layout`, plus existing `snapshots` case with PNG output | PASS; real X11 keys and the additional 600-high bounds described above |
| Isolated `review-saved-identity-text` reproducer | PASS its diagnostic assertions that the mapped saved status contains the full npub despite local label `UTM`; **confirms F1, not a product fix** |
| `git diff --check ca1a0b746...681e72dcc` | PASS |

Initial harness failures are retained separately: the first container exited 1 because my `xdotool version` environment probe required a display, before configuration/build/tests. Package metadata replaced that probe. The first keyboard diagnostic aborted before sending keys because it tried to focus the `AdwExpanderRow` container directly; corrected diagnostic traversal focuses its actual `AdwActionRow` child. Neither was a production failure, product change, disabled guard or retry of a failing shipped test. The requested CTest selection passed on its first executed run. Nonfatal EGL software-rendering and deprecated allocated-size getter warnings were not suppressed.

Local receipts: `/tmp/grotto-approval-polish-review-20261106/{initial-container.log,container.log,LastTest.log,probe-initial.log,probe.log,saved-status-probe.log,diagnostic.c,shots/}`. The diagnostic copy adds only review cases to the original fixture, is compiled using its exported compile command and relinked with the existing production objects/resources; it is not a tracked test change. Task-owned containers and the temporary derived probe image are removed; the compact receipts remain in `/tmp`.

Full suite/full gate, broad affected-lane execution, sanitizers, live relays/keyring, native desktop/Orca and owner acceptance were not run. `grotto-panels/smoke-host` was deliberately excluded as the accepted unrelated `nostrc-qdgge` baseline. No unrelated baseline repair is requested. Beads has no configured remote, so tracker synchronization is unavailable; Git delivery uses the branch's existing origin route.

Relevance assistance only: Jev `find-lines`/`filter-search` narrowed long-file reads (42 requests, 361,544 input tokens, about $0.0152). An overly broad keyboard query selected panel-navigation spans, so actual dialog defaults/callbacks were inspected directly instead. Relevance scores and earlier implementation receipts were not treated as verification; all load-bearing findings above were checked against current source and/or this run's execution.

## Version and delivery decision

This report is documentation-only: **no bump** for Grotto, grotto-panels or any other independently releasable component. The underlying compatible shipped UI/copy correction is PATCH-class within the already declared unreleased Grotto **0.1.1** wave; CMake, Meson and `VERSION_MANIFEST.md:35` agree. Grotto-panels remains unchanged at unreleased **0.1.0** (`VERSION_MANIFEST.md:34`). No version source, release column or tag was edited. The review commit adds only this report; `nostrc-empeg` tracks the residual display issue separately.

## Merge recommendation

**Request one narrow copy correction, F1, before merging as complete against the brief's no-npub permission-text criterion.** The option/footer change and compact layout themselves pass: there is no observed authorization, default-key, denial, G18 or viewport regression. Preserve those changes and the existing consent semantics. Do not add an always-confirm class, re-prompt saved declared sets, or require a repository-wide audit/full suite. Recheck the directly affected saved-permission presentation after that finite follow-up; the accepted smoke-host baseline remains separate.
