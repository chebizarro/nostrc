# Dependency-aware testing — staged implementation receipt

**Integration snapshot:** `017039eea6620caea791162e1646982abf3d69f4`, fast-forwarded into the assigned `.5` worktree. **Tracker:** `nostrc-0cfsg.5`, remains open/in progress. **Status:** explicit tooling/workflow interfaces implemented; **rollout acceptance NOT MET**. Documentation-only version assessment: **no bump**. No settings change, default cutover, schedule removal, merge back or push by this task; parent owns landing/push/cleanup.

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
