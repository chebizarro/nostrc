# Groundhog ownership, Ninja freshness and readiness: PR25 source receipt

## Landing and scope

[PR25](https://github.com/chebizarro/nostrc/pull/25) merged normally, without administrative bypass, at **2026-10-24T02:56:50Z** as `ab2fddeeae9ab5db7ecd883ff0ec6e85921d791b`.

| Identity | Exact value |
|---|---|
| Pushed head | `257c40473540bde07f27a6a81a62253744412264` |
| GitHub-tested synthetic merge | `27fa54a558f536f97db30ff1eb4a078428c4a703` |
| Tested and landed tree | `d5ff8d3e86fdadffc42ecd42f4ca7838d05a8e64` |
| Both merge parent lists | `077715e2520e1e6ebd4c1c5f3dad32a8e7cdfab5`, `257c40473540bde07f27a6a81a62253744412264` |
| Separately tested native source | `cdaf8de2d111622b48633fcdd718fcc96f1899c9` |

This finite internal CI/test repair is not whole-profile, complete-gate, W11, W12 or release acceptance. No shipped component version bump. Defaults, publisher modes, protections, legacy triggers and schedules are unchanged.

## Delivered changes

1. **Groundhog ordinary ownership (.36.19).** Exactly 46 observed registrations gain `groundhog.linux` projections in nine existing `supported.apps.*` checks. Their normalized descriptors and three support fixtures match same-source MDK08. Six actually consumed nodes join `groundhog.default_build`: `blossom_cache`, `grelay`, `grotto.daemon`, `grotto.gui`, `grotto.native_host`, `relayctl`. The original 316 registration plus 322 default-build edges resolve under the real ownership/prerequisite rules. No path ownership, profile definitions or omission authorities change. See `tests/selection/test_groundhog_app_enrollment.py` and its exact witness.
2. **Named-target Ninja freshness (.36.20).** Raw `ninja -n` hit CMake's forced glob-verification/regeneration prepass before selected targets. Configurations without a default attestation now author `ninja_generation_binding`: native cache, manifests, configured/PATH tool identities and bytes, regeneration inputs and VerifyGlobs control. Named targets reuse the bound verifier and engine-owned entry that only includes the original graph from its original working directory. Re-derivation, exact no-work and pre/post checks stay under the build lock; full source/Ninja/header/runtime/prerequisite audits remain mandatory. No graph filtering, `-d norebuild`, always-dirty exemption or fabricated `all` target. Missing/stale execution bindings refuse. Freshness `RealizationError` refusals retain stage/argv/output; cancellation keeps the distinct cancelled receipt. Query evidence is not physical coverage. Targetless Ninja lanes also bind tools at configure, but run no target-freshness query.
3. **Readiness cleanup/diagnostics (.36.21).** After readiness failure, retain the read end until existing pidfd teardown has stopped/reaped the service; then drain at most 4,096 bytes nonblockingly and close it. Preserve the same failure and 15-second deadline. Record outcome, elapsed time, liveness observation and late data. A controlled late handshake reproduces the old secondary EPIPE and proves this cleanup correction. It does **not** establish why the original hosted Xvfb startup exceeded its deadline. Reserved/control descriptors, cancellation, sibling isolation and cleanup guards remain; late data or exit0 never repairs a failed deadline.
4. **Exact sanitizer drift diagnosis (.36.22), not renewal.** Current serialized contract: **347 nonmembers /65 required-closure registrations**. Fresh hosted universe:413 names, the sole extra `nostr-gtk-resource-build`, introduced by `4ecc6829c` after reviewed source `8d052ca`. Configure equals the legacy sanitizer job; both provision Blueprint. The actual predicate still refuses. Removing precisely that name satisfies the old partition; selecting it changes the 65-entry closure and refuses. No setup workaround, original-193 authority alias, waiver or automatic renewal. The source-linked witness is test-only.
5. **Strict historical integration.** Preserve service/policy inverses and add the exact realization-source inverse in the newest layer, yielding the older post608c/runtime chain's exact expected bytes. Original, idempotent, modified, truncated, duplicated, ambiguous and unknown-source cases are exercised. No generic masking.

## Review and retained attempts

Independent Claude review plus one Oracle call (`untitled-chat-C3111C`) cover base `077715e25` through `cdaf8de2`. The missing source inverse and deeper nonmember-guard coverage were corrected. Final review found no blocking runtime defect or fail-open. Three test-only findings were applied and directly inspected, without another review loop:

- N1: structural validation does not authenticate review provenance. Tests now prove the actual exact digest and historical-list pins reject list-only renewal, while retaining structural negatives. Production validator/authority unchanged.
- N2: change an owned private Ninja executable's bytes after configure, leaving its stored record unchanged; retain record inconsistency as a distinct negative.
- N3: readiness EOF can precede kernel waitability. Assert actual exit/reaping and the retained observation, not instantaneous false liveness.

Native/reviewed `cdaf8de2` differs from pushed `257c4047` only in three Python regression files: `test_default_producers.py`, `test_native_services.py`, `test_sanitizer_partition_drift.py`. Runtime source and inventory bytes are equal. Native proof is not relabelled.

Preserve earlier branch attempts:266 tests/one exact-message assertion failure;198/one worker-golden failure;628/one untracked-input error; uninitialized-submodule diagnostics; two FD-helper errors; and interrupted discovery/sweeps without terminal summaries. Their causal corrections and focused results remain separate. The frozen hosted result below is not a stitched sum or unchanged retry-to-green.

## Verification by source and scope

### Final hosted Static — PASS

[Run40054881265 / job120755556033](https://github.com/chebizarro/nostrc/actions/runs/40054881265/job/120755556033), attempt1, tested `27fa54a5`. Job **02:36:37Z–02:51:55Z**, 918 seconds, within unchanged1,200-second limit.

- DEFAULT: **1,304 run, zero failures/errors,80 explicit skips**,711.389 seconds;1,224 derived passes.
- Separate actual Linux kernel matrix: **87 passed, zero skips**,52.055 seconds. It overlaps DEFAULT discovery, not additional unique coverage.
- All14 testhost commands exit0. Exact owned-container cleanup and AppArmor teardown succeed.
- Original artifact **12545660563**, **32,359,215 bytes**, SHA256 `ee7d8c325c5767e66d6d39e100b7a5176e7d724fe26efd1f9cb8bb25084a2475`; API digest agrees; all **6,024 member CRCs** independently checked.
- Original `evidence/regression.json` SHA256 `3d164d0f77a5b516a82c19408d5c54f636f26b45a01310087b4bac7c55541cec` binds that synthetic source, with `complete=false`, `complete_gate_eligible=false`, `native_profile_acceptance=false`.
- This regression job is not MDK producer, selected/whole-profile, performance or release acceptance. Its duration is not paired product savings.

### Native — PASS at cdaf, locally incomplete overall

One fresh independent clone and four exact pins configure/build native defaults successfully. Serial full CTest: **439 unique registrations =434 passed +5 explicit skips, zero failures**,472.40 seconds. Fingerprinted Node is present before configure; both web-extension cases pass. Debug build uses `--parallel 2`, `NIP55L_SECRET_BACKEND=libsecret`, `GROUNDHOG_STORE_KEY_DEFAULT_BACKEND=secret-service`. `BUILD_GROUNDHOG=OFF` is not Groundhog profile proof.

Explicit skips: `test_nip5f_tcp`, `signer/qr-scanner`, `signer/onboarding`, `signer/ui-layout`, `signer/ui-accessibility`. CTest-passing `signer/ui` and `signer/approval-dialog` also emit internal SKIP lines; no executed UI coverage.

All three read-only keychain guards exit0. Owned direct private bus exits0, is reaped and its socket disappears; not arbitrary-descendant/native-profile custody. Combined driver returns **1/incomplete**: Darwin is not admitted to the disposable hosted Linux controller, so local DEFAULT/kernel are NOT RUN, never forged or substituted.

Parent verifies **9,183 manifest entries /9,157 files /540,814,808 file bytes**, modes/symlinks, **6,538 immutable Git blobs** across source/pins, and original439-case JUnit. Summary SHA256 `a5d9904088721f0fe947d79d4cebaa576f91440d311e23d2e9ede8a30bd09133`; manifest `2b64df4a1459e1c58fbd015e49a3c9fea6d790ee824fac922aeca7cf983a2486`; named accounting `d6d197af3eb3c57a27ff932b2976ca5a7269524df7df8f120b098382daddbe59`.

### Focused results

Inverse/enrollment reconciliation:132 run, seven explicit Linux-only skips. Freshness/nonmember review-gap modules:63 passed at their earlier named test source; final private-tool correction:four focused targetless cases pass. Corrected sanitizer pin/ownership cohort:30 pass, with neutered-pin negative control failing. Final readiness cohort at `b8c8575e3`:15/15, no skips, one run on LinuxKit7.0.14 aarch64, not hosted x86/profile proof. Both exact owned Docker CIDs independently confirmed removed; pre-existing images/networks/volumes not pruned.

## Dated acceptance and remaining criteria

**2026-10-24T02:59:17.608254Z** PR25 snapshot, not a live all-history counter: core Linux/Mac ASan preaudits still refuse exactly `MockRelayUnit`, `concurrency_subscription_shutdown_asan`, `test_json_envelope`. Fresh original artifacts retained. Sanitizer partition still refuses. Standalone libgo Linux/Mac ASan and homed-units jobs pass bounded routes, not native-root/legacy-homed parity. Core ordinary and Grotto ASan fail; Grotto ordinary, Groundhog ordinary and MDK08/011 still run at that snapshot. Later outcomes require source-linked receipts, never guesses or retries to green.

`.36.19`/`.36.20` source repairs landed; specifically required supported-profile execution remains separate from Static/native fixtures. `.36.21` retains original hosted startup-cause proof. `.36.22` retains explicitly reviewed current-source treatment and physical acceptance. `.36.18` credential delivery scope, `.36.16` three true ASan exclusions, original MDK producer/use/cleanup/sanitizer parity, native authority and broader profile union remain open.

Reviewed acceptance remains **0/20 qualifying representative PR comparisons,0/2 successful complete broad integrations, zero real product samples**. PR25 is CI-control work, not a representative product comparison. Preserve seven workload classes, five clean/incremental pairs each, median/range/IQR, Grotto≥30% and non-MLS Groundhog≥25% runner-minute gains, zero unrelated MDK, and original critical-path/broad limits. W11 NOT MET / W12 NOT RUN. No cutover/release authority.

## Post-merge original-artifact review

The later PR25 originals verify progress without manufacturing a profile pass:

- **Grotto ASan, job120755612941, artifact12545341395:** exact no-work query succeeds, full input audit passes, and338 CTests physically run with zero skips. There are58 failing cases:49 LeakSanitizer reports,8 UBSan reports and one GstPlayer failure. Representative evidence: `GoContextCancelTest` leaks33 bytes/two allocations; `libgo/src/counter.c:30` accesses64-byte-aligned `LongAdder` at a misaligned address; `/media/player/host-stream` fails `GST_IS_PLAYER`. These are cases to group by demonstrated cause, not58 invented independent bugs. ZIP375,799,083 bytes,4,043 CRC-checked members, SHA256 `a97c1bfb1d3c8370752bfde4888dbd8754c487c61f9aa66cea623a55efcfa8c1`; API digest matches.
- **Core ordinary, job120755612969, artifact12545079555:** default and named no-work records succeed, full input audit passes,441 CTests run/zero skips. Four loader failures remain: image-viewer-remote-media lacks `gnostr_main_window_show_toast`; delete-authorization, ndb-main-thread-violations and real-bind-latency lack `gnostr_mute_filter_should_hide_fields`. A secondary pre-exit seal mismatch remains strictly refused, not laundered into coverage. ZIP172,180,291 bytes,6,030 CRC-checked members, SHA256 `520af4224e4826e19f2a4f74cd4aca608be6d6eab9fd43108f02f5d08e2efda9`; API digest matches.

Both receipts remain failed with incomplete/eligibility flagsfalse. Grotto's successful query is real support for the freshness repair, not a physical coverage claim for the query, an MDK08 receipt, or successful CTest/profile acceptance. Original failures and names are retained, not retried or hidden.

New finite follow-ups, separate from already-open credential scope and three root-ASan exclusions:

| Bead | Bounded next work |
|---|---|
| `nostrc-0cfsg.36.23` | Repair four Gnostr loader failures, two real symbol/link contracts; no unresolved-symbol waiver or success stub. |
| `nostrc-0cfsg.36.24` | Assign58 sanitizer cases to evidence-backed causal repair cells; preserve flags, failures and explicit case accounting. |
| `nostrc-0cfsg.36.25` | Preserve immutable failed receipts through seal/namespace exit; attach derived failure diagnostics separately, keep tamper refusals. |

### Later terminal receipts: keep inner results distinct from admitted results

At **2026-10-24T03:29:15.825987Z**, all13 selected-worker jobs are terminal:10 failed; only standalone libgo Linux/Mac and homed-units succeeded. This is not a complete workflow/root count or W11 quota.

- **Groundhog ordinary, job120755613098, artifact12545816121:** inner backend reports passed,471 CTests/zero failures, passed full audits and two exact no-work records; every newly mapped app check passes. Nevertheless, the outer source-custody seal rejects original receipt hashes while handling `SystemExit:0`. **No admitted/qualifying profile result follows from that inner success.** This also belongs to `.36.25`, not only failed lanes. ZIP306,954,577 bytes,7,725 CRC-checked members, SHA256 `2c1e93b2e15f97f53842dab951903559db84e0c16a5335f69ecc6ed48e72b33f`, API digest matched.
- **MDK08, job120755613174, artifact12545029305:** input audit and the exact named-target no-work query pass. Original MDK lifecycle cleanup/identity fails after the outer CTest process. The top-level `attempt-1/results.xml` actually contains571 cases/eight failures; `ctest_exit_code=null` is missing finalization, not proof that tests did not run. `test_selection_realization.py:3355–3387` executes CTest inside the resource ExitStack, then assigns its receipt fields after cleanup; `test_selection_mdk.py:280–281` can raise before that assignment. These remain failed, unfinalized diagnostics, never accepted coverage or quota credit. An earlier triage inference called this nested/not-executed from the null field; the exact ZIP path and code disprove that inference. Originals are unchanged. Original MDK `.8`/`.36.13` remains open. ZIP358,228,794 bytes,8,466 CRC-checked members, SHA256 `73576b601ead3bb9b5e1425742fadb14b068d2cd12f619b5604ac2ae5b78da07`, API digest matched.

Thus the false freshness refusal is repaired in both originally named profiles, but broader required execution/custody/parity still fails. `.36.19`/`.36.20` remain explicitly in progress rather than using rejected or nested receipts to close their acceptance. Grotto ordinary and MDK011 also failed at the later snapshot; their uninspected internals are not inferred here. Core's four loader failures, queries and audits above are failed-diagnostic observations; its seal divergence precludes receipt-based qualification.

## Durable evidence

Protected copies, not immutable remote escrow:

- `/Users/bizarro/Library/Application Support/RepoPrompt CE/evidence/nostrc/w11-next-batch-20261024`: original hosted ZIPs/logs, source/landing identities, independent proofs, branch failures, four handoffs and owned-resource records.
- Sibling `final-cdaf8de2d111-composed-native-20261024`: sealed native source/build/logs, JUnit, tools and manifest, labelled cdaf.

See [PR24](dependency-aware-testing-priority-parity-2026-10-24.md), [plan/index](../plans/dependency-aware-testing-ci-2026-10-15.md#execution-index), [acceptance audit](dependency-aware-testing-ci-2026-10-18-acceptance-audit.md), [run ledger](dependency-aware-testing-ci-2026-10-18-run-ledger.md), [criterion register](dependency-aware-testing-ci-2026-10-22-closeout.md) and [measurement protocol](dependency-aware-testing-w11-e-protocol-2026-10-23.md).
