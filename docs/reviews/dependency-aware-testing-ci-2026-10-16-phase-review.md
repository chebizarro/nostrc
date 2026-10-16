# Bounded phase execution and CI adapter review

## Context / Scope

- **Reviewed source:** `83dfea3bbb39f48dd9ab9e6bc17fe2148b2d013b`; **baseline:** `33a1012767c9db5a46057280178134a544b86af7`.
- Verified clean initial status, exact HEAD, branch `review/testing-phase-execution-20261016`, and RepoPrompt's `nostrc` binding to `/Users/bizarro/Documents/Projects/.repoprompt-worktrees/nostrc/rp-agent-ae2c2ba3-review-testing-phase-execution-2-2f3e9948`. The actual range changes 21 files (+2363/-118).
- Reviewed actual introduced phase environment application/fingerprints/cache/source/template audits and compatibility targets; outer command materialization/cancellation/evidence; shared sanitizer phases/parity/locks/failure accounting; raw pipe capture; independent shadow routing and conclusions.
- Fresh RepoPrompt diff snapshots: `2026-10-15/2132` and `2026-10-15/2132-2`, under `_git_data/repos/rp-agent-ae2c2ba3-review-9c222d2d/`. An independently materialized eight-runtime-file Git patch had SHA-256 `4d31556a1c530e089e9eb10d4276549866668af698074fb5bed2008618261764`; selection preview verified its actual contents. The temporary input file was removed afterward.
- **Oracle limitation:** exactly one REVIEW call, `untitled-chat-970A1B`. Despite the verified local source/patch selection, the provider explicitly reported receiving stale documentation/short excerpts rather than selected runtime sources and stopped. **No successful Oracle actual-source review is claimed; no retry.** Findings below are independently source-verified/reproduced.
- No implementation edits, merge, push, enforcement/default/schedule/legacy-trigger changes, or MDK custody acceptance. **No component bump: report only.** Parent owns corrections, final gates and landing. Known C integration metadata is excluded from new runtime findings.

## Findings

### R1 — Confirmed P2: cancellation kills the backend beneath supported isolation wrappers before evidence finalization

**Locations:** `scripts/test-gate.sh:73–78,90–100,246–253,371–374`. **Tracking:** `nostrc-0cfsg.22` (open).

Cancellation captures descendants of `active.pid`, sends TERM to the outer group, then immediately sends KILL to every captured descendant **before** its bounded wait. Without wrappers, `active.pid` is the Python backend and is excluded from that list. With `private_bus`, the direct child is `dbus-run-session` and the backend is itself a descendant; display wrappers can similarly add intermediary processes. The intended drain/finalization boundary therefore kills the backend it is meant to preserve.

**Actual reproduction:** native Linux/arm64, real owned processes in existing immutable image `sha256:e965b342b0aa8c9c2fb9b1cd5b96230e711f630ac5bae90923df4619fa30ca5c` (`nostrc-linux-ci:arm64`), source read-only, network disabled. Reuse the existing socket-readiness test with only the supported `private_bus` capability added; no sleep, retry, synthesized receipt, or production Groundhog execution:

```bash
docker run --rm --init -i --network none \
  -v "$PWD:/review:ro" -w /review -e PYTHONDONTWRITEBYTECODE=1 \
  nostrc-linux-ci:arm64 python3 - <<'PY'
import sys
sys.path.insert(0, 'tests/selection')
from test_local_command_adapter import LocalCommandAdapterTests
c = LocalCommandAdapterTests(
    'test_actual_command_cancellation_keeps_evidence_and_stops_owned_child')
c.setUp()
c.policy['profiles']['local.command']['capabilities'] = ['private_bus']
c.save_policy()
try:
    c.test_actual_command_cancellation_keeps_evidence_and_stops_owned_child()
finally:
    c.doCleanups()
PY
```

Observed: assertion `tests/selection/test_local_command_adapter.py:234` fails because **no retained `command-*.log` contains `b'command ready \xff\n'`**. Inspection before cleanup showed no command log and the attempt receipt still **`outcome=running`, `job_conclusion=running`, `observed_executions={}`**. Outer correctly exits **130**, records **`status=cancelled`, `complete=false`**, and removes its worktree. The test's earlier owned-child cleanup assertion passed; the container exited and no review-owned containers remained.

Existing unwrapped cancellation and shell CTest cancellation tests pass; neither covers this wrapper topology. The reader-thread capture cannot retain/finalize evidence after the outer adapter KILLs its process. This is cancellation evidence/accounting loss in the newly supported outer command route, **not a complete/release gate bypass or proof that the baseline raw-byte fix itself fails**.

**Recommendation:** preserve a bounded backend cancellation/drain/finalization boundary through supported wrappers before forced descendant cleanup. Distinguish backend ownership from wrapper/test descendants without global/name-based kills or sibling signaling. Add real private-bus and display-wrapper cancellation regressions asserting raw bytes, final negative eligibility and owned-process cleanup. Parent implements the correction.

No other introduced runtime defect was confirmed in the bounded paths examined; this is not an unbounded production-readiness assessment.

## Verification / Evidence

### Independently executed at the assigned source

| Check | Result |
|---|---|
| `python3 -m unittest discover -s tests/selection -v` | **306 run, 304 passed, 2 errors, no skips; 305.293 seconds. Not green.** Errors match the known pending C metadata below. |
| Focused members of that run | Phase environments **20**, signal-byte capture **3**, local command adapter **7**, shared sanitizer miniature **5**, workflow contracts **35** passed. Includes real miniature configure/build/CTest/commands, cache rejection, source/template audits, compatibility app build, first-failure handling and independent conclusion adversaries. |
| Preserved baseline-fix regressions | Detached MDK cleanup **4**, diagnostic eligibility **8**, command output bytes **5** passed. |
| `bash scripts/test-test-gate.sh` | Passed: exact pushed SHA, multi-ref union, dirty snapshot/index preservation, real resolve/build/run, CTest cancellation, lock release, capability refusal and cleanup. Does not invalidate R1. |
| `actionlint .github/workflows/dependency-tests.yml .github/workflows/dependency-worker.yml` | Passed. |
| Changed runtime Python compilation; `bash -n` on both gates; `git diff --check 33a1012..83dfea3` | Passed. |
| R1 adversary | Confirmed on native Linux/arm64 with real Docker/processes; not a sanitized application acceptance run. |

Local logs: `/tmp/nostrc-phase-review-tests.log`, `/tmp/nostrc-phase-review-outer-shell.log`. The R1 fixture was cleaned up; the reproduction above does not depend on temporary files surviving. An initial macOS private-bus probe did not reach execution because the installed D-Bus daemon could not start its local session configuration; the decisive reproduction is Linux/arm64, not a claimed macOS private-bus result.

### Known pending C metadata — not new runtime findings

The two suite errors at this exact target are:

1. `test_profile_contracts.WorkflowMatrixAccounting.test_every_existing_job_matrix_has_a_mapping_or_explicit_gap`: unaccounted `.github/workflows/dependency-tests.yml:{core_asan,core_native,groundhog_sanitizers}` records (84 → 87).
2. `test_selection_command_contract.SelectionCommandContractTests.test_production_explicit_source_closure_and_nonmutating_environment`: missing explicit runtime inputs for `tests/selection/{test_command_signal_bytes.py,test_local_command_adapter.py,test_shared_sanitizer_execution.py}`.

These are the user-identified C-owned pending updates. No C file was edited and no final C commit was supplied/applied. They are not relabeled new runtime bugs or hidden behind a green suite claim.

### Source-grounded preserved boundaries

- Exact configure/build/test maps, `{source_root}`-only substitution, effective phase hashes and source/configuration context are validated/revalidated (`test_selection_realization.py:415–555,601–754`). Template inputs receive ownership/content audits (`1334–1352`); compatibility closures include app prerequisites, Ninja/header inputs and artifact presence (`1069–1114,1244–1265`). These are actual source paths, not metadata-only acceptance.
- Shared sanitizer execution uses exact workflow/profile parity, same-process phases, shared-build then exclusive-build/test locks, capability checks, one attempt and partial aggregation (`sanitizer-gate-ci.py:481–547,578–659`; `linux-gate.sh:90–113,259–268`). Unsupported retry/quarantine shapes fail explicitly; legacy fallback is not shared proof.
- Three new profiles have distinct reusable calls and gate `needs`; `LANES` gives independent conclusions (`dependency-tests.yml:72–111,149–152`; `dependency_contract.py:14–17,71–95`). macOS runner/native-root setup is separate from standalone libgo (`dependency-worker.yml:18,64–73`). This is inspected routing plus local contract tests, **not hosted execution or complete coverage**.
- Baseline F1 remains fail-closed: MDK command success becomes nonzero `cleanup_unverified`/`cleanup_failed`, always `cleanup_complete=false` (`mdk-gate.py:328–348`). Baseline F2 rejects negative diagnostic decisions in either layer (`test_selection.py:510–513`). Baseline F3 raw-byte/surrogateescape capture and exception finalization remain (`test_selection_realization.py:56–221,1651–1697`); R1 is a separate outer boundary.
- Explicit exclusions remain unexecuted; diagnostic success is not complete eligibility. No full root build/CTest, production Groundhog ASan/LSan, live GUI/provider/peer, complete matrix, hosted Actions, W11 or cutover acceptance was run/granted here. Parent-reported root build at `7fd` and prior sanitized MINI 1/1 remain separate-source/bounded evidence, not fresh target production acceptance.

## Recommendations / Handoff

Parent should fix **R1 / `nostrc-0cfsg.22`**, land C's already-owned metadata reconciliation, and execute final integration gates. Preserve F1–F3, unsupported-capability refusals, incomplete/diagnostic distinctions, legacy requirements/triggers/schedules and keychain safeguards. Only this report is committed; no rollout approval.

Beads onboarding/hooks were checked; `bd dolt pull` reports **`Error 1105: no remote`**. R1 was filed using `bd`; no `.beads` files were hand-edited. No push/merge, as explicitly assigned. Jev was used only for navigation (19 requests, 141357 input tokens, about $0.005938); audited functions/diffs were read directly and no finding rests on its judgments.

## Resolution status — 8179275b2 guard only

**R1 / `nostrc-0cfsg.22` CLOSED by prelaunch refusal only**, source fix **`8179275b27c2ab20b1e4c41a88b07c91ae0e64f5`**, integrated at `2d64a7ac22bcc08018d4ffdec91fee800f54df6f`. The original review/finding above is preserved at `83dfea3bb`; this appended source-fix/owner-evidence status is **not a rereview**, successful Oracle review or positive wrapped-execution acceptance.

- `scripts/test-gate.sh` now rejects a selected lane containing any non-CTest `command`/`build_contract` when its profile/check capability union needs `display` or `private_bus`, including mixed CTest/non-CTest lanes. Refusal occurs **before wrapper/backend/build launch**, with an explicit unsupported-custody reason. No capabilities or selected checks are dropped, no silent unwrapping and no fabricated backend receipt. This closes unsafe admission, **not the original drain/custody implementation**.
- Owner's actual focused proof: **15/15 native macOS passed, 83.480s** and **15/15 native Linux/arm64 passed, 15.938s** in the immutable `e965…` image. The new regression covers **30 refusal combinations** (three wrapper capability sets × five selected-kind/mixed-lane sets × profile/check capability sources), alongside retained unwrapped cancellation evidence. Logs: `/tmp/nostrc-022-refusal-native-focused.log`, `/tmp/nostrc-022-refusal-linux-focused.log`; handoff `/tmp/nostrc-022-refusal-handoff.txt`. Counts/times and the guard were checked during this docs pass; tests were not rerun here.
- Legacy CTest cancellation/cleanup is unchanged. `.16` CLOSED only for bounded **unwrapped ordinary commands** and the previously proved actual shared Docker phases; `.15`/`.17`/`.18`/`.19`/`.21` closed for their bounded scopes. `.20` closed as a duplicate, **not an implemented fix**. Earlier 306-test parent/C command proofs keep their exact source identities; parent final combined at **`2d64a7ac22bcc08018d4ffdec91fee800f54df6f`: 307/307 passed, 0 skips, 125.303s**, `/tmp/nostrc-phase-final-selection.log`. Only docs/failure forensics changed afterward; final six-shell/four-static/actionlint/whitespace gates at `97e393cba` remain **running/pending**, not assumed passed. No final full-slice acceptance.
- **`.23` OPEN:** full safe wrapped custody is **not implemented**. The abandoned reserved-group draft/probes left a stubborn wrapped CTest child alive despite a finalized negative receipt. Archive `/tmp/nostrc-022-abandoned-wrapper-draft` is outside the repo/**not shipped**; no reserved-group drain success or positive wrapped acceptance follows.
- **`.24` OPEN:** original full-gate fixture invocation failed with its trace lost; the later diagnostic `bash -x` pass is **not a causal fix** or replacement for that failure. Forensics `967172f77` now preserves failed private fixtures/logs with command/line/phase/call stack; success cleanup is unchanged. Four injected negative exits **1/1/37/1** were retained and B's fresh full-gate fixture passed; the original cause remains unknown and `.24` stays OPEN. MDK cleanup/custody, `.11`, production/hosted/full-union and W11/W12 remain unaccepted; defaults/enforce/schedules/protections are unchanged. **No component bump:** internal testing/docs only.
