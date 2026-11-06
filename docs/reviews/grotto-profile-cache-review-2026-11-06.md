# Profile cache and identity avatars: behavior and boundary review

Date: 2026-11-06. Review task: `nostrc-byhwg`; implementation: `nostrc-u6v55.56` and `nostrc-u6v55.58`.

## Boundary and verdict

Reviewed `git diff origin/master...a9ebcdbcb`, with `origin/master` frozen at `96e96a4cdd5edab5ab5f16ea7e92994a37829994` and reviewed HEAD `a9ebcdbcb7b46a80b20fc69b76a30c8b71d06d0e`.

- `git log --first-parent origin/master..a9ebcdbcb` identifies implementation commits `b863ce91c` and `a9ebcdbcb`, plus merge `eea52d2b2` (parents `b863ce91c`, `96e96a4cd`). The merge-base is exactly `96e96a4cd`.
- Scope is the branch-owned cache/service/view/avatar changes, their tests and catalog regeneration: 19 files, +1160/-479. Upstream profile layout/About changes brought in by the merge are not attributed to this branch.
- Worktree throughout: `/Users/bizarro/Documents/Projects/.repoprompt-worktrees/nostrc/rp-agent-6cab3792-grotto-profile-cache-avatars-1838d0f7`, branch `grotto/profile-cache-avatars`.
- This is report-only. No production code, tests, catalogs, templates, version sources or acceptance guards were changed. Additional probes were built outside the tracked tree against production code.

**Verdict:** cache-first rendering, signed-cache validation, newer-event retention, texture reuse/bounds and cache-only Other Identities rendering are substantially implemented and verified. **Request changes before merging unchanged:** fresh metadata suppresses downloads even when images were never cached and the user has re-enabled them (F1). Six of the seven requested tests passed; accessibility completed its three cases but trapped during teardown. A fresh-home debugger diagnostic subsequently exited normally, which does not erase that failed CTest receipt or establish its cause.

Evidence labels: **confirmed/executed** means run during this review; **confirmed/static** means traced through source and its production caller; **unresolved** means observed but not attributed to a branch regression. S2 denotes a user-visible behavior defect, not exploit severity. File references are repository-relative at the reviewed HEAD. Container/Xvfb and isolated diagnostics are not native desktop, hosted CI, live-service or release acceptance.

## Findings, ranked

### F1 — S2, confirmed/executed and static — Should-fix: fresh metadata prevents loading images that were never cached

**Where:** `apps/grotto/src/grotto-profile-service.c:814-833`, especially the unconditional cache-only image lookup at `:829`; background refresh gate at `:1023-1024`. The regular network-capable image path remains at `:864`, with successful metadata freshness recorded at `:891`. The production preference is a plain GSettings binding (`apps/grotto/src/ui/page-settings.c:201-205`); it does not invalidate that stamp or schedule image hydration.

A successful kind-0 EOSE fetch records a fresh 900-second stamp even when `load-profile-images` is off or an image download fails. Subsequent ordinary FETCH operations return early from the metadata cache, calling `download_image(..., FALSE)` regardless of the now-enabled image preference. Background work is scheduled only when the metadata stamp is stale. Consequently neither visiting the profile again nor pressing Refresh Profile can hydrate missing images during that interval. This is a regression from the pre-branch regular FETCH path, which attempted allowed downloads; it is not a failure to enforce the off/Tor privacy paths.

**Failure scenario:** turn off Load Profile Images, fetch a profile containing valid HTTPS picture/banner URLs, turn the preference on, and refresh the profile before fifteen minutes have passed. Both images remain absent even though the setting now allows loading them. A transient image-server failure after a successful metadata fetch has the same retry suppression, by the same source path.

**Executed reproduction:** an external copy of the existing loopback-TLS image integration fixture added an ordinary FETCH immediately after its images-off case, with the preference enabled and Tor disabled. It reported `metadata published=1 picture=(nil) banner=(nil) HTTPS contacts=0`; the assertion requiring the picture failed, exit 134. The baseline fixture's off/Tor, redirect and image-limit cases passed in the unmodified requested integration test. The transient-server variant is source-confirmed, not separately executed.

**Correction direction:** retain the metadata EOSE TTL and immediate cached rendering, but allow permitted image-only hydration/retry for missing textures independently of metadata freshness. Keep FETCH_CACHED strictly network-free and preserve off/Tor cutoffs, redirect limits and service-delivered live updates. No correction was made in this review.

## Owner-policy and regression checks

- **Cache-first production wiring — executed/static:** `signer-window.c:865-869` binds the host service and panel; `grotto-profile-view.c:149-168,177-250,515-547` consumes service snapshots, probes FETCH_CACHED before a cold FETCH, and listens for `profile-updated`. The real-host integration test observed cached labels and both textures on GTK frames without Loading/spinner frames and without relay/image contacts while fresh. Panel tests cover warm/disk-only entry, delayed older callbacks, identity changes during held FETCH, drafts and adaptive remaps.
- **Staleness cannot be renewed by a failed cached FETCH — executed/static:** freshness uses the successful-fetch stamp, not event `created_at` (`grotto-profile-service.c:113-141,814-817`). `remember_fetch` is guarded by `online` at `:891` and refuses cancellation/no-relays at `:128`; merely returning cached metadata does not call it. A supplemental copy of the real-daemon offline-refresh integration case asserted that the aged stamp was still not fresh after the background failure: PASS. The normal integration also proves fresh network silence for an event ninety days old, a stale return before relay contact, and renewed freshness after EOSE. Missing/invalid/future stamps are stale by inspection. F1 concerns missing-image retry, not a failed relay refresh resetting this clock.
- **Signed cache and identity binding — executed/static:** `grotto-profile-service.c:71-88,563-577` requires signed deserialization, canonical-ID/signature validation, the requested kind and the identity's decoded public key on disk reads. `libnostr/src/event.c:606-671` confirms this reaches canonical hashing and Schnorr verification, not merely JSON parsing. Supplemental real-service FETCH_CACHED probes accepted a valid signed cache file and rejected changed content, a missing signature and a valid event from the wrong identity: all PASS. A cache miss is not invented unpublished metadata.
- **Newer-event wins — executed/static:** `grotto-profile-service.c:371-384,90-110,792-799` selects by timestamp with the lower-event-ID tie break and re-reads the disk winner after discovery. `grotto-panels-service.c:132-158` and `grotto-profile-view.c:153-158,192-197` reject older published snapshots and use the service winner. Integration verifies a newer event survives an old relay response on disk and in the visible view; panel tests verify delayed old view/avatar callbacks cannot regress the display. No older-timestamp overwrite was found.
- **Texture LRU and identity safety — executed/static:** the host-window-owned LRU at `grotto-profile-service.c:19-28,613-643,1052-1060` is mutex-protected, keyed by full URL, promotes hits, and evicts until both 32 entries and 32 MiB of estimated RGBA storage are satisfied. Oversized textures are returned to the caller but not admitted. A supplemental probe of these exact production functions verified 33rd-entry eviction, promotion, byte-budget eviction with three 16-MiB textures, and oversized non-admission: PASS. The separate service display cache is also bounded (`grotto-panels-service.c:98-158`); these are cache limits, not a total-process RSS/live-widget limit. Real-service probes verified that two signed identities sharing a URL reuse the same texture without swapping their metadata, and changing the signed picture URL selects a different cached image: PASS. Normal integration verifies switch-back reuse after image bytes are removed.
- **Other Identities has no render-time disk/network I/O — executed/static:** `grotto-identity-panel.c:126-147,219-234` uses AdwAvatar custom images from service snapshots and initials while absent. Its only fallback is asynchronous FETCH_CACHED through the panel/host boundary. The host dispatches the worker at `grotto-profile-service.c:1069`; the worker exits the cache-only path before D-Bus/discovery/HTTP at `:814-839`. The row callback re-reads the service winner. The focused avatar test verifies initials, cached images and delayed-reply protection; service probes exercise actual disk cache reads. No synchronous disk/network call was found in the new list-rendering path.
- **Bootstrap W7b — executed/static:** the scoped Nostr-protocol-smells review found no new polling or timeout-as-success behavior. Freshness is visit-driven, background discovery retains author/kind/limit REQs and the explicit EOSE completion boundary (`grotto-profile-service.c:361,400-439,516-561`). Bootstrap fetches receive no signer, and AUTH/auth-required CLOSED are refused before signing; ordinary identity-relay auth remains distinct. The requested integration exercises cold AUTH/CLOSED refusal and no bootstrap signing/publishing. Bootstrap provenance is copied from the stamp; publication destinations still come from verified kind 10002 or configured identity relays, never bootstrap fallback.
- **Off/Tor and .33 image restrictions — executed/static:** unmodified integration passes both preference-off and Tor cases with no additional image contacts while retaining cached bytes. `grotto-profile-service.c:807-812,858-864` retains the network/image-setting guards. Download code still validates HTTPS at every hop, refuses downgrade, follows at most five redirects, limits bodies to 5 MiB and decoded rasters to 8192 pixels per side/24 megapixels (`:579-611,645-690`). NIP-05/NIP-11 policy was not changed. F1 is the re-enable/missing-image exception.
- **Panels boundary and .36 states — executed/static:** panels receive public GrottoProfileInfo/textures through GrottoPanelsService and existing host callbacks; no secret/policy API or renderer-side I/O dependency was added. Both no-secrets and Control1 tests pass. `grotto-profile-view.c:88` still distinguishes successful unpublished metadata from not-loaded/failed state; focused panel tests cover both, and integration tests cover EOSE-confirmed absence versus cold AUTH/CLOSED/error. Negative freshness is recorded only after actual discovery completion and never merely for no-relays/Tor/failure. No cold failure falsely presented as a successful unpublished profile was found.
- **Catalogs and Blueprint — executed:** msgfmt --check --check-format passes for all five Grotto catalogs and panels Spanish; no fuzzy entries are present. The branch's new cache-miss string is gettext-marked; incomplete pre-existing translations are not claimed complete. Pinned Blueprint 0.22.2 drift passes for production templates, including panels and the kind catalog. There is no branch-owned .blp/.ui delta. No manual visual/Orca acceptance was performed.

## Verification actually run

Linux image: `nostrc-linux-ci:arm64`, ID `sha256:3d1c8f2383c7159a44e3c8f1714476ca749333d34c887b780d6efc144eab35e2`, arm64. The assigned worktree was mounted read-only at `/src`; a task-owned build volume was seeded from the existing read-only HIG build, reconfigured and the focused targets rebuilt. xdotool was installed as root only inside the disposable container. Tests ran serially with private buses/test homes; no developer keychain commands were run.

| Command/check | Actual result |
|---|---|
| `scripts/install-hooks.sh` | Static pre-push hook already installed; no bypass requested |
| `cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_GNOSTR_APP=OFF -DBUILD_GROUNDHOG=OFF -DBUILD_TESTING=ON -DBLUEPRINT_COMPILER=OFF -DNIP55L_SECRET_BACKEND=libsecret -DGROUNDHOG_STORE_KEY_DEFAULT_BACKEND=secret-service` | PASS; Groundhog backend option unused because Groundhog was disabled |
| `cmake --build /build -j4 --target test-profile-integration test-ui-layout test-grotto-panels-smoke test-grotto-panels-admission test-grotto-panels-control1` | PASS; 393 build steps, production libraries/daemon prerequisites rebuilt as needed |
| `xvfb-run -a ctest --test-dir /build -j1 -R '^(signer/(profile-integration\|ui-layout\|ui-accessibility)\|grotto-panels/(profile-panel\|management-panels\|no-secrets\|control1-service))$' --output-on-failure --output-junit /evidence/focused.xml` | **FAIL, exit 8:** profile-integration, ui-layout and all four panels checks PASS; test-home fixture PASS. ui-accessibility SIGTRAP during teardown after its three test cases reported success. 7/8 including fixture, 160.04 seconds; not an all-green requested run |
| Supplemental real-service signed-cache/shared-URL/URL-swap probes and direct production LRU probes, `/evidence/review-profile-integration -p /review` | PASS, 2/2; diagnostic compilation includes the unchanged production service implementation plus an observer/test function, not a product fix |
| Supplemental real-daemon offline integration with failed-refresh-stamp assertion | PASS, exit 0; no renewed freshness after failed background refresh |
| Supplemental preference re-enable TLS fixture, `/evidence/review-images -p /signer/profile/images-autoload-https-redirects` | Expected assertion **FAIL**, exit 134; confirms F1, not a passing product test |
| `msgfmt --check --check-format -o /dev/null <catalog>` for all shipped Grotto/panels `.po`; fuzzy-entry scan | PASS; no fuzzy entries |
| `python3 /src/apps/grotto/tests/test-blueprint-drift.py /src/apps/grotto` | PASS; pinned 0.22.2 production-template/kind-catalog drift checks |
| Fresh-test-home diagnostic of ui-accessibility under `gdb -batch -ex run -ex 'thread apply all bt'` and Xvfb | Three cases PASS and inferior exited normally. Diagnostic/instrumented execution only; does not supersede the original CTest failure |
| `git diff --check origin/master...a9ebcdbcb` | PASS |

**Accessibility failure, unresolved:** the original process ended with `GLib-GIO-FATAL-CRITICAL: Error while sending AddMatch() message: The connection is closed`, after all three layout cases. Teardown closes the private bus at `apps/grotto/tests/test-ui-layout.c:1295`, an unchanged fixture line. This review did not establish whether the failure is branch-caused or an inherited asynchronous teardown race; it is not silently classified as the accepted smoke-host baseline. An initial debugger attempt incorrectly reused the consumed test home and failed earlier at the duplicate-account assertion (`:389`); after explicitly running the test-home fixture, the debugger run exited normally. Both diagnostics and the original failure are retained, not presented as retry-to-green CTest acceptance.

Local evidence: `/tmp/grotto-profile-review-byhwg/` contains `apt.log`, `configure.log`, `build.log`, `focused.{log,xml}`, `catalogs.log`, `blueprint.log`, `cache-and-lru.log`, `failed-refresh-ttl.log`, `image-enable.log`, both `a11y-debug*.log`, fixture-reset output and the external probe sources/build drivers. Initial probe compilation failures (missing relocated-test include path, then incorrect object-target selection) are retained alongside the successful build log; neither was a production build failure. The assigned source was unchanged while tests ran. The task-owned container/build volume are removed after retaining evidence; shared seeds and this unmerged branch/worktree are preserved.

The full suite, full affected-lane matrix, sanitizers, native desktop/Orca, live relays, owner acceptance and release qualification were not run. `grotto-panels/smoke-host` was deliberately excluded as requested (`nostrc-qdgge`); its baseline crash is neither rerun nor counted as this branch's failure. No timeout, assertion, platform guard or suppression was weakened. Jev relevance queries guided function-range reads; narrow snippets were widened where lifecycle/state flow required owning functions (26 requests, approximately USD 0.0093).

## Version and delivery decision

This review changes documentation only: **no bump** for Grotto or grotto-panels and no other independently releasable component changed. The implementation adds public cache APIs/an appended action enum while the owner's explicit instruction folds this unreleased work into existing Grotto **0.1.1** / grotto-panels **0.1.0**. Those versions agree in `VERSION_MANIFEST.md:34-35`, Grotto CMake/Meson and panels CMake/Meson; no version source, release column or tag was edited. The report is committed and pushed on the same feature branch, without merging it or modifying main/siblings. Review scope, results and version decisions are recorded in `nostrc-byhwg`; `bd dolt push` reported no configured remote and skipped synchronization, so tracker updates remain local/shared, not remotely synchronized.

## Merge recommendation

**Request changes before merging unchanged.** Correct F1 with image-only hydration/retry while retaining cache-first rendering, the metadata TTL, off/Tor behavior and cache-only identity-list semantics. Rerun its directly affected profile integration coverage. Also obtain a clean ordinary accessibility execution or explicitly triage/disposition the retained teardown failure; the successful instrumented run is not its replacement. No cache-signature bypass, older-event regression, LRU-bound violation, cross-identity metadata leak, bootstrap-signing regression or new panel authority transfer was found. Do not expand this into a full-suite qualification exercise or treat the accepted smoke-host baseline as a new prerequisite.

## Re-check (fbd2fccae)

Date: 2026-11-06. Re-check task: `nostrc-x0blw`. Reviewed HEAD: `fbd2fccae6ff67fc8ecc1c67c048b500ec3ea711`, obtained with `git pull --ff-only` in the same worktree/branch. The relevant changes since this report are image hydration `d137c7d5f` and late-cache texture preservation `fbd2fccae`; merge `115c5fff9` includes master `561a52c97`. This re-check is limited to F1, its immediate downstream behavior and the seven requested checks, not a new review of the merged master work. Earlier findings and failed receipts above remain intact.

### F1 disposition — resolved, confirmed/executed and static

`apps/grotto/src/grotto-profile-service.c:814-845,1015-1040` now distinguishes a fresh-metadata image-only job from a stale metadata refresh. Missing images schedule background hydration only for ordinary FETCH when downloads are allowed; the worker rechecks `network && load_images` before HTTP (`:833-834`). FETCH_CACHED cannot schedule hydration. The image-only path returns before discovery and freshness-stamp writes, and background jobs cannot recursively schedule another hydration attempt.

The unmodified real-host loopback-TLS integration case `/signer/profile/images-reenable-and-retry` (`apps/grotto/tests/test-profile-integration.c:417-508`) executed and passed. It verifies:

- A successful EOSE fetch with images off leaves both textures absent and makes **zero** HTTPS image contacts. A deliberately fresh 100-second-old metadata stamp is then used for the remaining checks.
- With fresh metadata and missing images, another ordinary profile refresh with the preference off still makes **zero** image contacts. Tor mode with the preference on also makes **zero** contacts.
- FETCH_CACHED remains image-network-free even after downloads are enabled.
- Re-enabling downloads and activating the real profile view's Refresh Profile action loads **both** avatar and banner through the service and into visible widgets: **two** HTTPS contacts, **no additional relay REQs**, and the freshness file remains **byte-for-byte unchanged**. Cached rendering remains spinner-free. This verifies completed asynchronous hydration, not an assumption that the initial cache-return callback must synchronously contain downloaded images.
- A separate case first receives two HTTP 503 responses, then successfully retries both images while metadata is still fresh (four contacts total). A subsequent FETCH reuses both textures without another image contact.

Late same-revision cache replies also cannot erase the hydrated textures: `gnome/grotto-panels/src/grotto-panels-service.c:139-151` preserves them only for unchanged image URLs, and both view completion paths consume the service winner through `grotto-profile-view.c:160-166,189-199`. The executed profile-panel regression holds cache-only/FETCH replies until after hydration, checks retained textures, and checks that changed URLs do **not** retain the old images (`gnome/grotto-panels/tests/test-profile-panel.c:404-426`). No new finding was identified in this scoped re-check.

### Verification actually run

Same `nostrc-linux-ci:arm64` image ID as above; assigned source mounted read-only at `/src`. Installed xdotool in a new disposable container, reconfigured with the same focused Debug/libsecret options, and explicitly rebuilt the same five focused targets (393 build steps). Ran **one ordinary serial CTest invocation**, without debugger, suppression, bypass or retry-to-green:

```sh
xvfb-run -a ctest --test-dir /build -j1 -V \
  -R '^(signer/(profile-integration|ui-layout|ui-accessibility)|grotto-panels/(profile-panel|management-panels|no-secrets|control1-service))$' \
  --output-on-failure --output-junit /evidence/focused.xml
```

| Requested check | Result |
|---|---|
| signer/profile-integration | PASS, 12.99 s; includes the re-enable/retry case above |
| signer/ui-layout | PASS, 76.45 s |
| signer/ui-accessibility | PASS, 76.49 s; ordinary execution, **teardown trap did not recur** |
| grotto-panels/profile-panel | PASS, 2.38 s |
| grotto-panels/management-panels | PASS, 0.32 s |
| grotto-panels/no-secrets | PASS, 4.58 s |
| grotto-panels/control1-service | PASS, 0.03 s |

Overall **8/8 PASS including the required test-home fixture**, exit 0, 173.32 seconds. The initial review's AddMatch failure is still valid historical evidence: this clean execution does not establish its cause or prove it cannot recur. It remains separately tracked, with attribution open, in `nostrc-rmea8`; it is not reclassified as the accepted smoke-host baseline. No full suite or smoke-host run was performed, and catalogs/Blueprint were not separately rerun in this narrowly requested re-check.

Receipts retained at `/tmp/grotto-profile-cache-recheck-x0blw/`: `commit.txt`, `source-sha256.txt`, `apt.log`, `configure.log`, `build.log`, `focused.log` and `focused.xml`. The task-owned container/build volume are removed after retaining these receipts; shared seeds and the unmerged feature branch/worktree remain. No implementation or test was edited. This documentation-only append needs **no version bump**; Grotto **0.1.1** and grotto-panels **0.1.0** remain unchanged.

### Final verdict

**Approve the requested profile-cache/avatar scope at fbd2fccae.** F1 is resolved and all seven requested checks now pass in an ordinary serial run. This supersedes the original request-changes recommendation for this reviewed scope; it does not erase the earlier failed receipt, close `nostrc-rmea8`, or claim native-desktop, hosted-CI, live-service or release acceptance.
