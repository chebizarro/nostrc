# Dependency-aware testing — staged implementation receipt

**Historical integration snapshot:** `017039eea6620caea791162e1646982abf3d69f4`, fast-forwarded into the assigned `.5` worktree. **Tracker:** `nostrc-0cfsg.5`, remains open/in progress. **Status:** explicit tooling/workflow interfaces implemented; **rollout acceptance NOT MET**. Documentation-only version assessment: **no bump**. No settings change, default cutover, schedule removal, merge back or push by this task; parent owns landing/push/cleanup.

The dated [9c03e7712 continuation](#2026-10-16-continuation--integration-9c03e7712) and latest [post-review ae36f5a26 reconciliation](#2026-10-16-post-review-continuation--ae36f5a26) below record subsequent states. All receipts and outcomes retain their original SHA and meaning, including the failed diagnostic lane and the historical MDK smoke whose cleanup claim is now superseded.

## Evidence and review resolutions

| Evidence | Exact outcome / boundary |
|---|---|
| Parent final integrated suite at `017039eea` | **172/172 selection/regression tests passed, no skips, 34.193s**. Parent also confirms **all 6 relevant shell suites**, Actions syntax, every repository static checker, and committed-range whitespace passed. These are parent-confirmed receipts, not reruns by `.5`. EOF correction `db9bf4543` is included. No pending 172-test count. |
| [Independent source review](dependency-aware-testing-ci-2026-10-15-implementation-review.md), committed as `efd427e90` | Review was performed at `48e8d2b0e`, not repeated here. R1/R2 fixes below are integrated and parent-confirmed by the final regression run; this is not a new broad review or W11 approval. |
| R1: reused-checkout/source identity | Caller binding fix `e7cbf3cf6` plus actual source-content identity/backend checks in `726f54fd5` bind reuse to admitted checkout and source content. Parent previously ran five actual cross-checkout tests. Integration fixture helper collision was corrected by `d132bcc89`; final 172-test result supersedes the intermediate helper errors, which were not native/source assertion failures. |
| R2: fixture/ordering consumer impact | `726f54fd5` audits full primary support-input closure, including fixture/order consumers; `b0a31b0ab` corrects CTest build-path alias canonicalization. Backend branch **167 tests passed**, parent-reported; final integration result above includes the additional caller tests. No claim that all supported production profile/ownership edges are now complete. |
| [Native root execution](dependency-aware-testing-ci-2026-10-15-native-root-qa.md) at `d9e8c6294a35f04d0c73870d4d27f28990759a24` | Fresh Debug/Ninja root configure and all default targets passed; full CTest **432 passed / 5 skipped / 0 failed**. Private Secret Service/bus and before/after read-only keychain guards passed. **Groundhog OFF**; no Groundhog/GUI/live-peer acceptance. Subsequent integrated changes are tooling/workflows/regression fixtures, not the compiled project sources/CMake, so this remains valid compiled-source evidence, not a claim of a new full native run at `017039eea`. |
| Native selected CLI at `b0a31b0abc85bee747e53836bfe30dba4a468b13` | Actual standalone `libgo.macos.asan` execution: **32/32 passed, no skips/failures**, one CTest `-I` invocation; backend subprocess time **9.1395s** (CTest log 9.13s). Post-build audit `passed=true`, **`gaps=[]`**. **CLI test phase exit 1**, `job_conclusion=failure`, `complete_gate_eligible=false`; both complete and partial aggregation exit 1 / `passed=false`. Execution proof is **not a green/eligible lane**. |

The native profile explicitly excludes `GoChannelStressTest` under the existing macOS ASan baseline. Its `go_channel_stress_test` target was built **only for discovered-header auditing**; registration status is **`not_executed`**, never passed, XFAIL or a completed check. The backend retains the exclusion and profile-union/W11 coverage gaps; the complete aggregate additionally rejects the partial projection. No eligibility relaxation is requested or implied.

Native CLI evidence was read, not rerun, at `/tmp/nostrc-0cfsg2-native-b0a31b0a/`: normalized `receipt.json`, nested backend receipt/CTest command, `postaudit.json`, both aggregate results and exit files, and `evidence/**/attempt-1/results.xml`/`ctest.log`. JUnit has 32 cases, zero skips/failures. Earlier path-alias failure remains preserved at `/tmp/nostrc-0cfsg2-native-726f54fd/` (`test-exit-code=1`, `preexecution-failure.txt`), not overwritten or represented as a successful attempt. Bounded SHA-256 anchors:

```text
bc2a9d3f24543fc097e9bddebefb7f33a39e9fa3bc97b64f00527ea829a60b3b  receipt.json
60e53d9d693ba1a399733018f7f5f23c1567babae704f7b65d7d59924ecd4ebc  postaudit.json
637746cc30df36f7e8b644b318964b504081a98aefb23a302cf4ac2f5ac37be7  aggregate-complete.json
5295f4d8a18682ff182812562abd015b6b3f8dfacb1ceb9c79205d1bbd378a49  aggregate-partial.json
```

## Implemented explicit commands, not new defaults

`scripts/test-plan.py` provides plan/explain/validate/resolve/check-inputs/run/aggregate/release-plan; `scripts/test-gate.sh` provides explicit local snapshots/exact-tree diagnostics, profile projection, broad mode and plan-only `--explain`. Current command shapes and caveats are in [TESTING](../TESTING.md#dependency-aware-commands--explicit-diagnostics). `NOSTRC_SELECTIVE_GATE=1` adds the adapter to existing push gates; it does not replace static checks or mandatory broad verification. Unsupported checks/capabilities fail visibly; a partial run or bypass cannot become complete CI evidence.

The checked-in native profile is **standalone libgo**, macOS arm64/clang, Debug ASan/UBSan, source `libgo`, Ninja, portable context OFF, warnings-as-errors, effective `ASAN_OPTIONS=detect_leaks=0`. It is not a root/Groundhog or LSan profile. Its 32 exact selected registrations are split between `libgo.channel` and `libgo.native_standalone`, with the stress exclusion above.

`.5` validated the exact CLI surfaces at `017039eea` without compiling/running new tests:

- Planner and gate top-level `--help`, plus **all eight** planner subcommand helps: exit 0.
- Planner `explain --base e6030a017d619d9ff14cc4372acee8c04212eff9 --head 017039eea6620caea791162e1646982abf3d69f4 --context local --profile libgo.macos.asan`: exit 0.
- Gate with that same base/head/profile and `--explain`, plus `--head <same SHA> --broad --profile libgo.macos.asan --explain`: exit 0; each plans **2 checks / 1 lane** and explicitly says **NOT complete CI/push coverage**. No configure/build/test occurred.
- Policy-only `validate --inventory tests/selection/inventory.json`: exit 0, `valid=true`, `enforcement_ready=false`, retained coverage gaps. `--strict`: expected exit **1**, not an accidental error or acceptance pass.

Exact commands/stdout/stderr, plans and adapter receipts are retained under the final CLI evidence directory named in the `.5` bead notes. Temporary evidence is local diagnostic material, not durable hosted/W11 artifacts. Documentation whitespace, local links/fences and receipt/hash consistency were checked before commit.

## Shadow workflows and publisher staging

- `dependency-tests.yml` is **Dependency tests (shadow)**: additive unfiltered PR, merge-group, integration/tag/manual/reusable routing; `dependency-worker.yml` executes same-SHA selected lanes; stable `Dependency gate` aggregates them. It is **not accepted as a new required status** by this receipt.
- `dependency-broad.yml` calls existing validation families, including MDK, extended fuzz/BP and Windows; `release-validation.yml` requests exact-SHA broad validation. Legacy triggers/schedules remain. Source wiring/unit tests are not successful hosted broad/release evidence, and inventory gaps still prevent complete acceptance.
- Main, Groundhog and Windows publishers use the repository variable `NOSTRC_DEPENDENCY_RELEASE_MODE`. This documents the **source fallback**, not an independently queried current GitHub variable value; no variable/settings mutation was made.

| Mode | Current publisher dependency-validation contract |
|---|---|
| `disabled` (fallback when unset) | Added validation job is not requested. Existing publisher checks still apply. Publication decision reports `validation_required=false`, `validation_status=staged_disabled`; it is **not** a validation attestation. |
| `shadow` | Added exact-SHA validation runs, but its result is not a new publication requirement. Decision is `staged_shadow`, not a fabricated validation pass. Failures remain visible. |
| `enforce` | Requires successful validation job plus broad/complete, enforcement-ready, gap-free exact-source receipt and every applicable package result/artifact. Not authorized before W11/operator cutover; no bypass or relaxed completeness is proposed. |

The publisher variable does not disable the independent general PR shadow entrypoint. No protection/required-check transition or schedule removal is performed here. The [W1 GitHub audit](dependency-aware-testing-ci-2026-10-15-w1-baseline.md) remains its dated live-state snapshot, not a refreshed server-state assertion.

## Remaining acceptance stays on existing open beads

| Existing bead(s) | Still-open scope; no false completion |
|---|---|
| `.1` / `.2` | Complete authored ownership, exact logical membership, runtime/header/semantic edges and supported registration/profile union. Parent's root/433-Groundhog zero-unowned probes and this narrow native zero-gap audit do not establish the full union. Review fixes are resolved; union acceptance remains open. |
| `.2` / `.3` | Guarded non-CTest command/build-contract execution; currently unsupported paths must refuse rather than synthesize receipts. |
| `.3` / `.4` | Actual live-MDK adapter/handoff, fixed-tag 0.8/0.9 cross-checkout image lifecycle lock, 0.11 snapshot/image/driver-container ownership/cancellation, and hosted broad/release proof. Local fixtures do not establish these. |
| `.3` | Shared Groundhog sanitizer profile/config/targets/environment/input-audit parity; legacy parser/selectors remain until proven replacement. No LSan/TSan/live sanitizer acceptance inferred from this pass. |
| `.2` / `.4` | Native/toolchain/architecture/optional profile union; `.4` explicitly owns `libgo_macos_asan_stress_exclusion`. 32 executed cases do not close the unexecuted stress gap. |
| `.5` / epic | Representative five-sample clean/incremental costs, **20 representative PRs / 2 broad integration runs**, release replacement/dry-run evidence, skip/cancellation/merge-group accounting, no-gap protections and later W12 cleanup. None is satisfied by this docs/CLI pass. |
| `.6` | Existing workflow ShellCheck cleanup remains separately tracked; passing Actions syntax is not a claim that unrelated legacy shell cleanup is complete. |

No new broad source review/Oracle retry or implementation was performed. Final parent confirmation now supplies the merged 172-test outcome; **no single-line pending-count update is needed**. Future updates require actual external rollout receipts or new acceptance work, not changing the failed native lane into a pass. `.5` and the remaining scoped implementation beads stay open.

## 2026-10-16 continuation — integration 9c03e7712

**Dated source snapshot:** `9c03e77126d8f2f092b22bafbd79280fbaf10950`, fast-forward merged into the assigned documentation worktree; clean before this docs-only pass. It includes the coherent A/B/D/C/E changes and the new native-root report. This continuation supersedes current-status claims above that non-CTest core execution is unimplemented, that every partial diagnostic aggregate must fail, or that `.6` remains open. It does **not** rewrite the earlier failed-lane receipts, native SHA identities or historical 172-test result. **No component bump:** internal testing/documentation only. No production/runtime/helper/metadata edit, new Oracle review, default/enforcement/schedule/protection change, merge back or push by this pass.

### New evidence, with bounded provenance

| Evidence / source | Actual result and limit |
|---|---|
| Combined parent regression at `9c03e7712` | Parent confirms **244/244 tests passed, 0 skips, 58.163s**, clean unchanged source; retained `/tmp/nostrc-completion-final-selection.log` was spot-checked. Full actionlint **including ShellCheck** passed. Parent also confirms **all six shell suites passed** at that SHA: `test-sanitizer-gate.sh`, `test-full-gate.sh`, `test-linux-gate-lifecycle.sh`, `test-linux-gate-smoke.sh`, `test-pre-push.sh`, `test-test-gate.sh`; **all four static checkers and range whitespace passed**. These are final combined results, not inferred from B's earlier pass. Independent review was subsequently delivered as `24b7bb987` at `9c03e7712`; fix status is recorded in the post-review continuation below, without claiming a broad rereview. |
| B slice `3a7c685e02ba9ad537153cf69e83c5db40df1ef2` | Reported **244/244 tests**, six shell regression suites and static checks passed. This is B's bounded slice evidence, distinct from the parent final verification above. |
| Native selected libgo at `a21d25f7fdba6b5cb13c44cc837f3763773eb211` | Actual **32/32 passed, 0 skips/failures**, one indexed CTest invocation, **8.7923s**, zero post-build audit gaps, CLI **exit 0**, `job_conclusion=success`, **`diagnostic_eligible=true`**, **`complete_gate_eligible=false`**. Partial aggregate exits **0**, `passed=true`, `complete=false`; complete aggregate exits **1**, `passed=false`. This is a **DIAGNOSTIC success**, never complete/stress acceptance. Receipt/aggregates were read and hashes verified, not rerun in this docs pass. |
| Real core command/build contracts | Implemented and actually exercised in native miniature projects, including prerequisite builds, bound argv/cwd/source/input identity, capability denial, missing/nonzero/stale evidence, drift and cancellation/process cleanup. `/tmp/nostrc-0cfsg7-native-a21d25f7/miniatures/proof-index.json` records eight retained miniature probes; combined regression provides further tests. Production profile union and outer guarded adapters remain separate acceptance. |
| Active-language compiler correction `8db718cb1` | Compiler identity now follows languages actually compiled, while missing/ambiguous/incorrect active identities fail closed. Root **configure + resolve** evidence only; it is not shared root ASan execution or full input/profile-union acceptance. |
| Root CI / sanitizer metadata `e6a381156` + `d3cf449e1`, integrated with B | `core.linux.asan` / `core.macos.asan` retain faithful root CI configure/command/environment and configured registration membership. `groundhog.linux.sanitizers` retains configure/build/test phase environments (0/0/5 entries), exact configured membership and target metadata. These are **configured membership/phase parity only**, not executed shared sanitizer receipts. Existing CI exclusions remain visible unexecuted gaps; no `.11` exclusion was added. |
| **Historical** standalone MDK 0.11 smoke | Docker/private-bus/throwaway-Secret-Service command/CTest evidence: **5/5 CTests**, 0 skips/failures, about **12.91s**; read-only keychain guards passed. The old receipt reported `status=command_succeeded`, `exit_code=0`, `cleanup_complete=true`, `complete=false`; **the successful-cleanup claim is superseded by F1 / `85a2e2ab`**, not current lifecycle proof. Arbitrary detached-tree cleanup remains unverified; no selector/full-matrix/shared-sanitizer/complete-gate acceptance follows. |
| [Fresh native root QA](dependency-aware-testing-ci-2026-10-16-native-root-qa.md) at `84ae0896b0abd835935c264b872e9b184d616364` | First configure/all-default-target build/full serial CTest passed: **437 exact unique cases, 432 passed / 5 skipped / 0 failed/errors/disabled**. Private HOME/XDG/Unix bus/throwaway Secret Service, loopback-only outbound IP and read-only keychain guards passed. Receipt/JUnit/outcomes hashes and accounting independently spot-checked. **Groundhog OFF**, no GUI/live/profile-union acceptance; not a new run at `9c03e7712`. |

Parent wrapper setup initially named nonexistent `test-linux-gate.sh`, then corrected the invocation to `test-linux-gate-lifecycle.sh`. That wrapper command-name error was **not a test failure or test retry**; it does not erase a failed suite or supply retry-based acceptance.

`GoChannelStressTest` remains **`not_executed`**; its target was built only for discovered-header audit. The successful newer diagnostic receipt does not turn the excluded registration into a pass and does not make partial/incomplete evidence complete. The older `b0a31b0ab` lane/aggregate failures above remain true at that SHA.

### Unsupported execution and actual failed custody proposal

- The **core** real `command`/`build_contract` implementation is no longer missing. The **outer** `scripts/test-gate.sh` adapter still explicitly refuses selected non-CTest checks and unsupported Docker/external-peer capability admission. That refusal must not be described as lack of a core backend.
- Shared phase-aware sanitizer **execution remains unsupported**. The outer adapter refuses phase-environment profiles; `NOSTRC_SANITIZER_SHARED_PROFILE` on the Linux sanitizer route exits 2 before Docker/build rather than falling back to a legacy tree and calling it shared proof. Exact phase metadata/parity cannot substitute for phase-aware configure/build/test environment application, fingerprinting, input audits and executed acceptance. No shared ASan/UBSan/LSan pass is claimed.
- MDK **selector custody/finalization remains unsupported**. The inherited-socket/FD proposal failed an **actual CMake/Ninja/CTest chain**: the consumer saw `EBADF` on the control FD; CTest exited 8 and its guard failed. The negative probe note is `/tmp/nostrc-0cfsg8-ctest-fd-blocker.txt`; unaccepted verifier/outer attachment prototypes were archived as `/tmp/nostrc-0cfsg8-unaccepted-*`, **not shipped**. Direct subprocess probes did not prove CTest traversal. The standalone helper's state/environment and `complete=false` receipt confer no selector authority. No config fallback, listener/token redesign or new transport was authorized.
- `.11` remains **unresolved**: CI heartbeat 18 / missed 3 / max gap 60.3 ms failed the original guard. The fresh root sample's four latency cases passed (9 / 0 / 15.0 ms), but it was non-sanitized/serial and does not establish the hosted cause. Prior bounded ASan/probe evidence is preserved separately in the new QA report. No threshold relaxation, exclusion, retries-as-acceptance or runtime/test correction is authorized.

### Limited current CLI/docs verification

At clean `9c03e7712`, this docs task ran planner/gate top-level help plus all eight planner subcommand helps: **10/10 exit 0**. Planner `explain` and gate `--explain` for base `84ae0896b0abd835935c264b872e9b184d616364`, head `9c03e77126d8f2f092b22bafbd79280fbaf10950`, profile `libgo.macos.asan`, plus gate broad/profile explain all exited **0** (2 checks / 1 lane; plan only). Policy-only `validate --inventory tests/selection/inventory.json` exited **0** with `enforcement_ready=false`; `--strict` returned the expected **1**. No configure/build/test was rerun by this CLI check.

Current help confirms `resolve`/`run` need `--plan` and `--build-dir`, raw `run --phase {configure,build,test,all}`, and `aggregate --plan <plan.json> --receipt <receipt.json> --partial` for diagnostic-only aggregation. Exact source identity/admission is still required; examples do not authorize reusing a raw realization from a different checkout. Full output/argv/exit receipt: `/private/tmp/nostrc-0cfsg5-docs-9c03.JgrA7U`. Current help/limitations are reflected in [TESTING](../TESTING.md#dependency-aware-commands--explicit-diagnostics).

New bounded SHA-256 anchors below are scoped to their named directories, not substituted for the older block above:

```text
# /tmp/nostrc-0cfsg7-native-a21d25f7/
c8b7e8cdea6ce7c08cacf245ee4cb610511f0d8fec82d0d53126c6a70d08c853  receipt.json
dd8b52d23e66f460604b9d80651c9639110418abca486a488a3272e0aa2cf2ea  aggregate-partial.json
b96a60ef43e84f39583398e96b0ad5f5a743da36b03f5246865597ef596236d5  aggregate-complete.json
# /private/tmp/nostrc-0cfsg5-native-20261016.2LRxvZ/
b377e6b016514e8a0666aceb90b79ffbf0d50af25baa01551c410afc69bcaf8d  receipt.json
c8fb9263af73866766d6a8c48c301c623670e99b403534b558477bb0d1e8c050  ctest.xml
4f559a4ceb6763199a6d8a73eb30a291c4d04bf4c819f3c9fc5f2830685eebbb  outcomes.json
# /tmp/nostrc-0cfsg8-peer/evidence-final-phase/b414c3bb2e964b8b91b1e61407fad70a/
e5e4ccb9d9952f5cde3223b86d53086e25a4346a946a91e31e1d63af8dabe0d8  receipt.json
# Parent combined Python log
9a6e2b88cb727e9f2a0fbe7709d70257c9cc3d5c492e4ebe2bf3f3dde9de3b77  /tmp/nostrc-completion-final-selection.log
# /private/tmp/nostrc-0cfsg5-docs-9c03.JgrA7U/
f92c42d93e4f91e0377b75f0f87a9af7f31ecad902ea003749ecc18c085c3f16  receipt.json
395ab873deecc8e29c4cbe995aa747cfe86d1bb101c0564ff6a2ac0a7c70b247  gate-help.out
7e12eac82849674bcec37deb5e2fb04f57f8af841db50e50b4f45a4031a6ec4b  run-help.out
2ba6624e67907a5711c65cc239baa32f317a5264da96b185bebcf003181badc3  aggregate-help.out
```

### Status and exact remaining acceptance

| Bead | Current status / remaining boundary |
|---|---|
| `.6` | **CLOSED**: workflow ShellCheck cleanup; parent full actionlint including ShellCheck passes. This does not authorize workflow/default changes. |
| `.10` | **CLOSED**: inventory exporter compatibility with declared CMake 3.22 restored and tested across old/current versions; internal metadata/build-contract correction, no shipped API/runtime change. |
| `.7` | **OPEN / in progress**: real command/build contracts and native diagnostic eligibility are implemented; complete ownership/runtime/header/semantic/profile union, stress-gap acceptance and guarded capability integration are not complete. Root active-language fix is configure/resolve evidence only. |
| `.8` | **OPEN / in progress**: chain-safe MDK selector custody/finalization and phase-aware shared sanitizer execution/audit acceptance remain unsupported; standalone smoke/parity/cleanup proofs are not substitutes. Full 0.8/0.9, concurrency/cancellation and hosted integration evidence remain distinct obligations. |
| `.9` | **OPEN / in progress**: configured root/sanitizer metadata and observed membership/consumer edges are integrated, not complete executed workflow/profile/runtime/header/optional-variant union. Native stress, root-ASan execution-contract and platform/hosted evidence gaps stay explicit. |
| `.11` | **OPEN**: source-grounded cause and deterministic verification remain unresolved; retain original latency guard and failure evidence. |
| `.1`–`.5` / epic | Existing original scopes stay open. W1 representative five-sample costs, W11 **20 representative PRs / 2 broad integration runs**, exact release replacement/dry-run, selected-skip/cancellation/merge-group and no-gap protection/operator cutover proof remain unmet. W12 cleanup remains NOT RUN. |

No default/enforce/requirement/schedule/protection cutover is authorized before W11/operator approval. Independent source review `24b7bb987` was delivered at `9c03e7712`; its findings and post-review fix status are recorded separately, not repeated or promoted to broad approval here. Parent owns integration/merge/push and remaining acceptance; this continuation is documentation, not a completion attestation.

## 2026-10-16 post-review continuation — ae36f5a26

**Current integration:** `ae36f5a2639ff1c28d6657621b8afb0d81771b2b`, fast-forward merged for this docs-only reconciliation. The [independent review](dependency-aware-testing-ci-2026-10-16-continuation-review.md) committed as `24b7bb987` remains historical at **`9c03e77126d8f2f092b22bafbd79280fbaf10950`**, with F1–F3 intact. Its appended [resolution-status section](dependency-aware-testing-ci-2026-10-16-continuation-review.md#resolution-status-after-source-fixes) records fix commits/evidence. Parent performed source spot-checks, **not a broad rereview**; no new provider/Oracle review is claimed. The original rollout and `.7`/`.8`/`.9`/`.11` scopes remain open.

| Finding / bead | Current bounded resolution |
|---|---|
| F1 / `.12` **CLOSED**, `85a2e2ab633e77157b2c8ede80449e68977ed0d4` | Removes the false cleanup attestation only. MDK helper **always** records `cleanup_complete=false`, `complete=false` and arbitrary-process-tree `unverified`. Otherwise successful command phases retain their own success/exit but run returns nonzero `cleanup_unverified` (or `cleanup_failed` for explicit scoped errors). Original command failure/cancellation, original-session cleanup and recorded-container cleanup remain distinct. Real detached-descendant/CTest/fake-Docker regressions passed; owner reports 16 focused / 248 branch tests. **`.8` remains OPEN** for actual arbitrary-tree ownership/lifecycle and selector custody/finalization. |
| F2 / `.13` **CLOSED**, `83a38d879c82429b07dd627a36a64e52e9810c1b` | A nested or outer negative `diagnostic_eligible` rejects aggregation even if the outer duplicate is absent. Focused eligibility 8/8 passed; positive bounded diagnostics still do not confer complete coverage. |
| F3 / `.14` **CLOSED**, `732346a87bf22df15ea1e11040b94f9e7eca01bc` | Raw byte capture/log/hash plus lossless UTF-8/surrogateescape decoding; running checkpoints are not success. Unexpected execution errors finalize failed receipts, retaining original failure/output and separate cleanup errors; direct-child fallback/reap handles group-signal denial. Real byte/exit/timeout/error miniature receipts are retained under `/tmp/nostrc-0cfsg13-14-proof-732346a87/`. |

**No current successful MDK full-lifecycle proof:** the prior live 0.11 **5/5 smoke is historical command/CTest evidence only**. Its old `cleanup_complete=true` and successful-cleanup claims are superseded, not silently accepted because CTests passed. No new live smoke or arbitrary-tree quiescence proof was obtained after the fix. Standalone helper command/image/consumer evidence, scoped cleanup results and complete process-tree ownership are separate. Selector custody/finalization and the inherited-FD failed/archived/not-shipped proposal remain unsupported; no fallback/transport redesign is authorized.

**Verification provenance:** A-branch core regressions passed **251/251** at `732346a87`, with four static/compile/whitespace checks; these are not the combined integration suite. Parent confirms the combined run at **`ae36f5a2639ff1c28d6657621b8afb0d81771b2b`: 255/255 passed, 0 skips, 66.169s** (`/tmp/nostrc-completion-reviewed-selection.log`); **all six shell suites, four static checkers, full actionlint including ShellCheck and whitespace passed**. Subsequent `4956fcf2` changes were docs only; this adds no broad rereview or rollout approval. Earlier 244-test/six-shell/static/actionlint/whitespace results remain receipts at `9c03e7712`, not new runs at this SHA. Native-root **432 passed / 5 skipped** at `84ae0896b` and 32-case libgo diagnostics remain their own source-specific evidence, not current MDK lifecycle or complete acceptance.

**Version and rollout:** no component bump for internal testing-tool fixes/documentation. `.6`, `.10`, `.12`, `.13`, `.14` are closed for their bounded scopes; `.8` and the other original acceptance scopes remain open. Shared sanitizer execution remains explicitly unsupported/refused; `.11` remains unresolved with no threshold/exclusion change. W11/W12, default/enforce/schedule/protection cutover and full ownership/profile union remain unmet/unchanged. This task edits documentation only, verifies links/whitespace and preserves the review findings; parent owns remaining verification, integration/push and rollout decisions.
