# Identity panel UX: discovery, default identity and backup reminder review

Date: 2026-11-06. Review task: `nostrc-zlkpu`; implementation: `nostrc-u6v55.36`, `.46`, `.47`, `.50`.

## Boundary and verdict

Reviewed exactly `git diff ca1a0b746...1ec85fbfb` on `grotto/identity-panel-ux`:

- Reviewed HEAD: `1ec85fbfb282c77e9afb3acbd33165347e0e9ade`; 52 changed files, including catalogs, packaging and test registrations.
- Worktree throughout: `/Users/bizarro/Documents/Projects/.repoprompt-worktrees/nostrc/rp-agent-54680542-grotto-identity-panel-ux-06f9fbe9`.
- Scope: bounded profile discovery/spinner/empty state and existing profile publication; direct default action/indicator; persistent cosmetic backup-banner dismissal in Grotto and the Settings manager host; identity-count removal; HIG wording, additive ABI, versions, Blueprint and catalogs.
- This review changes only this report. No implementation, tests, catalogs, version sources or policy were fixed. Closed implementation beads were treated as claims, not verification.

**Verdict: request changes before merging unchanged.** All seven requested focused tests pass. The watchdog remains a failure boundary, not a substitute for EOSE, and secret/policy ownership remains host-side. However, a successful address check after failed discovery incorrectly produces the definitive empty-profile message (F1), and the branch changes panels to 0.2.0 despite the current explicit 0.1.0-unreleased policy (F2).

Evidence labels: **confirmed/executed** means run in this review; **confirmed/static** means traced through source and production callers. S2 denotes a user-visible correctness or owner-contract defect, not exploit severity. File references are repository-relative at the reviewed HEAD unless explicitly marked base. Container/Xvfb proof is not native desktop, hosted CI, live-relay, owner-acceptance or release proof.

## Findings, ranked

### F1 — S2, confirmed/executed and static — Should-fix: address checking can turn failed discovery into “No profile published yet”

**Where:** `gnome/grotto-panels/src/grotto-profile-view.c:165-170,180-186`; empty-state condition at `:91`; address-check dispatch at `:228`; production result path at `apps/grotto/src/grotto-profile-service.c:743-755`.

A failed FETCH now correctly creates an editable blank model with `fetch_failed = TRUE`. But **every** subsequent successful profile-host action clears that flag, including `GROTTO_PROFILE_VERIFY_NIP05`, which only checks an address and does not run `discover()`. Its result updates the editor draft, not the displayed fetched profile, yet `render()` now reports that no profile has been published. Closing the editor does not restore the discovery state (`grotto-profile-view.c:247-258`).

**Failure scenario:** an identity has no cached profile and its relay backfill times out or is refused. Open Edit Profile… → use Check beside Nostr Address → cancel the editor. The underlying summary has changed from “Profile not loaded” to “No profile published yet” without any successful backfill or publication. The ordinary caption also loses the fetch error when the address result completes. This is unknown state misreported as absence, not a signing-authority bypass.

**Executed evidence:** an isolated diagnostic linked the unchanged production panels DSO and reused the existing test fake-service/profile-host helpers. After one failed FETCH and one successful address-check callback it printed `FETCH=1 (failed); VERIFY_NIP05=1; SAVE=0; summary=No profile published yet`; the assertion that the summary remained “Profile not loaded” failed, exit **134**. This is a real-widget reproduction with controlled host results, not a live network reproduction. The production worker separately confirms that address checking returns a profile result without discovery. The shipped profile-panel test covers failure → successful FETCH, but not this intervening action (`gnome/grotto-panels/tests/test-profile-panel.c:71-85`).

Keep editing enabled after a fetch error, but preserve discovery failure across address/image-only results. Only a result that actually establishes fetched or published metadata should clear that state. No additional confirmation or permission class is needed.

### F2 — S2, confirmed/static — Owner-policy blocker: panels are changed to 0.2.0, not retained at unreleased 0.1.0

**Where:** `gnome/grotto-panels/CMakeLists.txt:3`, `gnome/grotto-panels/meson.build:2`, `VERSION_MANIFEST.md:19,36`; consumer requirements at `apps/grotto/CMakeLists.txt:12` and `gnome/nostr-settings/CMakeLists.txt:16`.

Both authoritative panel build versions, the manifest and package versions are changed to **0.2.0**. Grotto and Settings also require the 0.2 panels package. These values agree internally, and the release columns still say Unreleased; nevertheless they contradict this review's explicit owner policy that the additive panels ABI remains **0.1.0 unreleased**. The older bead's MINOR decision does not supersede that direction.

**Failure scenario:** publishing/building this branch as submitted declares a different panels version than authorized; a system-panels configure against the intended 0.1 package is rejected by the new 0.2 minimum. This is a version/build-contract mismatch, **not evidence of an ABI break**. Reconcile the panel version, dependent minimums, package metadata and API “Since 0.2” comment with the authorized unreleased policy, or obtain an explicit owner policy change before landing. No version was altered by this review.

## Owner-policy and regression checks

### What actually bounded “Loading profile…”

- **EOSE handling was already present.** At the base, `apps/grotto/src/grotto-profile-service.c:312` already sets both `complete` and `done` on matching EOSE; the current equivalent is `:315-316`. The diff does not repair missing EOSE dispatch. It replaces a per-relay/per-round 15-second watchdog, including AUTH extensions, with one monotonic discovery deadline shared across both rounds (`:478-505`), and bounds the AUTH signing RPC by the remaining discovery time (`:148-164`).
- The other material fix is terminal UI state: previously the fetch-error branch never set `fetched`, so Edit Profile… stayed disabled and later maps/model renders could request FETCH again. The new branch sets `fetched` and records failure (`grotto-profile-view.c:180-185`), stopping those automatic retries (`:366-390`) while permitting deliberate editing/refresh. The spinner is colocated with the loading text and follows `busy` (`:110-114,436-443`). Merely shortening a timer would not have fixed this UI-state problem.
- **The timer does not manufacture successful empty backfill.** Its callback sets failure/done, never `complete` (`grotto-profile-service.c:339-344`). With no kind-0 result, attempted discovery succeeds only when every attempted relay completed (`:510-515`); CLOSED/AUTH rejection and timeout remain errors. This respects the [NIP-01 EOSE boundary](https://github.com/nostr-protocol/nips/blob/master/01.md#from-relay-to-client-sending-events-and-notices). It is an operational failure deadline, not “sleep means history is complete.”
- Through the real profile-service entrypoint, empty EOSE completed in **0.007 s**; an EOSE-withholding relay returned an explicit unreachable error in **10.014 s**; discovery disabled with no relays returned the intentional no-relays result in **0.001 s**. A normal failed fetch renders “Profile not loaded,” not the empty-state text. The no-relays result has separate guidance to enable/configure relays; it is not evidence of a global search. These measurements bound these fixtures, not the entire FETCH including image downloads/address verification. F1 is the identified later UI-state exception.

### Profile publication and bootstrap boundary

- Edit Profile… still opens the existing editor; Save delegates through the host (`grotto-profile-view.c:197-221`). `SignerWindow` checks management lock, the selected signable/non-watch-only identity and daemon writability before forwarding (`apps/grotto/src/ui/signer-window.c:846-863`). Discovery, signing and publication are not implemented in the panels DSO.
- Save still discovers first, refuses stale profile overwrite, calls the existing signer and waits for the publisher's relay verdict (`grotto-profile-service.c:616-658,743-768`). `SignProfileEvent` still validates kind/identity/destinations and submits the existing `SIGNER_TRANSPORT_MANAGEMENT` review (`apps/grotto/daemon/grotto_control.c:888-947`). The real-daemon, one-review publication integration case passed. No new always-confirm class, per-kind re-prompt or automatic approval path is introduced.
- Bootstrap remains read-only: tightly author/kind-scoped REQ filters with `limit:1`, then CLOSE; advisory NIP-11 probing is separate. Bootstrap has no signer and rejects AUTH/CLOSED auth-required responses (`grotto-profile-service.c:126-130,232-243,269-270,317-319,363-366`). Existing integration assertions for both FETCH and SAVE require **zero signature requests and zero AUTH frames** on bootstrap challenge/refusal (`apps/grotto/tests/test-profile-integration.c:320-343`); they passed. This review does not recommend signing bootstrap AUTH to suppress an error.

### Default identity, dismissal and shared panels

- **Use as Default** directly invokes the existing `GROTTO_PANEL_SET_DEFAULT_IDENTITY` command (`gnome/grotto-panels/src/grotto-identity-panel.c:110-124`), which the Grotto identity adapter sends through `SetDefaultIdentityForLegacy` (`apps/grotto/src/grotto-identity-service.c:112-145`). The real integration clicks that button, observes Default and checks the shared persistent default setting (`apps/grotto/tests/test-profile-integration.c:500-508`).
- Indicator propagation is model-driven, not a local clicked-button patch: the adapter rebuilds **every** identity's `is_default` against the single configured key and splices the whole model (`grotto-identity-service.c:58-89`); it listens for `changed::default-identity` (`:193`). Every mounted identity panel subscribes to that model and rerenders its Default label/button (`grotto-identity-panel.c:175-182,205-220`). Thus old and new identity details update from the same source. This all-identities propagation is source-confirmed; the executed real-daemon button assertion covers one identity, not a separate multi-identity desktop click-through. Other Identities rows still navigate to these details; no new row-level shortcut is claimed.
- Dismissal is **host-side and per identity**, using relocatable `org.nostr.Grotto.IdentityReminder` settings at a SHA-256-of-public-key path (`apps/grotto/src/grotto-panels-control1-service.c:507-527`; schema at `apps/grotto/data/org.nostr.Grotto.gschema.xml:284-290`). It is not a shared read/modify/write dismissal list. Both adapters observe preference/writability changes; missing/unwritable schema refuses persistence. The library receives only an immutable cosmetic boolean; backup acknowledgement remains separate.
- The new command is classified MANAGER, has no daemon mutation method, validates a known identity and saves only that preference (`gnome/grotto-panels/src/grotto-panels-service.c:86-90,114-120`; `apps/grotto/src/grotto-panels-control1-service.c:325-342`). The manager adapter's test verifies recreation, identity isolation, independent writers, unchanged mutation-RPC count and unchanged `backup_acknowledged` (`gnome/grotto-panels/tests/test-control1.c:228-252`); it passed. Grotto's real keyfile-backed integration also verifies dismissal/remount and the unchanged “Not backed up” row.
- Nostr Settings mounts the same panels through its manager adapter. Its host callbacks only navigate to Grotto for security/profile/backup actions; no profile-signing or trusted command callback is installed (`gnome/nostr-settings/src/ui/nss-grotto.c:35-44`). The backup row's visibility/status and deliberate backup navigation are not replaced by dismissal (`grotto-identity-panel.c:168-171`). Unknown manager-host backup metadata stays explicitly unavailable, not acknowledged. A successful Settings build and the manager-adapter test support this boundary; a Settings desktop interaction was not run.
- The public metadata struct and existing enum values are unchanged. New item constructor/accessor functions are additive, and the new capability uses one reserved interface slot. An old host's `can()` is never called with the new enum value (`gnome/grotto-panels/src/grotto-panels-service.c:114-119`); the executed admission/compiled-boundary check includes that legacy-host assertion. SONAME remains `libgrotto-panels-0.so.0`. This does not excuse F2's version-policy mismatch.

### HIG, templates and localization

- The count caption and its C binding are removed from both Blueprint and committed UI. Default Identity is now a direct action row rather than an expander; the Default indicator uses text, not color alone. “Use as Default” is correctly header-capitalized and immediate; “Edit Profile…” retains its ellipsis because it opens further input. Empty/error subtitles use sentence case. The icon-only dismissal has an explicit accessible name and tooltip. These choices follow [GNOME writing guidance](https://developer.gnome.org/hig/guidelines/writing-style.html#capitalization) and [ellipsis guidance](https://developer.gnome.org/hig/guidelines/writing-style.html#ellipses); the backup banner retains an [informal heading](https://developer.gnome.org/hig/patterns/feedback/banners.html#guidelines). No wording/capitalization blocker was found in the changed controls.
- Pinned Blueprint **0.22.2** regeneration/drift checks passed for both Grotto and panels. All six shipped `.po` catalogs passed `msgfmt --check --check-format`; an explicit scan found no fuzzy or obsolete entries. The changed Spanish strings include “Usar como predeterminada,” “Predeterminada,” “Descartar recordatorio de copia de seguridad,” and distinct empty/not-loaded messages. No wrong retained fuzzy match was found in the changed strings. Catalog syntactic validity is not a claim of complete translation: the newly extracted app-count plural remains untranslated, and that wording already existed in the base source.
- `ui-layout` and Spanish `ui-accessibility` passed serially, including their existing keyboard/layout/accessibility assertions. No fresh manual visual review, Orca session or new screenshot acceptance was performed.

## Verification actually run

Fresh build in `nostrc-linux-ci:arm64`, image ID `sha256:3d1c8f2383c7159a44e3c8f1714476ca749333d34c887b780d6efc144eab35e2`; Ubuntu 24.04, `aarch64`, GTK **4.14.5**, libadwaita **1.5.0**, Blueprint **0.22.2**. Installed `xdotool` and `libxdo3` in the disposable container only. Source was mounted read-only at `/src`; the build/receipts were mounted at `/review` from this worktree's ignored `_build/identity-panel-review`.

Configuration used Debug/Ninja, `BUILD_APPS=ON`, `BUILD_GNOSTR_APP=OFF`, `BUILD_GROUNDHOG=OFF`, `BUILD_TESTING=ON`, `BUILD_SIGNER_TESTS=ON`, `BUILD_NATIVE_HOST=ON`, `NIP55L_SECRET_BACKEND=libsecret`, `GROUNDHOG_STORE_KEY_DEFAULT_BACKEND=secret-service`, `BUILD_RELAYD=OFF`, `SIGNET_ENABLE=OFF`, `ENABLE_TCP_IPC=ON`, `NIP55L_TEST_TRUST_ENV=ON`, `ENABLE_NOSTR_SETTINGS=ON`. Explicitly built only `test-ui-layout`, `test-profile-integration`, `test-grotto-panels-smoke`, `test-grotto-panels-admission`, `test-grotto-panels-control1`, `nostr-settings` and their prerequisites, with build parallelism 2. Building the smoke executable did not run its excluded aggregate test.

The exact requested execution was serial:

```sh
xvfb-run -a ctest --test-dir /review/build -j1 -V --no-tests=error \
  -R '^(signer/(ui-layout|ui-accessibility|profile-integration)|grotto-panels/(profile-panel|management-panels|no-secrets|control1-service))$'
```

| Command/check | Actual result |
|---|---|
| `grotto-panels/profile-panel` | PASS, 1.47 s |
| `grotto-panels/management-panels` | PASS, 0.25 s |
| `grotto-panels/no-secrets` | PASS, 4.01 s, including admission and legacy-command checks |
| `grotto-panels/control1-service` | PASS, 0.03 s |
| Required `grotto-tests-test-homes` fixture | PASS, 0.10 s |
| `signer/profile-integration` | PASS, 12.39 s, including discovery timing, bootstrap refusal, default/dismissal and real-daemon one-review publication |
| `signer/ui-layout` | PASS, 58.79 s |
| `signer/ui-accessibility` | PASS, 60.31 s |
| Requested CTest invocation overall | **8/8 PASS** including fixture; exit 0, 137.39 s; no skipped tests |
| `python3 apps/grotto/tests/test-blueprint-drift.py apps/grotto` | PASS, compiler 0.22.2, both components |
| `msgfmt --check --check-format -o /dev/null <catalog>` for all six shipped catalogs; fuzzy/obsolete scan | PASS |
| Review-only `dbus-run-session -- xvfb-run -a /review/empty-state-probe`, `G_DEBUG=fatal-warnings GTK_A11Y=test` | Assertion **FAIL**, exit 134, confirms F1; diagnostic compile passed |
| `git diff --check ca1a0b746...1ec85fbfb` | PASS |

Local receipts: `_build/identity-panel-review/{configure.log,build.log,container-deps-update.log,container-deps-install.log,focused-tests.log,blueprint-drift.log,catalogs.log,catalog-flags.log,probe-build.log,empty-state-probe.c,empty-state-probe.log}`. A nonessential initial `git config --global` invocation inside the container emitted an inaccessible host-worktree `.git` path error; it was not needed by the build/tests and is not counted as a passing check. The compile and test results above are independent of it. The diagnostic's failing receipt is retained, not retried to green.

Reading aids: `rpce-jev find-lines` selected source spans; partial hits were widened to trace actual callers. One guessed Settings path was absent, then resolved with repository path search. Current-run totals: 18 calls, 17 successful, 36 API requests, 262,892 input tokens, $0.011041. These judgments selected reads only; findings were checked against source and execution.

No full suite, affected-lane gate, sanitizer run, native/macOS keychain test, live public relay, hosted CI, package installation or release acceptance was run. `grotto-panels/smoke-host` was deliberately excluded as instructed (`nostrc-qdgge`), not reclassified as a branch failure. No guard, timeout or suppression was weakened. Test buses/homes were private; no developer keychain command was run. Other agents' containers and worktrees were not touched.

## Version and delivery decision

This report is documentation-only: **no bump** for Grotto, grotto-panels or Nostr Settings; no other independently releasable component is changed by the review. The implementation declares Grotto/panels/Settings 0.2.0 with unreleased columns. Grotto and Settings versions were inspected, not changed; the current explicit panels 0.1.0 policy takes precedence over the implementation's historical MINOR rationale (F2). No release tag or release column was altered.

Delivery is this report committed and pushed on the existing `grotto/identity-panel-ux` branch, not a merge to master. The scoped implementation remains unchanged. The unmerged branch/worktree are preserved; only this review's container and disposable build outputs are eligible for cleanup.

## Merge recommendation

**Request changes.** Preserve the bounded EOSE-aware behavior, direct default action, host-owned cosmetic dismissal and reduced layout. Correct the address-check/failed-discovery state transition (F1), and reconcile the panels version/minimums with the current owner policy (F2). Then rerun the directly affected profile-state checks and version/build consistency checks. No new approval ceremony, bootstrap authentication, repository-wide audit or accepted-smoke-baseline repair is warranted by these findings.
