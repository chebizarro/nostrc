# W1 review dialog: UX/HIG and consent-contract critique

Date: 2026-11-04. Review task: `nostrc-gzmus`; implementation: `nostrc-u6v55.23.1`.

## Boundary and verdict

Reviewed `master...grotto/w1-review-dialog` in the supplied worktree:

- Base/master: `e383c1bac62d6f8886589625d14c780e58603bff`.
- Branch/HEAD: `966607e8f9b63cca3e67caa0b5d35d40adacbfa5`.
- Worktree: `/Users/bizarro/Documents/Projects/.repoprompt-worktrees/nostrc/rp-agent-835a6890-grotto-w1-review-dialog-40bf198b`.

Scope is only the requested consent contract, HIG/floor, design §d.1/§e copy/layout, tests/screenshots and residual §a.1 items. §d.1 delegates to §e. No product source, fixtures, build configuration or version sources were edited. No commit, push or merge was attempted. This documentation-only review requires no component version bump.

**Verdict:** the nine decision-mode mappings are preserved; no wrong-mode dispatch or automatic decision caused by successful Discard was found. The redesign substantially follows the approved sketch. It is **not a clean UX/HIG pass**: there is a false local-caller assurance, a batch accessibility regression, and incomplete visual evidence for the scope/footer contract. These do not demonstrate a daemon authorization bypass.

Evidence labels: **confirmed/static** means directly established by source and its caller; **confirmed/visual** means visible in committed PNGs; **retained execution** means existing implementation logs inspected, not a test rerun by this reviewer; **plausible** is explicitly not reproduced. S1/S2/S3 follow the design's consent-comprehension/usability/polish scale, not exploit severity. All file:line references below are at the branch HEAD and repository-relative.

## Findings, ranked

### F1 — S1, confirmed/static: remote callers receive a false “Verified local app” assurance

**Where:** `apps/grotto/src/ui/approval_dialog.c:553-558` (also the compatibility setter at `:435`); production input at `apps/grotto/src/bunker_service.c:527-533`, `nips/nip55l/src/glib/signer_caller.c:75-85`, and `nips/nip55l/src/glib/signer_approval_service.c:358-360,391-395`.

A remote NIP-46 caller is constructed with a `nip46:` principal and `SIGNER_TRANSPORT_NIP46`. Its zero-initialized `unattested` flag is false; the daemon therefore exports `verified=true`, while `via` is empty. The new UI treats every verified, non-bridge caller as a **local app**. A remote signing or pairing request consequently takes that branch. Authentication of a remote transport key is not verification of a local executable or of the remote app's claimed name.

This is a new misleading assurance in the who-is-asking summary, not a changed daemon decision. Details retain the actual transport/principal, but §e requires the summary itself to be honest; hiding the correction in Details is insufficient. Distinguish remote-key verification from local-app verification using the already supplied caller/transport metadata. No live remote session was run; the source path is confirmed, the rendered remote example is not.

### F2 — S2, confirmed/static: selected-request checkboxes lose their accessible labels

**Where:** `apps/grotto/src/ui/approval_dialog.c:461-471`; compare master's `gnostr_approval_dialog_set_batch`, which used `gtk_check_button_new_with_label(label)`.

The new batch entries use an empty `GtkCheckButton`, put the operation/identity/peer text into a custom child `GtkLabel`, and set neither an accessible label nor a labelled-by relation. GTK explicitly makes accessibility labelling the caller's responsibility when using [`Gtk.CheckButton.set_child`](https://docs.gtk.org/gtk4/method.CheckButton.set_child.html); this requirement is also in the [GTK 4.14 source](https://github.com/GNOME/gtk/blob/4.14.0/gtk/gtkcheckbutton.c#L1035-L1048). The omission is confirmed; the exact Orca announcement was not executed here.

This matters to consent: keyboard/screen-reader users need to identify which operations they are selecting before **Allow Selected Once**, not merely toggle anonymous checkboxes. Associate the wrapping label with its checkbox. This is distinct from the two flow checkboxes: their `AdwActionRow.activatable-widget` wiring supplies label/description relations in [libadwaita 1.5](https://github.com/GNOME/libadwaita/blob/1.5.0/src/adw-action-row.c#L634-L659). The HIG requires descriptive [accessible names](https://developer.gnome.org/hig/guidelines/accessibility.html#accessible-names).

### F3 — S2, confirmed/visual; contract interpretation noted: scope is available on scroll, not always visible with the commit

**Where:** `apps/grotto/data/ui/approval-dialog.blp:22-53,101-106`; committed `apps/grotto/data/screenshots/approval/approval-{360,420}-{light,dark}.png`; scope text set at `apps/grotto/src/ui/approval_dialog.c:198-240`.

All four top-view fixtures show the active **Always Allow** button while Permission and the “until revoked” scope sentence are entirely below the fold. The hierarchy spends most of the viewport on avatar/headline and five information rows before Content, Permission and Scope. Thus the user's literal “scope + identity always visible” requirement is not met at these supported sizes. The scope is also rendered as low-emphasis ActionRow subtitle text in the scrolled companions, so the prominence concern in §a.1 is only partly resolved.

Important qualification: §e also explicitly permits scrolling at 360 px, and its sketch puts Scope inside the scrolling page. If “always visible” means only “outside Details, wrapping and reachable by scrolling,” the source meets that narrower interpretation. This is a confirmed visibility observation and a residual design/contract tension, **not** proof of truncation, a hidden permission mode or an unequivocal HIG violation. Keep the scope readable in the decision context without changing its daemon meaning; resolve the visibility interpretation with the owner rather than calling the screenshots an unconditional pass.

### F4 — S2, confirmed fixture/evidence defect; runtime footer loss only plausible

**Where:** all four committed `apps/grotto/data/screenshots/approval/approval-{360,420}-{light,dark}-scope.png`; their contrary description at `apps/grotto/data/screenshots/approval/README.md:11-14`; capture assertions at `apps/grotto/tests/test-approval-dialog.c:611-640`.

The scrolled PNGs show Permission and Scope, but **neither bottom commit button nor the footer is painted**. They therefore do not demonstrate the README claim that both views retain the commit bar. The source correctly places the footer in `Adw.ToolbarView`'s bottom slot, and `stepper_focus` checks that Approve has no scroller ancestor (`test-approval-dialog.c:356-367`). Those facts argue against assuming a layout defect from the image alone: capture/render invalidation is another possible explanation. No live scroll interaction was reproduced in this review.

The screenshot test only checks that Approve is mapped before scrolling and that PNG saving succeeds; it does not establish that the rendered footer survives the scrolled capture. The retained `w1-screens` copies also differ byte-for-byte from all eight committed PNGs; for example their 360-light scope capture has the footer and older effect/scope wording. Consequently `screens-final.log` is not a receipt for the exact committed image set. Correct/reconcile the captures and their evidence; do not report a confirmed production footer disappearance without reproducing it.

### F5 — S3, confirmed/static, partly inherited: the plain-language/translation cleanup is incomplete

**Where:** `apps/grotto/src/ui/approval_dialog.c:739-748,773-776,793-796`.

The main crypto preview still starts with the raw operation token (`nip44_encrypt`, `nip44_decrypt`, etc.) and repeats a peer fingerprint in Content, although §e.3 puts the peer in Details and asks for task vocabulary. The Action row and headline are now plain language, so this is a remaining §a.1 jargon issue rather than a failure of the new “all peers” scope. Reaction/repost and pairing preview format strings also remain untranslated English. The specifically audited Block/Trust strings were translated; these residual branches in the same dialog were not. This is copy/localization polish, not a consent-mode change. See HIG [terminology and translations](https://developer.gnome.org/hig/guidelines/writing-style.html#terminology).

## Consent contract: mode-by-mode result

There are **nine `DecideRequest` strings**, plus three separate UI routes (Not Now, selected once, flow). They should not be conflated into nine total controls. The daemon implementation is unchanged by this diff.

| Visible action | Actual route | Result of comparison |
|---|---|---|
| Allow Once | `do_finish` → callback `(true,false,0)` → `allow-once` | Preserved; no saved authority |
| Allow for 24 Hours | callback `(true,true,86400)` → `allow-24h` | Preserved; exact scoped grant, 24 hours |
| Always Allow This App To … / Always Allow | callback `(true,true,0)` → `allow` | Preserved; zero expiry means until revoked, not an immediate expiry |
| Trust This App for Routine Operations / Trust App | `decision-mode("trust-routine")` | Preserved atomic preset/current-request route, not `SetClientTrust` followed by an unrelated approval |
| Deny Once / Esc / ordinary close | callback `(false,false,0)` → `deny-once` | Preserved; selecting a reusable allow does not turn denial into a saved block |
| Block This Operation for This App | header action → `block-operation` | Preserved scoped persistent deny |
| Block App… | cancel-default destructive confirmation → `block-app` | Preserved app-wide block until removed |
| Activate saved permission | `decision-mode("activate-saved")` | Preserved saved scope/original expiry; no conversion to a fresh 24-hour grant |
| Discard saved permission | `decision-mode("discard-saved")` | Preserved deletion-only operation; see trace below |
| Not Now | `deferred` → withdraw notification | No decision RPC; request remains pending until its original expiry |
| Allow Selected Once | `approve-selected` IDs → individual once decisions | Only displayed checked IDs in the same group; no future authority |
| Allow for This Message | `approve-flow` booleans → `ApproveFlowRequest` | Ephemeral flow consent; unchecked options stay off; no disk grant |

Dispatch evidence: `apps/grotto/src/ui/approval_dialog.c:92-175,258-310`; controller mapping/revalidation at `apps/grotto/src/approval-controller.c:738-836,902-947,949-957`; daemon modes at `nips/nip55l/src/glib/signer_approval_service.c:1238-1323`; persisted expiry expression at `nips/nip55l/src/glib/signer_permissions.c:788`.

**No newly offered reusable choice that merely approves once was found.** Choices derive from daemon `rememberable`, `routine_trust_available` and `policy_review` fields; empty `claimed:` cannot get reuse; pairing/self-test have no ladder; custom kinds do not get Routine Trust. AUTH retains the exact daemon canonical relay in the row, selector and saved/once scope. Crypto saved grants explicitly say **all peers**, and encrypt/decrypt remain separate operations (`approval_dialog.c:568-593,609-617,652-703`). The Routine Trust explanation retains cross-identity/preset breadth, ask-by-default exceptions and explicit-block precedence (`:209-211`). These conclusions concern successful eligible decisions, not a promise that service failures can never occur.

Scope resets to the first choice on each authoritative snapshot; moving away from flow clears its booleans (`approval_dialog.c:190-196,665-669`). Steps create a new dialog (`approval-controller.c:984-1001,1082-1098`); daemon-owner/internal closure clears the callback before forcing close (`:206-213`), avoiding an implicit deny. The displayed signing identity comes from the daemon-pinned npub, with local metadata used only as a label (`:1003-1011`; `apps/grotto/src/main_app.c:441-456,476`). It is not an identity-selection authority. Literal viewport visibility remains F3.

### Discard trace

1. The UI disables its affirmative action/selector but does **not** mark the request finished or clear the ordinary review callback (`approval_dialog.c:260-268`). Discard is styled neutrally (`:237-239`).
2. The controller sends `discard-saved` through a dedicated `review_discarded` callback instead of `review_decided`/`next_group_id` (`approval-controller.c:893-915`).
3. The daemon removes the inactive saved permission, not the request, and notifies/refetches without signing (`signer_approval_service.c:1250-1268`).
4. On reply the controller restores READY and fetches the same ID; `info_done` repopulates the existing dialog and resets its choice to Allow once (`approval-controller.c:360-375,717-724`). There is no success-driven advance or automatic decision in that path.
5. The added integration case asserts the same dialog, one pending request and no saved-policy group after Discard; it then explicitly clicks Deny to resolve it (`test-approval-integration.c:638-660`). This uses a deterministic D-Bus fixture with production UI/controller, **not** the real daemon's saved-permission migration path. The daemon's deletion-only behavior was checked statically. Error feedback asks the user to reopen/retry; the report does not claim failure/race paths were executed.

## HIG, sketch and per-request copy

- **Dialog structure:** `AdwDialog` + header + scrollable information groups + two bottom actions matches §e's action-dialog sketch. Block uses `AdwAlertDialog`, Cancel default/close response and destructive response appearance (`approval_dialog.c:279-291`). Deny precedes Allow in the footer. These agree with the HIG's [dialog guidance](https://developer.gnome.org/hig/patterns/feedback/dialogs.html); approval is deliberately not a default Return action. The existing controller only presents review following an explicit review request and supplies a host window (`approval-controller.c:1056-1098`). No unsolicited-dialog regression was found.
- **Q4/writing:** buttons/menu items are header case; selector items/body sentences are sentence case. Informal sentence-style headlines are permitted by the [writing guidance](https://developer.gnome.org/hig/guidelines/writing-style.html#heading-style). Pair Application, Trust App, Activate Permission and neutral Discard Permission labels track selection. F5 covers the remaining copy issues.
- **Styling and menus:** standard `card`, `title-2`, `linked`, `suggested-action` classes and destructive alert appearance; no custom colour/provider was added to this dialog. The three-dot header menu is not a menubar. Theme-derived avatar/button colours are not custom-colour violations. Semantic rows/expanders accord with [boxed lists](https://developer.gnome.org/hig/patterns/containers/boxed-lists.html); [UI styling](https://developer.gnome.org/hig/guidelines/ui-styling.html) supports theme/built-in styles.
- **Keyboard/focus:** initial focus remains Deny and there is no default widget (`approval_dialog.c:384-385`); stepping retains this in the integration assertion (`test-approval-integration.c:533-540`). Ctrl+A/Ctrl+D consent handlers are removed; `_Allow`, `_Deny`, `_Not Now` provide access keys. The test at `test-approval-dialog.c:566-595` compares shortcut-controller bindings and calls label selection directly; it does **not** send real Ctrl+A, Return, Escape or Tab events. Full traversal and Orca were not run. Normal labels/tooltips and flow-row label relations are present; F2 is the identified batch naming regression, not a claim that every control is unnamed.
- **API floor:** no 1.6+ widget was introduced. `AdwDialog` and `AdwAlertDialog` are [available since 1.5](https://gnome.pages.gitlab.gnome.org/libadwaita/doc/1-latest/class.Dialog.html), [including the alert](https://gnome.pages.gitlab.gnome.org/libadwaita/doc/1-latest/class.AlertDialog.html); ActionRow/ComboRow/ExpanderRow/ToolbarView/Avatar are compatible with that floor. The custom checkbox-child API is since GTK 4.8, below 4.14. Existing native logs report GTK 4.14.5/libadwaita 1.5.0 compilation and execution. This review does not use the latest docs' examples as proof that newer widgets are available on 1.5.

| §e.3 case | Result at HEAD |
|---|---|
| Note/contact list | Requested headline; Effect correctly says the app can publish the returned signed object, not Grotto (`approval_dialog.c:624-625,808-812`) |
| AUTH | Sign-in headline, full canonical relay, no hostname-only persistent scope (`:603-606,626,659-661,731-734`) |
| Encrypted signing | Plain encrypted-message headline/placeholder; ciphertext withheld (`:627-629,715-724`) |
| Encrypt/decrypt | Plain headline/action and separate **all peers** saved scope; raw-token/peer placement residue is F5 (`:612-620,773-776,813-817`) |
| Custom kind | Kind number shown, Always available when rememberable, Routine Trust follows daemon eligibility (`:630-631,652-663`) |
| Pairing | **Pair Application**, identity, relay list and transport-key fingerprint; no reusable signing ladder (`:228-232,574-575,669,782-797`); caller assurance needs F1 correction |
| Saved-policy review | Replacement selector, reason, exact identity/relay, original expiry; loading disables affirmative action rather than inventing authority (`:654-656,671-703,818-823`) |
| Identity self-test | Test headline; signing/encryption/decryption explained together; nothing published/no app permission stored (`:536-539,622,777-781`) |
| Room-send flow | Prepare-encrypted-messages headline; unchecked choices only after selecting flow; exact identity, supplied count and remaining first-arrival window, no disk permission (`:584-593,623,648-649`) |

## Residual §a.1 audit, without expanding to other surfaces

| Original concern | Status |
|---|---|
| Mixed loose text/caller/action/scope | Grouped rows fix the structural issue; density and scope priority partly remain (F3) |
| Full monospace key next to caller | Local label + abbreviated key in Signing As, full key in selectable Details; scope intentionally retains exact identity |
| Dim scope text | Explicit `dim-label` removed, but sentence remains a low-emphasis subtitle (F3); a design judgment, not a proven contrast-threshold failure |
| Checkbox/conditional-checkbox consent ladder | Replaced with one wrapping selector and matching action/scope |
| Trust/Blocks buried in Details | Fixed: selector and header menu |
| Ctrl+A signs | Handler removed; static shortcut regression checks retained |
| Text-button stepper | Fixed: compact linked symbolic arrows with tooltips |
| Bare GtkExpander with buried commit | Fixed widget type; batch commit remains inside its dedicated batch expander as §e sketches, ordinary commits are outside scroll |
| NIP-44 jargon/technical flow labels | Flow labels improved; raw crypto token remains in preview (F5) |
| Three scrolling commit buttons/cancel last | Two ordered footer actions, header Not Now; source fix present, exact scrolled screenshot proof defective (F4) |
| Block app lacks destructive appearance | Fixed and explicitly tested |
| Untranslated Trust/Block strings | Those audited strings fixed; other owning-dialog branches remain untranslated (F5) |
| Hidden title redrawn as body title | Real Review Request header plus distinct request headline; no hidden-title flag |
| GtkFrame preview | Replaced with `.card` |

## Test and screenshot evidence: what was and was not executed

**Inspected retained execution:** the common Git directory contains `nostrc-test-gate/w1-966607e8f-native/`. `provenance.txt` attributes it to HEAD/base above and native arm64 image `sha256:3d1c8f2383c7159a44e3c8f1714476ca749333d34c887b780d6efc144eab35e2`, GTK 4.14.5/libadwaita 1.5.0. `tests-committed.log` and `LastTest.log` report six passing CTests, no skips: the test-home fixture, `signer/approval-dialog` (31 cases), `signer/approval-controller`, `signer/approval-integration`, `signer/permission-repeat`, and `signer/blueprint-drift`. The integration output includes “real GTK dialog review path exercised”. This is retained local Linux/container/Xvfb evidence, not hosted CI, a fresh reviewer run or GNOME Shell/owner acceptance.

The recorded focused route was:

```sh
dbus-run-session -- xvfb-run -a ctest --test-dir /build/w1 \
  -R '^signer/(approval-dialog|approval-controller|approval-integration|permission-repeat|blueprint-drift)$' \
  --output-on-failure --no-tests=error
```

**UI honesty is not replaced by model-only tests.** The integration test still uses the production dialog/controller and real daemon for persistent note consent and its repeat (`test-approval-integration.c:785-807`), alternating 24-hour/until-revoked consent and no-prompt repeats for AUTH, deletion, reposts, reaction, custom kind and encrypted kinds (`:813-877`), and atomic Routine Trust with an ask-by-default zap repeat (`:914-933`). Both kinds of reusable grant are still exercised. The separate unchanged `test-permission-repeat.c:92-109,122-172,205-219` additionally checks NIP-04/NIP-44 operation repeats, custom-kind trust rejection and silent block-operation/app denials through the controller/API; that is not claimed as additional GUI click-through. Unit tests retain once/deny/close/defer, until-revoked, explicit modes, expiry, batch and flow cases (`test-approval-dialog.c:667-700`). New tests cover scope text/reset, flow exclusivity, empty claims and shortcut bindings. Discard's new controller/UI integration is the fixture boundary described above.

**Screenshots were actually supplied, not merely promised:** all eight tracked PNGs were opened and inspected. They are 350×690 or 410×690 client captures for requested 360/420 host widths, light/dark and top/scrolled variants, consistently with `README.md:7-14`. The generator uses the production dialog/template and synthetic contact-list data (`test-approval-dialog.c:597-645`). All eight render the same request/Always choice, not eight request-type proofs; the fake short identity also is not a real-length npub layout test. The capture log records eight saved files, but its retained PNGs are not the same bytes/content as the committed set (F4). The eight files establish visual artifacts exist; they do not establish exact final capture provenance or an accepted live workflow.

**Executed by this reviewer:** local ref/base/status checks, full requested branch diff inspection, targeted source/caller tracing, official HIG/API retrieval, inspection of the retained logs, opening all eight committed PNGs plus one differing retained capture, PNG signature/dimension/SHA-256 comparisons, and `git diff --check master...grotto/w1-review-dialog` (pass). Hooks were already installed. No build, product test, fresh screenshot capture, external relay request, keychain operation or GNOME 46 session was executed. Beads synchronization was attempted once and unavailable (`no remote`); it was not retried or treated as verification.

Read-navigation tooling used Jev `find-lines`/`filter-search` only, not code or prose generation: 10 calls / 39 requests / 322,211 input tokens / USD 0.013535. Its snippets identified entrypoints but were too narrow for complete consent/callback proofs, so the relevant whole functions were read; the integration re-query explicitly returned partial. No secondary agent was launched. Only this report is the review deliverable; implementation acceptance/publication and any fixes remain with the owner.
