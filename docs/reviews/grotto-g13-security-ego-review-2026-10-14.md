# G13 Shell signer indicator — focused security / EGO review

## Context / Scope

- **Verdict: REQUEST CHANGES** for F1 (status accuracy), not a demonstrated privilege escalation. No high/critical finding in the reviewed delta.
- Compared local `master` **d9cd619f830482a2f88785849b825eac4859f8d6** to pushed tracking ref `origin/grotto/g13-shell-indicator` **b9842a32b4cc9baa5f2a5c3434e5f8dbcac2fbf5**. Target worktree HEAD matches that ref; one commit, 15 changed files. Remote was not fetched during review.
- Worktree: `/Users/bizarro/Documents/Projects/.repoprompt-worktrees/nostrc/rp-agent-9554056d-grotto-g13-shell-indicator-37922dcc`.
- Contract: `docs/plans/grotto-signer-ux-2026-10-12.md:343-352` (§5.12, complete), `:227-233` (§5.6 indicator/SNI), `:151-177` (§5.3 auth/revisions), `:56-61` (§3.4 Shell facts). G07's actual string action contract takes precedence over §5.12's older empty-target shorthand.
- Limited to the seven requested topics. Existing daemon/GUI code was followed only to establish the new module's privilege and G08 integration boundaries. No general daemon, custody, relay, or backlog audit.
- Only this report was written in the repository. No source edits, tracker changes, hook installation, commits, pushes, or worktree cleanup. Target was clean initially; main's pre-existing dirt was preserved. Review-only documentation: **no component version bump**.

## Findings (severity ordered)

### F1 — Medium · Confirmed: RequestsChanged leaves the previous badge authoritative while refresh is pending

**Evidence:** `gnome/nostr-shell-extension/lib/signer.js:70-72,86-94,97-108,119-127`; `lib/signer-panel.js:66-80`; shared call timeout `lib/dbus.js:12-17` (paths in this paragraph are under `gnome/nostr-shell-extension/`).

`RequestsChanged` increments a local serial and schedules/coalesces a read, but neither clears nor marks `_status` stale, and does not emit an invalidated state. The panel keeps the old count, state label and Review sensitivity until an accepted reply or failure. With a held read, this can retain an obsolete badge through that read's 5-second timeout and the subsequent fetch. A previously empty queue can continue to say **Ready** with Review disabled even though requests now wait.

**Local reproduction:** a temporary copy of the existing private-bus test, importing the unchanged production monitor, first rendered count 5, held the next snapshot, changed the daemon count to 8, emitted `RequestsChanged`, and waited until the monitor had processed the invalidation. Observed: `badge=5, daemon count=8, refresh pending=true`. The original test at `tests/test-signer.js:214-231` proves a superseded reply is not rendered, but never checks the already-rendered badge during that interval.

**Impact:** confirmed violation of the requested “badge never stale after RequestsChanged” condition; inaccurate informational UI, not authorization bypass.

**Recommendation:** on a relevant newer invalidation, stop presenting the previous count as current; expose an explicit refreshing/unknown state until a current snapshot arrives. Do not substitute a false zero/Ready state. Add a held-read regression assertion at the invalidation boundary, including 0→N and N→0.

### F2 — Low · Plausible robustness gap; confirmed that wire revisions are ignored

**Evidence:** `gnome/nostr-shell-extension/lib/signer.js:70-72,89-94,109-121,138-140`; daemon supplies `requests_revision` and `status_revision` at `apps/grotto/daemon/grotto_control.c:159-161`; the fake snapshot omits both at `gnome/nostr-shell-extension/tests/test-signer.js:65-75`.

Signal parameters are discarded and returned revision fields are not inspected. The actual ordering defense is **local invalidation serial + unique-owner equality + one fetch at a time**. This correctly rejects the ordinary signal-during-read and owner-replacement races covered by the suite; it is not fair to call those races unprotected.

However, no revision watermark establishes that an accepted snapshot reaches the latest announced revision or prevents a regressing snapshot from the same owner. An older/inconsistent same-owner implementation could return stale data to the latest local call and have it accepted. That outcome is plausible, not reproduced against the current daemon. A spoofed owner can already lie about coarse status; this does not give it approval authority.

**Recommendation:** include genuine revision fields and lower/equal/newer revision cases in the fake; validate watermarks if revision freshness is the intended contract. Otherwise explicitly justify the serial-only ordering guarantee against the current daemon instead of treating the existing test as wire-revision coverage.

### F3 — Low · Confirmed: §5.6's Quit approval UI action is absent

**Evidence:** `gnome/nostr-shell-extension/lib/signer-panel.js:37-40` contains only Review, Open, Pause and Resume; §5.6's menu contract is `docs/plans/grotto-signer-ux-2026-10-12.md:233`.

The module matches §5.12's narrower menu list, but not §5.6's additional **Quit approval UI** action. This is a small contract-completeness issue, not an approval or lifecycle vulnerability.

**Recommendation:** add the GUI-only quit action, or explicitly reconcile the two spec lists if G13 intentionally excludes it. Do not terminate the signer daemon from the extension.

## Confirmed checks / bounded conclusions

### 1. Privilege boundary

- Production daemon calls in the new module are exclusively **GetStatus** and **SetPaused(true)** (`lib/signer.js:105-108,143-149`). The only consumed Control1 signals are **RequestsChanged** and **StatusChanged** (`:70-72`). No ApproveRequest, request listing, approval details, identity enumeration, or signing-content access was found. The coarse result is explicitly whitelisted before reaching the panel (`:109-121`); the identity-canary assertion passes (`tests/test-signer.js:73-74,139`). Receiving an extensible GetStatus dictionary is not sensitive enumeration by the extension.
- Resume goes exclusively to `org.freedesktop.Application.ActivateAction("resume-signing", [], {})` on **org.nostr.Grotto**, never SetPaused(false) (`lib/signer.js:157-171`). The existing GUI opens a confirmation with Cancel as default, rechecks resume eligibility and the pinned Control1 owner before sending false (`apps/grotto/src/main_app.c:523-566`). The daemon independently requires the approver check for false and checks session-lock policy (`apps/grotto/daemon/grotto_control.c:614-625,691-699`). This is a source trace, not a live end-to-end authorization test.
- A spoofed **org.nostr.Signer** can forge coarse status, counts and apparent pause success, but cannot select a method/action/target through the status data. Daemon calls are pinned to the observed unique owner (`lib/signer.js:98-108,138-148`); GUI action destination and names are constants (`:10-11,152-171`). No status signal automatically invokes resume or review. A malicious process cannot call methods on the extension's suppression name: it exports no callable object. No new extension-mediated path to resume or approve was found. The informational panel must not be represented as authenticated daemon identity.
- Proxy construction uses **DO_NOT_AUTO_START | DO_NOT_LOAD_PROPERTIES** (`:55-57`); status and pause calls use **NO_AUTO_START** (`:107,148`). The activation-marker test passes with an activatable fake signer (`tests/run-signer.sh:8-10`; `tests/test-signer.js:128-133,267-268`). Only explicit GUI actions permit application activation, as intended.

### 2. EGO / Shell compatibility and lifecycle

- ESM imports throughout the new modules and entrypoint. Shell-side imports are Gio/GLib and Clutter/GObject/St plus Shell resources (`lib/signer.js:1-5`, `lib/signer-panel.js:1-11`, `extension.js:15-27`). No Gtk/Adw/Gdk import was found in the entrypoint or its lib modules; `prefs.js` is separate. No direct network/telemetry implementation was found in this Shell import graph. This is not a claim that user-activated external services have no network behavior.
- Objects are instantiated by `enable()` (`extension.js:175-182`), not at module load. Disable destroys the signer before the original indicator and clears extension references (`:185-190`). Signer destruction cancels calls, disconnects both proxy handlers, drops the proxy, unowns the suppression name, and prevents late emissions (`lib/signer.js:199-214`). Panel destruction disconnects its menu handler and destroys the actor/menu tree (`lib/signer-panel.js:86-94`); menu action handlers belong to that destroyed tree. No signer polling timer/main-loop source is created.
- Existing indicator cleanup remains connected: `extension.js:142-170`; relay signal subscriptions/name watch and timer/debounce/settle sources are removed (`lib/relay.js:161-185`), wallet and DM proxies are disconnected/dropped (`lib/wallet.js:167-178`, `lib/dm.js:60-69`). No newly introduced lifecycle violation found. Actual actor cleanup across Shell lock/unlock and failed enable paths is not exercised headlessly.
- Metadata lists exactly major versions **46,47,48,49,50**, preserves UUID, schema and gettext domain, and does not request unlock-dialog mode (`metadata.json:1-16`). No holdKeyboard/releaseKeyboard, show-restart-message/restart signal usage was found in the Shell code. These are valid source-level checks, **not runtime proof** for every declared version.
- Rules checked against the official [EGO review guidelines](https://gjs.guide/extensions/review-guidelines/review-guidelines.html) (enable/disable cleanup, GTK separation, metadata and telemetry) and [GNOME Shell 50 porting guide](https://gjs.guide/extensions/upgrading/gnome-shell-50.html) (removed keyboard/restart APIs). This focused review is not EGO approval.

### 3. G08 name ownership

`SignerIndicator.start()` runs only after successful panel insertion (`extension.js:180-182`, `lib/signer-panel.js:49-51`). The monitor requests **org.nostr.Shell.Signer1** with NONE flags on start and unowns it on destroy (`lib/signer.js:50-54,199-213`); repeat destruction is harmless. Headless tests verify ownership, disable release, late-reply suppression and clean re-enable (`tests/test-signer.js:128-129,250-266`).

G08 watches that exact name and switches visibility both ways (`apps/grotto/src/main_app.c:594-599,665-669,686-687`), so there is no persistent suppression setting to get stuck after normal disable. Loss of the Shell process/bus connection releases connection-owned names by D-Bus lifetime semantics; abrupt-process-loss integration was not tested here. A JavaScript exception is not equivalent to process exit: cleanup still relies on Shell invoking disable. Same-session name spoofing can hide the opportunistic SNI under the existing G08 cosmetic contract, but cannot authorize resume or approval. No new security dependency on the sentinel is introduced.

### 4. States and G07 activation

`lib/signer.js:32-47` correctly prioritizes stopped → unavailable → paused → waiting → enabled-bunker-problem → ready. Pending badges remain available when paused; the panel shows bunker problems separately when another phase takes precedence (`lib/signer-panel.js:54-83`). A disabled bunker never produces degraded/offline; failures and unsupported versions clear accepted state (`lib/signer.js:111-127`). F1 is the invalidation interval exception.

Review supplies **one string Variant with target ""**, not an empty target array (`lib/signer.js:152-155,168-171`). That matches G07's registered string action and inbox handler (`apps/grotto/src/main_app.c:381-391,1053`) and passes the fake application's exact-target test (`tests/test-signer.js:186-194`).

If the name exists but Control1 is absent, UnknownMethod/UnknownInterface follows the read-error path to **Signer status unavailable — update or open Grotto**, with no badge or enabled pause/resume. It does not falsely label an old daemon stopped or ready, nor fall back to sensitive legacy APIs (`lib/signer.js:33-44,124-127`; `lib/signer-panel.js:62,75-80`). Actual interface absence was source-traced, not directly tested by the committed fake.

### 5. Packaging / versions

- CMake installs all lib JS and icon SVGs; the zip target already includes both directories (`gnome/nostr-shell-extension/CMakeLists.txt:9-16,33-43`). Four local shape-distinguished SVG states ship; the Ready SVG is byte-identical to Grotto's current symbolic glyph. No external SVG resource references found.
- Both Debian control variants use optional **Recommends**, not Depends (`apps/grotto/packaging/debian/control:43-46`, `packaging/debian-desktop/control:68-69`). Signing/approval does not require the extension.
- **0.4.0 → 0.5.0 MINOR** is appropriate additive user functionality under AGENTS.md. `metadata.json:15` agrees with the new authoritative manifest row (`VERSION_MANIFEST.md:24`); no duplicate extension version source is declared. G13's decision records the extension bump, test/docs no-bump, and unchanged release columns (`:39`). Grotto's packaging-only change is explicitly assessed PATCH-class and folded into unreleased 0.1.0 **per the manifest's recorded owner direction**; this review did not independently establish that approval. That fold is an exception to the literal automatic bump checklist, not a claim that packaging has no version impact.

## Tests / coverage gaps

### Executed in this review

- Unmodified `sh tests/run-signer.sh <gjs-wrapper>`: **50 checks passed**, using the real production monitor, canonical Control1 XML, private bus and isolated activation directory.
- Temporary invalidation probe described in F1: **53 checks passed** including reproduction assertions; scratch copy only, not a proposed test patch.
- `node tests/run-tests.js` and `gjs -m tests/run-tests.js`: **99 checks passed each**.
- `glib-compile-schemas --strict --dry-run schemas`: passed.
- GJS Reflect.parse module syntax check: passed for the entrypoint/new modules/test; an additional all-module parse also completed successfully.
- `git diff --check master..origin/grotto/g13-shell-indicator`: passed.
- Homebrew gjs initially failed to resolve `libgjs.0.dylib` before test execution. Successful runs used `DYLD_LIBRARY_PATH=/opt/homebrew/opt/gjs/lib`; for the private-bus launcher this was set immediately before exec of gjs in a temporary wrapper. No installation, keychain or source change was made.

### What these tests do and do not prove

The committed suite covers badge changes/clearing, enabled-vs-disabled bunker transitions, pause failure/success, GUI-only resume dispatch, empty-string review, owner loss/replacement, superseded replies, unsupported version/read failure, normal suppression-name release and destroy with an in-flight call (`tests/test-signer.js:128-268`). The CTest entry is conditional on gjs and dbus-run-session (`CMakeLists.txt:59-70`); absence of that test on another host must not be counted as a pass.

Remaining bounded gaps:

1. **Confirmed gap:** no assertion of current/unknown badge immediately after invalidation (F1); no genuine snapshot revision fields or revision-regression cases (F2).
2. **Confirmed gap:** fake Application records action calls; it does not run Grotto's real confirmation/cancel/lock/owner-change path or independently prove the real daemon rejects a non-approver's false/approval calls. Those boundaries are source-verified here, not integration-tested by G13.
3. **Confirmed gap:** unsupported version and generic NotReady are tested, but owned-name-with-no-Control1, missing/mistyped required fields and missing/false pause capability are not directly covered.
4. **Confirmed gap:** no malicious non-owner signal sender test, suppression-name contention/name-loss test, real G08 notifier hide/return integration, abrupt Shell connection death, or real panel actor lifecycle test. These are missing proofs, not confirmed escalation or stuck-name bugs.
5. **Plausible load gap:** continuous signal invalidations can repeatedly supersede reads; no sustained-signal progress test establishes a bound on stale/unknown display. Coalescing limits concurrent reads, not freshness latency.
6. **Explicitly deferred acceptance:** no installed package/zip smoke, real GNOME 46 or 50 session, lock/unlock, accessibility/theme, or Ubuntu AppIndicator coexistence test was run in this review. README `:169-185` explicitly says G13 did not perform the live matrix and assigns all rows to **G12 / nostrc-u6v55.14**. That documentation is honest; metadata alone is not completed GNOME 50 acceptance. No full repository build/CTest or prior Linux monitor suite was run for this focused review.

## Recommendations

Fix F1 and add its counterfactual private-bus test before accepting the requested state-machine contract. Resolve the lower-severity revision proof and §5.6 menu discrepancy without expanding the module into an approver. Keep real Shell 46/50, G08 coexistence and GUI confirmation acceptance explicitly with G12; do not promote headless passes to live/EGO approval.

**REQUEST CHANGES**
