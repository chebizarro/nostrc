# W3 Identity panel — UX/HIG and custody-boundary review

## Scope and verdict

Reviewed `master` **8aaae2f6a196eb748259f648d6baed327f9a3792** →
`origin/grotto/w3-identity-panel` **ecc7c49fc497ae94817d41517aa520e9f12fbeb9**.
The supplied worktree HEAD matches; a read-only `git ls-remote origin
refs/heads/grotto/w3-identity-panel` independently returned the same head.
References below are relative to this worktree at that head.

Contract: `docs/designs/grotto-settings-style-ux-2026-11-03.md` §c.3,
§d.2–d.3, relevant §f tests, and resolved Q2/Q3/Q4. This is the **W3 slice**:
Home remains default; it is not an assessment of W4/W6 completion, approval UX,
packaging/version release readiness, or general protocol correctness.

**Verdict:** the public-metadata/library separation is sound in the inspected
source, and the new panel largely matches the specified hierarchy. Do not sign
off the requested close/lock custody behavior: copied secrets survive it.
The ordinary entrypoints also do not expose the root panel exercised by most
new layout coverage. No demonstrated non-Grotto secret-export bypass was found.

Severity: **P1** custody/privacy issue to resolve before acceptance; **P2** material
routing, usability or accessibility issue. **Confirmed** means directly supported
by source or inspected artifacts, not necessarily reproduced interactively.
**Plausible** explicitly requires a live reproduction. This review did not launch
Grotto, build, rerun CTest, start containers, or access a keychain/secret service.
Only this requested report was added; no implementation changes, commit or push.

## Findings, highest severity first

### 1. P1 — Confirmed (static): close/lock does not revoke copied-secret custody

**Evidence:** `apps/grotto/src/ui/page-identity-detail.c:162-164,238-241` copies
raw nsec/recovery words through `grotto_secret_clipboard_copy()` but discards its
lease/timer handle. `apps/grotto/src/secret-clipboard.c:20-30` retains the provider
and secret bytes until its **60-second** timer runs; `:10-18` clears only if still
owned. Backup cleanup (`page-identity-detail.c:61-80,367-372`) wipes the displayed
phrase/passphrase but never revokes that clipboard lease. Management lock
(`src/ui/signer-window.c:446-462`) unmaps content/closes a dialog, not the lease.

**Precondition/impact:** explicitly copy a key/phrase, then close Backup or lock
management before the timer expires, without replacing clipboard content. The
Grotto-owned clipboard copy remains available after that boundary; even replacing
the clipboard retains the lease's secret bytes until timer cleanup. This is not a
claim that Grotto can retract a clipboard manager's already captured copy.
The ownership check is good and must be preserved; the missing part is prompt
revocation of Grotto's own lease at close/lock. The helper predates W3, but the new
Backup callers inherit the gap on the surface under review.

**Coverage:** `tests/test-onboarding.c:535-546` proves timer clearing and preservation
of someone else's later copy. It does not copy, close/lock, and assert immediate
owned-content release. Raw confirmation coverage only cancels (`:370-389`).

### 2. P1 — Plausible (static path): standalone Backup shell lacks the lock cleanup connection

**Evidence:** `apps/grotto/src/ui/page-identity-detail.c:411-422` explicitly creates
a trusted Backup shell for a non-`SignerWindow` parent, including onboarding.
The page's cleanup is driven by dialog close, page unmap, owner loss or disposal
(`:69-80,359-387`); the page has no session-manager lock subscription.
The added lock-driven unmap/force-close is in `signer-window.c:446-462`, affecting
that management window, not this independent shell.

**Precondition/impact to verify:** leave this standalone shell and its revealed
phrase mapped, then emit the real management-lock event. If its parent remains
mapped, neither the words nor an in-flight phrase reply is invalidated by that
lock. This is a credible uncovered path, **not a reproduced phrase disclosure**.
`tests/test-onboarding.c:255-279` checks window-hide cleanup, not management lock.
The regular SignerWindow path does have the necessary unmap/generation cleanup.

### 3. P2 — Confirmed (static): ordinary identity entry bypasses the root panel; row/CLI contract is unmet

**Evidence:** Home's `identity_card` has no activation property/handler
(`apps/grotto/data/ui/page-home.blp:20-34`, `src/ui/page-home.c:177-184`). Only the
avatar button opens identity management (`page-home.c:27-30,48-58`); it passes an
npub, and `page-identity-detail.c:413` forwards that npub to
`signer_window_show_identity()`. Thus it opens a **pinned detail**, not the root
Users-style panel. Pinned pages hide Other Identities and Add Identity
(`gnome/grotto-panels/src/grotto-identity-panel.c:154-172`). Home now lists only its
default/first identity avatar (`page-home.c:39-48`).

**Impact:** clicking the visible identity information row does nothing; clicking
the avatar cannot browse Other Identities. The root panel is reachable indirectly
by opening Backup and going Back (`signer-window.c:891-896`), but that is not a
discoverable everyday identity entrypoint. It is not correct to call the root
panel completely unreachable.

`--new-identity` also still dispatches to onboarding, **not the panel**
(`apps/grotto/src/main_app.c:91-101,892-896,1083`; `signer-window.c:856-860`). This
preserves existing creation semantics rather than introducing a broken CLI, but
does not satisfy the review request's literal “--new-identity reaches the panel”
criterion. Do not silently redefine that existing action's semantics to fix it.

**Coverage mismatch:** `tests/test-ui-layout.c:244-257` activates the avatar, not
the row, and checks only the panel type. Screenshots subsequently use the direct
`show_page("identity")` helper (`:293-299`), which is a different, unpinned route.
The “repeated-new-identity-activation” test calls onboarding directly
(`tests/test-onboarding.c:488-501`), not the CLI/GApplication action.

### 4. P2 — Confirmed (source + captures): nested full header bars duplicate window controls

**Evidence:** the unchanged shell keeps its top `AdwHeaderBar`
(`apps/grotto/data/ui/signer-window.blp:14-22`); the panel adds another
(`gnome/grotto-panels/data/identity-panel.blp:6-10`), as does Backup
(`apps/grotto/data/ui/page-identity-detail.blp:6-7`). Both new headers use default
window-button behavior. `_build/screenshots/identity-360-light.png`,
`identity-360-dark.png` and `backup-360-light.png` visibly show two sets of
minimize/maximize/close buttons.

**Impact:** extra chrome consumes scarce 360 px height and gives a subpage a
misleading second app-close affordance. The Users hierarchy is appropriate, but
embedding its header in this existing shell requires a single window-control
hierarchy. This is an observed composition problem, not a claim that nesting
navigation views or using AdwHeaderBar is inherently against the HIG.

### 5. P2 — Confirmed (source + captures): Spanish coverage does not exercise translated panel copy

**Evidence:** `gnome/grotto-panels/data/identity-panel.blp:14-42` and
`src/grotto-identity-panel.c:122-173` add marked translatable strings, but the
library's build files contain no translation extraction/install setup
(`gnome/grotto-panels/meson.build:1-55`, `CMakeLists.txt:14-51`); the application's
`apps/grotto/po/POTFILES.in:1-42` does not include the new shared panel sources.
`apps/grotto/tests/test-ui-layout.c:300-303` asserts **English literals** even on
the Spanish accessibility route; locale setup at `:496-501` only proves old
“Allow”/“Custom event” translations.

**Observed:** `identity-light-es.png` is visibly English and byte-identical to
`identity-light.png` (SHA-256
`580f1bf3262027e197dd7e2152fead9115f74a84fbca250a71ffc1229050a4fe`). The supplied
Spanish Backup capture is likewise not evidence of expanded translated copy.
Consequently these passes do not establish the §f translated-long-string case.
**Plausible, not confirmed:** translated expansion/RTL layout or focus defects;
no RTL capture or forced-RTL case was found in these focused tests.

## Custody boundary: what is supported

- **Public-only adapter:** `apps/grotto/src/grotto-identity-service.c:29-55` maps
  account IDs, local labels, setup state, capability booleans and app counts into
  `GrottoPanelsMetadata`. It does not fetch/export keys or phrases; rejected
  credential-shaped labels fall back to empty text before entering the library.
  Its commands are only label/default (`:24-27,77-101`). `has_secret` is a boolean,
  not secret key material. No pairing token is intentionally passed to this path.
- **No secret-bearing panel API:** the metadata and closed command/host schemas
  remain public-only (`gnome/grotto-panels/include/grotto-panels-0/grotto-panels.h:
  18-46,59-71,132-170`). Backup activation forwards only the identity string
  (`src/grotto-identity-panel.c:27-34`). Models/host targets reject known nsec,
  mnemonic and pairing encodings (`src/grotto-panels-validation.c:9-28,52-57`),
  not every possible covert secret disguised as ordinary text.
- **Non-Grotto host cannot acquire Grotto's callback through this API:** callbacks
  are private per-panel state (`src/grotto-panel.c:52-99`); W3's actual Backup hook
  is static, closes over a weak SignerWindow reference, checks management lock,
  and is installed by Grotto (`apps/grotto/src/ui/signer-window.c:696-699,789-801`).
  A fake/other host can supply its own callback, but that neither installs this
  Grotto callback nor grants daemon authority. Real exports remain behind fresh
  approver checks before dispatch and before secret disclosure
  (`apps/grotto/daemon/grotto_control.c:357-361,658-668,846-858`). This is static
  evidence, not a newly executed malicious-host/daemon authorization test, nor a
  sandbox claim about arbitrary native code in the same process.
- **W2 gate extends to the DSO:** `tests/test-boundaries.py:20-35,52-58,102-146`
  includes the new schema, exports and tightly allowlisted widget imports; no
  IPC/secret-store imports were granted. Negative schema/IPC mutations remain
  (`:147-181`). Admission exercises all command fields, including identity labels,
  and nsec/phrase/token-shaped data (`tests/test-admission.c:39-95`). The new
  Grotto-only adapter and trusted Backup implementation are **outside** that DSO
  symbol gate; the gate alone cannot certify their real host wiring.
- **Display cleanup, regular window:** `page-identity-detail.c:61-80` clears secure
  phrase widgets, owned phrase text and passphrase; `:52-56,118-121,367-387`
  invalidates late results on unmap/disposal/owner loss. Management lock unmaps the
  page and force-closes its dialog. No raw key is rendered in the panel; raw
  replies go straight to the trusted clipboard helper. Findings 1–2 qualify the
  close/lock claim.

## Design fidelity and HIG

- **§d.2 / Q4:** the new panel uses PreferencesPage/Group/EntryRow/ActionRow,
  one 96 px avatar, local-label explanation, selectable public key, Backup/Apps,
  default indicator/action, Other Identities, root-only flat `GtkButton`
  **Add Identity…**, and pinned-only Advanced **Remove Identity…**
  (`gnome/grotto-panels/data/identity-panel.blp:19-42`). Removal is not adjacent to
  Add. Default-first selection, pinned navigation, watch-only hiding and
  translated whole app-count sentences are present (`src/grotto-identity-panel.c:
  106-173`). Button copy is header case. This follows the GNOME 46
  [Users navigation model](https://raw.githubusercontent.com/GNOME/gnome-control-center/gnome-46/panels/system/users/cc-users-page.ui)
  and [user detail grouping](https://raw.githubusercontent.com/GNOME/gnome-control-center/gnome-46/panels/system/users/cc-user-page.ui);
  the flat Add action deliberately differs from GNOME's suggested pill, per spec.
- **No dashboard inside the new panel:** no stat tiles, avatar strip or management
  selector exists in its template. Selection storage/API was removed
  (`apps/grotto/src/accounts_store.c:538-542` and diff of `accounts_store.h`).
  The old Home stat group/avatar container and appearance/language settings still
  exist in the **transitional shell** (`page-home.blp:9-54`,
  `page-settings.blp:5-10`); they were not deleted repository-wide. Under the
  explicit Home-still-default scope this is not a demand to finish W4/W5 now.
  Q2 is respected on the new Identity/Backup surfaces; it is not yet an app-wide
  completed removal. Q3 retains the existing management lock, without a new
  Settings-host unlock claim.
- **§d.3:** Backup is a trusted NavigationPage, separate from everyday metadata.
  Phrase, Encrypted Backup and Advanced groups match the proposed actions
  (`apps/grotto/data/ui/page-identity-detail.blp:10-26`). Raw reveal has both the
  persistent clipboard warning and an AdwAlertDialog with explicit control-risk
  text, destructive **Copy Key**, Cancel default/close (`page-identity-detail.c:
  325-335`). Phrase and passphrase dialogs use AdwDialog; remove uses AdwAlertDialog
  with typed npub and Cancel default (`signer-window.c:748-765`). Save cancellation
  and write failure cannot acknowledge backup (`page-identity-detail.c:87-101,
  197-235`). Missing phrase has an encrypted-backup alternative (`:123-129`).
  Minor sketch difference: GnSecureEntry is inserted directly into the preferences
  group (`:286-293`), not wrapped in the specified row.
- **Floor/styling:** shared builds pin GTK 4.14 / libadwaita 1.5
  (`gnome/grotto-panels/CMakeLists.txt:10-13`, `meson.build:4-9`). Both new XML
  templates parse; neither contains AdwButtonRow, AdwSpinner or AdwBottomSheet;
  inspected new C contains none either. AdwDialog is
  [available since 1.5](https://gnome.pages.gitlab.gnome.org/libadwaita/doc/1-latest/class.Dialog.html).
  No custom color rules were added; standard banner/avatar/destructive styles
  are not custom colors. This fits the HIG's preference for
  [built-in styling](https://developer.gnome.org/hig/guidelines/ui-styling.html).
- **A11y/narrow/keyboard:** icon buttons have explicit names/tooltips
  (`identity-panel.blp:26-27`); passphrase/removal entry names are present
  (`page-identity-detail.c:288`, `signer-window.c:753`). The ordinary 360 px captures
  show wrapped public keys and no obvious horizontal overflow; lower actions are
  below the fold, not proof they are absent. Focus is deliberately moved before
  disabling edits (`grotto-identity-panel.c:98-102`). Current assertions establish
  a mapped, visible, sensitive focus widget, not complete keyboard traversal or
  on-screen focus bounds (`test-ui-layout.c:170-173,307-323`). The HIG calls for
  [keyboard-only testing](https://developer.gnome.org/hig/guidelines/keyboard.html).
  RTL/expanded translation behavior remains unverified, not presumed broken.

## Routing: remaining observations

Home is still first/default (`apps/grotto/data/ui/signer-window.blp:63-70`;
`tests/test-ui-layout.c:259-267`). The direct Identity and Backup routes exist
(`signer-window.c:805-806,891-896`). Backup navigation does not reveal a phrase
until activation; the layout test checks zero exports on mount and both routes
(`test-ui-layout.c:311-323,471-475`). Public QR opens a dialog; Connected Apps
returns to the existing Home app list (`signer-window.c:701-709`). That is an
unfiltered transitional handoff, not the eventual identity-filtered Apps panel,
but not a dead-end and not a new W4 requirement. Programmatic Back is tested for
Backup and pinned identities. Finding 3 describes the real-entrypoint gap.

## Tests and artifacts actually present

**Execution evidence supplied by the worktree, not rerun in this review:**

| Evidence | Actual recorded result / limitation |
|---|---|
| `_build/w3-verification/w3-final-tests.log:1-30` | 13/13 CTest checks pass, including `grotto-panels/smoke-host`, `no-secrets`, `control1-service`, `signer/onboarding`, `identity-model`, `home-model`, `ui-layout`, `ui-accessibility`, `blueprint-drift`. `/tmp/w3-build` receipt, not hosted acceptance. |
| `w3-last-tests.log:1-12` | Later focused 4/4 pass: test-homes, onboarding, UI layout, accessibility. |
| `w3-meson.log:1-19,50-59` | Ubuntu aarch64 toolchain; GTK 4.14.5/libadwaita 1.5.0; Meson panel build and 3/3 tests pass. This is useful floor-environment evidence, not a GNOME desktop walkthrough. |
| `w3-asan-test.log:1-12` | Standalone panels sanitizer-route log: smoke/no-secrets/bus/control1 4/4 pass. Not proof that the entire trusted Backup GUI ran under sanitizers. |
| `affected-final-plan.log:2-3,20`; `affected-final-execution.log:2-12` | Names exact head, but explicitly **partial**; Linux/panels lanes are `not_executed (local profile projection)`. Not complete affected-gate/CI acceptance. |
| Earlier `w3-tests.log:107`, `w3-tests2.log:100`, `w3-tests3.log:37` | Retained failing intermediate runs: respectively 3/6, 4/9 and 1/9 failures. Later success does not erase those receipts. |

The standalone new fake-service case is real: registered at
`gnome/grotto-panels/tests/test-smoke-host.c:287`; `:210-279` mounts the actual
Identity widget at 920/360, selects the default, checks flat Add, opens another
identity, submits label/default commands, changes capability, handles watch-only
and unavailable states, and destroys a page with a deferred edit. Backup there
is a host callback count, **not a trusted secret export**.

`signer/ui-layout` now really mounts the shared widget and trusted Backup route,
captures both, checks no automatic phrase export, and exercises the production
label adapter (`apps/grotto/tests/test-ui-layout.c:293-324,464-475`). Onboarding
adds raw-warning cancellation and encrypted file cancel/failure/success cases
(`tests/test-onboarding.c:370-389,439-485,576-584`). This is substantive coverage,
not merely screenshots renamed from Home.

Eight nonempty new PNGs exist under `_build/screenshots/`: `identity-{light,dark}`,
`identity-360-{light,dark}`, `backup-{light,dark}`, `backup-360-{light,dark}` (also
`-es` variants). I visually inspected Identity 360 light/dark, Backup 360 light,
and Identity wide `-es`: these show the **new panel/Backup**, not old stat tiles.
They are ignored, local artifacts, **not in the pushed commit**; tracked
`apps/grotto/data/screenshots/` still contains only the old Home names for these
surfaces. The captures do not demonstrate lower Advanced/Add controls, secret
cleanup, RTL, or a complete keyboard task.

**Checks run during this review:** ref/remote-head verification; focused source
and diff inspection; `git diff --check master..HEAD` (pass); XML parse/forbidden-
widget inspection (pass for both new templates); screenshot existence, visual
inspection and the hash comparison above. No CTest/GUI/daemon test execution is
claimed. Hooks were not installed because this was a no-implementation-edits
review, not a landing task. Long-file triage used Jev `find-lines` and
`filter-search` (13 API requests, 100,991 input tokens, approximately $0.00424);
its design-section selection was incomplete, so the explicitly requested sections
were read directly. Judgments were navigation aids, not evidence of correctness.

## Focused §f gaps (not an expanded acceptance programme)

| Requested evidence | What remains missing |
|---|---|
| Real entrypoint and default/pinned behavior | Test Home **row**, actual CLI/GApplication action and default-to-Other-Identity navigation, rather than only helper routes; finding 3. |
| Close/lock and late secret replies | Successful raw copy + close/real lock + owned clipboard revocation; phrase dialog close and lock with a late reply; standalone-shell lock. Current hide cleanup and timer ownership tests do not prove these. |
| Cross-host boundary | W2 DSO schema/import/admission coverage is extended, but no new real non-Grotto caller test proving denial of this host/export route, nor a new adapter contamination test, is present. Static daemon gates are strong evidence, not execution. |
| Accessibility / 360 / RTL | Real keyboard Back/Tab/activation and focus return through nested pages/dialogs; translated expanded panel strings; RTL direction and key rendering. Existing programmatic activation/`navigation.pop` and Spanish-English captures do not establish them. |
| Screenshot / desktop contract transition | New local captures exist and are genuine. Published screenshot names, metainfo and desktop-contract assertions were not migrated together as eventual §f specifies. Do not require the final Home/sidebar removal during this explicitly transitional W3 review. |

No new approval-mode, permission-repeat, W6 Settings-host or broad platform matrix
is required by this review. The report is documentation-only: **no component
version bump**; implementation version/release assessment is outside its scope.
