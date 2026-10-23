# W11 concrete closure batch: implementation and bounded acceptance

## Source and authority

Base: `663edcd70c0d313c98f87610aa41f1261bce4ee6`. Preserving merges include the exact approved tips:

| Component | Approved tip |
|---|---|
| Heartbeat | `d232d31b71edc80aae25961d751e077ce914a4ae` |
| Worker admission | `68da0e7b3afe1796bd1aaba2dc1ed22a7ff8863b` |
| Provisioned policy | `742bebdfd7b014a83f763bdd9e2b27318728cbb1` |
| Comparison/core | `11def03f6756f652f95cd6a7463d4ffe4c4d614c` |
| Full ordinary Go | `b5a61cf774afe7fd5708d9417258b751cf0592e3` |
| Strict fuzz dependency remedy | `4a55397bbaf9fc0103068e18d466564ed2bd6fc7` |

Local integration code freeze: `0e3601eab21ca480a7d80c72d6c29ac29155f5aa`. Later documentation/tracker commits are not substituted into earlier source receipts. Parent lands this branch and runs frozen-source hosted proofs using the existing workflows; there is no endpoint, SSH retry, approval request or external enrollment dependency pending. Internal input/profile enrollment is implemented in this batch. No push, W12, publisher, protection, trigger, schedule or default/enforcement cutover is authorized.

Evidence root: `/tmp/nostrc-w11-closure-20261023/final-integration`. `parent-evidence-index.json` binds the parent verification files: H57 artifacts, A3147 artifacts plus6 blobs, F6429 physical source bindings plus59 critical artifacts (not its entire developmental manifest), C936+60 proof entries, C6/G4 blobs and G physical proof, and J25 artifacts. Approved ancestry does not imply that integration-modified files still equal their component blobs.

## Actual execution design

### Policy workload, not a predecessor-only fix

`static.policy` uses the authored `policy_controller_linux_v1` domain and `namespace_policy_controller` capability. Its actual selected command is `python3 {source}/scripts/test_selection_policy.py attach --control {build}/namespace-policy/control`. The parent controller owns the Docker lifecycle; the workload has no Docker socket. It binds the actual Docker/Git/Python executables, source-labelled immutable image and build producer, independent exact Git/Gitlink source snapshot, full postbuild input audit, actual controller argv, private control directory, exact CID/token, pre/post inspect, workload and cleanup bytes. Readiness is not inferred from a Boolean.

The workload is nonroot, cap-drop ALL, no-new-privileges, network none, read-only source/root and private tmpfs. Scoped container security configuration provisions the namespace kernel tests; no developer host policy/sysctl is changed. Docker's actual empty MaskedPaths/ReadonlyPaths representation is checked rather than assuming `systempaths=unconfined` remains in SecurityOpt. All14 original commands and the zero-skip kernel matrix are required. Aggregation accepts only the typed source/tool/producer/audit-bound controller evidence, not predecessor status or raw regression JSON. `policy_regressions` remains a mandatory predecessor but does not replace `policy / execute`.

This is an explicitly authored provisioned policy workload, **not native policy parity**. Provisioning was an orchestrator technical decision within the CI task, not an invented user security-policy waiver. Unsupported platform/architecture or malformed custody still refuses.

### TCP, resources and source closure

Ordinary profiles explicitly configure apps, TCP, test trust, libsecret and Secret Service, with the exact one-shot consent `NOSTRC_TEST_NIP5F_APPROVAL=unidentified-get-public-key-once`. The real consumed CMake export binds `test_nip5f_tcp`, `test_nip5f_approver`, `grotto-daemon`, `grotto-schemas` and generated schema inputs. The fixture owner is consumed by the existing inventory macro, not an ignored custom property.

Full source/Ninja/header/runtime/default and omitted-prerequisite audits precede admission. Actual daemon fixture closure owns Seahorse, relayd async-storage API and the generated NIP55L D-Bus contract; these are fixture-only edges, not new product NIP5F dependencies. Core also enables GNOSTR to match its actually claimed registrations. Core's existing parallelism2 is now explicitly authored as build2/test2, without changing the CLI guard or heartbeat foreground policy.

Only exact `test_nip5f_tcp` is supported. Private identity/token, actual Secret Service readiness and explicit unidentified-caller approval remain mandatory. No ambient HOME/bus leaks into other keyring CTests. Those tests keep their own private fixtures, including initially absent services. `nak`/keyring executable evidence is distinct from resource setup; other signer commands and consumers still refuse. The A signer/profile diagnostic flags remain false for complete eligibility even when the signer check is unselected. Passing one provisioned cell is not permission to clear them.

The public ordinary worker now uses retained-plan `test-gate.sh --native-custody pidns`; it cannot manufacture a receipt from ambient Xvfb/bus setup. Retained plans are exact source-bound lane projections, not replanning aliases. Independent snapshot Git objects and exact recursive Gitlink OIDs avoid trusting a mutable worktree/cache tip. No selected-only audit, prepared/ARM/native alias, or `signer-tests` build substitution is used.

### Full Go and coverage state

`subset.contracts(policy)` retains immutable historical authority. `active_contracts(policy)` excludes only strictly validated current full-owned ordinary33; invalid full metadata refuses instead of falling back to old omission authority. `supported.libgo` owns all33 registrations, including all six W4 variants and their actual environment/timeouts/prerequisites. Full graph validation remains mandatory on unselected Go lanes, but those lanes must not invent Go executions. Selected Go requires all33 physical executions. The original prebuild audit is retained through backend and normalized receipts; full-owned test-only invocation refuses rather than synthesizing prerequisite evidence.

The normalized top and original pre/post audits retain `ordinary_go_full_ownership` and all five state fields: coverage_gaps, physical_coverage_gaps, rollout_prerequisites, enforcement_ready and rollout_readiness. Only explicitly marked `shadow_cutover` is administrative rollout readiness, not observation coverage. Its prerequisite and all cutover guards remain. The other nine technical gaps still block. Original exact33/193 authority and sanitizer omissions are not expanded; finite source/profile renewal preserves membership and historical negative proofs.

The exact historical inverse is composed before existing projections; the one descriptor-owner insertion is reversed only under before/after source hashes. J's exact three job changes, five inputs and two path rules have a finite inverse. Unknown mutations refuse; there is no blanket golden reset.

### Pinned strict fuzz toolchain

J's official JSON-GLib1.10.8 setup/probe/regression sources, producer/toolchain inputs, three workflow job fingerprints and semantic input edges are enrolled. Source archive SHA256 is `55c5c141a564245b8f8fbe7698663c87a45a7333c2a2c56f06f811ab73b212dd`. Parent-verified local proof has51 distinct retained inputs: system1.8 exits77, official1.10.8 exits0; unchanged strict60s json-event passes three runs and both other fuzzers pass. This is a concrete CI-only remedy, not a provider-refusal blocker. Original named62179 input identity remains unrecoverable and exact replay NOT RUN; the51 inputs are not substitutes. Hosted acceptance and `.29` remain open. No production dependency bump, suppression, dependency-source patch, corpus replacement or duration reduction.

## Verification and retained attempts

The independently materialized source manifests bind physical bytes, executable modes, symlink text and recursive submodule OIDs. No mutable root source is imported. Detailed final outcomes are in `local-proof-summary.json` and the raw directories below.

* At final code `0e3601eab21ca480a7d80c72d6c29ac29155f5aa`, fresh safe native all-default build and full439-registration CTest pass with zero failures and five visible skips (469.71s). Read-only keychain guards pass before/after. The original eight heartbeat cases all pass; control thread off, foreground unchanged, all blocking/final/burst/boundary negatives retained. Native TCP is a visible skip, not a TCP positive.
* At prior code `189fcc2237724ea5395f64d7c44e1776efacdf74`, provisioned Linux/ARM policy regression passes all14 commands:79 actual kernel cases with zero skips,915 selector cases with six explicit MDK opt-ins, all static/shell checks, and exact owned container cleanup. Actual wrapped command/CTest positives, failures, cancellation, service restoration and escaped-tree cleanup are exercised by the kernel matrix. This is component/public-wrapper proof, not a selected static.profile receipt or native x86 parity.
* The one bounded integration review identified three seams: tool/producer/audit validation, fabricated test-only preaudit, and unselected Go execution accounting. All three were fixed and their focused regressions pass. No iterative re-review or threshold relaxation.
* Focused integration runs include106 combined checks,21 actual miniature backend retention cases,52 actual-gap fixes,44 historical/semantic fixes,24 phase checks,6 owner/phase negatives,11 J inverse checks, and five final postbuild-edge checks. These are overlapping diagnostic suites, not W11 quota counts.
* Final static checks include unsequenced/gui/package self-tests, privacy, six affected workflow actionlint inputs, four affected shell scripts and whitespace. A mistaken first ShellCheck invocation named two nonexistent paths; its failure is retained separately from the corrected successful invocation.

Retained unsuccessful attempts are not erased or promoted:

1. Earlier public TCP could not fetch nested Git objects offline; exact selected-OID local hydration was implemented, not a moving-tip alias.
2. `e17d3843` public full preaudit retained65 findings:61 claimed-but-disabled GNOSTR/GTK registrations plus four fixture semantic findings. Core configuration and exact fixture edges fixed them; none were deleted from the universe.
3. `735e3b2f` Mac-shared worktree evidence triggered Git ownership refusal inside custody. POSIX owned tmpfs provisioning replaced that diagnostic storage; no Git trust/caller-attestation weakening. The missing final seal is not called successful native cleanup even though the outer owned Docker CID was removed.
4. The POSIX source then passed the2357-input preaudit and refused build phase1 versus authored2. Explicit core phase2/2 fixed the actual contract; no CLI guard was relaxed.
5. `189fcc223` passed full preaudit and built the actual Linux default graph, then postbuild retained four findings for the relayd header and generated D-Bus input (two findings each, registration and support check). `0e3601eab` owns those exact fixture dependencies; no header/Ninja audit is bypassed. Full raw575098880-byte tar and hashes remain.
6. An auxiliary Rosetta `ps` observer hit `/proc/.../auxv` permission/SIGTRAP; this is an emulation observer failure, not an established cause of a gate failure. No retry/identity relaxation follows from it.
7. Earlier policy suites failed on a minimal-fixture import seam and then one frozen historical descriptor mismatch. Source fixes and exact historical projection produced the later all14-command passing invocation; prior logs remain.

At final code0e3601eab the public emulated Linux default build passes, with full preaudit2357 inputs and postbuild6921 input bindings, zero findings. The actual compiled heartbeat ELF is1574360 bytes, SHA256 `fbf8456d629c45aab7867a257b15f0a1a632d8f434e95adb8b337c74d597907d`. Earlier standalone heartbeat compilations remain separate target-only proof, not full Linux build claims. Quiet after-Test scheduling probes remain diagnostic, not evidence of the actual parallel Test gate. The hosted Test log must retain its real `-j2` scheduling environment.

### Terminal public-route failures: internal work still required

Final TCP source0e passes both complete audits and the default build, then refuses **before CTest**. Exact retained `signer/worker/bus.log`: `Failed to start message bus: Socket name too long`. The fixture appends its Unix socket to the deep custody resource/evidence pathname; this is an **unresolved internal fixture defect**, not an established emulation incompatibility or external approval/provider blocker. The backend reports unexpected bus exit and failed expected fixture shutdown. No verified native final receipt exists. The outer owned Docker CID is removed, but that is not substituted for missing inner final seals or successful signer readiness. Raw581058560-byte final tar and its SHA remain. Tracked internal defect **nostrc-0cfsg.36.5** requires a safe owned short-address solution and actual long-evidence-root positive/negative/cancel proof before a successful public signer cell; do not dispatch expecting this source to pass that cell.

The new selected policy controller was also attempted under an emulated Linux/x86 controller using the existing local Docker socket (controller only; never mounted into the workload). It did not reach selected execution: the SDK lacked buildx; supplying the official checksum-verified plugin first exposed a missing executable mode, then the correctly executable plugin exposed Docker socket DAC permissions for the cap-dropped root controller versus the501-owned local socket. These are retained setup failures, not passing typed receipts or a native parity claim. The first outer-controller cleanup correctly refused F's different user1001 workload identity; its exact exited CID/name/token was separately verified and removed with a retained recovery record. Later outer controller CIDs were removed with separately scoped exact-ID cleanup. No socket permission, host policy, capability or production argv was relaxed. Per the bounded stop instruction there is no further container retry. Existing14-command ARM regression proof stays valid within its own scope; a successful **selected** typed policy receipt remains NOT RUN after correct matching-host provisioning.

## Remaining ownership and acceptance, not an internal enrollment deferral

`ownership-and-admission-register.json` enumerates every actual named check/registration owner and its authored target/command requirements for core/grotto/groundhog, plus full33 ownership and remaining admission prerequisites. Its older reference census is explicitly not a fresh executed-universe receipt. Current root full audits and raw configured inventories remain separate physical evidence.

Full ordinary/root acceptance still needs the exact fresh native Linux registration/source/toolchain universe and all physical prerequisites, signer consent/token/service/cancellation seals, unsupported signer/keyring-command ownership, native pidns cleanup/profile parity, and genuinely complete workflow/profile union. Full Go ownership is not whole-profile ownership and cannot promote a diagnostic subset. No complete flag is cleared here.

The nine still-blocking technical rows are workflow_profile_union, root_asan_execution_contract, groundhog_sanitizer_execution_contract, libgo_macos_asan_stress_exclusion, supported_profile_union, prepared_peer_linux_contract, homed_unit_profile_contract, worker_setup_and_grotto_execution and supported_profile_execution_remainder. Administrative shadow_cutover is separately retained with enforcement_ready=false.

C's source-linked census snapshot was `2026-10-23T02:12:04.578186Z`–`02:12:28.940355Z`:21 real PR planner replays twice, diagnostic only. Fresh API refresh06:34:08.709047Z–06:34:42.397955Z returns54 roots,53 failed/one cancelled; four complete job/artifact lists and eleven original ZIP/member hashes are retained, all four validation and three broad receipts fail.0/20 qualifying comparisons and0/2 successful complete broad integrations. These are dated—not live counters. This local batch adds no W11 quota credit. W11 NOT MET; W12 NOT RUN. Parent runs the frozen landed source; no hosted or native-x86 claim is inferred from ARM/emulation, skipped CTests, fixture tests or a predecessor job.

## Parent dispatch, no rediscovery

Use an existing landed branch/ref and record `LANDED_SHA`. These workflows have no dispatch `sha` input; verify every actual run's headSha matches it:

```sh
gh workflow run dependency-tests.yml --ref "$LANDED_REF" -f mode=broad
gh workflow run grotto-fuzz.yml --ref "$LANDED_REF" -f target=all -f fuzz_duration=60
gh run list --workflow dependency-tests.yml --event workflow_dispatch --branch "$LANDED_REF" --limit 5 --json databaseId,headSha,status,createdAt
gh run view "$RUN_ID" --log > "run-${RUN_ID}-attempt-${ATTEMPT}.log"
gh run download "$RUN_ID" --dir "run-${RUN_ID}-attempt-${ATTEMPT}-artifacts"
```

`dependency-tests.yml` optionally accepts `mode=affected` and exact `base`; that is diagnostic, not a replacement broad run. Standalone fuzz60 matches the unchanged strict PR/broad duration; manual default300 is unchanged. Reusable worker/broad/namespace-policy/nostrc-ci workflows are not directly dispatchable.

Expected logical job IDs include select; core/execute; grotto/execute; groundhog/execute; policy_regressions/regressions; policy/execute; broad/core/build-and-test (matrix); broad/static/regressions; broad/fuzz/fuzz-test; broad/broad_gate; gate. Additional selected profiles retain their own workers. Standalone fuzz uses select-fuzz, fuzz-test (display names `Fuzz nip49`, `Fuzz bip39`, `Fuzz json-event`) and coverage-report. A successful regression predecessor is not policy/execute acceptance.

Artifacts and paths (retain the actual run attempt):

| Artifact | Retained contents |
|---|---|
| dependency-plan-${PLAN_ID}-${ATTEMPT} | plan.json, optional release.json |
| dependency-receipt-${PLAN_ID}-${PROFILE}-${ATTEMPT} | ${PROFILE}/receipt.json and worker.log; verified native receipt only |
| dependency-evidence-${PLAN_ID}-${PROFILE}-${ATTEMPT} | dependency/evidence/**; dependency/ordinary-tool-setup.json; dependency/policy-testhost-setup.json; dependency/policy-image-*/**; dependency-build/namespace-policy/{receipt.json,source-manifest.json,control/**,workload/**}; inventory/descriptors |
| dependency-namespace-policy-regressions-${SHA}-${ATTEMPT} | control/** and evidence/{regression.json,logs/**,custody/**}; predecessor only |
| validation-namespace-policy-regressions-${SHA}-${ATTEMPT} | broad/static counterpart, not selected receipt |
| dependency-broad-${SHA}-${ATTEMPT} | broad.json |
| dependency-validation-${PLAN_ID}-${ATTEMPT} | validation.json |
| validation-${RUNNER_OS}-${SANITIZER}-monorepo-logs | failure-only broad/core build/Testing logs and CMakeCache |
| validation-fuzz-corpus-${TARGET}, validation-fuzz-crashes-${TARGET} | broad fuzz corpus or retained failed input/log |
| fuzz-corpus-${TARGET}, fuzz-crashes-${TARGET} | standalone fuzz corpus or failed raw input/log |

Broad uses the exact `validation-` artifact prefix, not `broad-`. Preserve original failed/cancelled attempts. Cancellation uses only `gh run cancel "$RUN_ID"` for the intended run; inspect always-uploaded cleanup/source seals rather than assuming job cancellation proves cleanup. No native/hosted positive is predeclared.

## Tracker, preservation and version assessment

Official `bd export --all` includes all records/comments and all37 memories; the final export has1563 records (1526 issues),654 comments,37 memories and1165 byte-identical interactions; exact hashes/comparisons are in tracker-preservation.json. No initial record/comment or memory was lost. The preservation proof compares initial HEAD/worktree/official snapshots and interactions without hand editing records. Parent component comments and failed evidence remain. `.36`, `.9.1` and `.29` acceptance remain open; `.5` keeps its finite historical closed status, with a new evidence note rather than quota inflation. Root user files/submodule state are checked against initial-user-preservation.json. Final counts/hashes are in the handoff evidence.

Version assessment: this batch changes internal CI/test ownership, setup, diagnostics and documentation, not a shipped public API/runtime or production dependency. **No component version bump.** No unsupported native parity, release, complete-gate, or all-closed claim.

## Addendum: private NIP5F bus socket placement (nostrc-0cfsg.36.5)

Supersedes only the "unresolved internal defect" statement above; nothing else in this report changes. Code `744d2b4eddd5bae22f160604e45ab67bf67240d1` (atop sealed `8b7d730`; first fix `4529c0111`, parent review fixed in `744d2b4ed`). Exact details, hashes and retained attempts: `dependency-aware-testing-w11-socket-placement-handoff-2026-10-23.json`.

- **Cause and fix.** `test-gate.sh` mounted the private-resource tmpfs at `<lane>/resources`; the fixture's bus `unix:path=` socket therefore exceeded `sun_path` (107 bytes) under deep evidence roots. The mountpoint is now allocated by `allocate_resource_mountpoint()` under `/tmp` as `nostrc-rr-<128-bit token>` with an append-only hash-chained fsynced journal (`intent → allocated(dev/ino) → released`). Release removes only a directory the journal proved it created and never recurses. An intent-only journal never authorizes removal: a present directory is refused and retained (`recovery_refused`), an absent one is recorded `absent` with `allocation_proved=false`. The namespace tmpfs, HOME, runtime and sockets remain namespace-private.
- **Proofs (not native-x86/hosted).** 84/84 real Linux cells on native ARM64 (deep-root negative control reproduces `Socket name too long`; short-root bus roundtrip; nonzero; SIGTERM cancel; public-gate pidns positive/nonzero/cancel). Real signer fixture on a native aarch64 build, 151-character evidence path: old placement 12 errors, new placement 6/6, journal released, directory gone. Whole selection suite matches the `8b7d730` baseline except the new passing test.
- **Actual public route (amd64 emulation).** Full audits, default build and bus/Secret Service/daemon startup now pass; the approver then aborts `fixture API failed: StoreKey` before CTest. This is a **new named failure** and is not proven emulation-caused (the same signature was retained for the emulated fixture under `.36.1` while the native ARM fixture passes). No successful public TCP receipt exists; W11 NOT MET.
- **Retained, not hidden.** Attempt 1 failed on my incomplete snapshot (nested `secp256k1` gitlink offline); attempt 2 (source `4529c0111`) was cancelled by me after about one minute and is not proof; attempt 3 is above. The original `0e3601e` failure is unchanged.
