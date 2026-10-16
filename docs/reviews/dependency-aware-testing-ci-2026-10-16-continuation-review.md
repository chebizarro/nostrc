# Integrated dependency-testing continuation review

## Context / Scope

- **Reviewed source:** `9c03e77126d8f2f092b22bafbd79280fbaf10950`; **baseline:** `5467c02d11f74efc1a85c833024efb654bbf98b5`.
- Verified before investigation: assigned worktree `/Users/bizarro/Documents/Projects/.repoprompt-worktrees/nostrc/rp-agent-6d92d345-review-testing-continuation-2026-6e1de074`, branch `review/testing-continuation-20261016`, exact target HEAD, clean status. RepoPrompt confirmed its `nostrc` logical root maps to this checkout, not master.
- Reviewed introduced execution/aggregation code, CMake deferred registration export, profile/membership evidence and tests, standalone MDK lifecycle, sanitizer metadata parity and explicit execution refusals. The plan and staged receipts supply requirements, not proof that current implementation works. No implementation changes, push, merge, settings change or enforcement/default cutover. **No component bump: report only.** Parent owns corrections/integration.
- Fresh RepoPrompt Git snapshots: `2026-10-15/1958` (all 30 changed files) and `2026-10-15/2001` (15 relevant implementation/test files), under `_git_data/repos/rp-agent-6d92d345-review-b36e4505/`. A separate eight-implementation-file scoped patch was generated directly from the exact two commits; SHA-256 `3c2ac2a1e8d6dc0317cd1836c73b4e1f1ce3451ba116f7fb660101d927b10248`.
- **Oracle limitation:** one fresh review call only (`untitled-chat-DCD260`). Local reads/selection showed actual bound source and the materialized scoped patch, but the provider response explicitly reported receiving only stale documentation/CMake excerpts, not those implementations or patch. **No provider integrated-source review or provider patch verification is claimed.** No identical retry; findings below are independently source-verified/reproduced.

## Findings

All three findings are **confirmed P2**, ordered by practical safety/result-integrity impact. None demonstrates a complete/release gate bypass.

### F1 — MDK cleanup declares success while a detached descendant remains alive

**Locations:** `scripts/mdk-gate.py:89–103,132–139,315–318`. **Tracking:** `nostrc-0cfsg.12`.

`session_members` only sees processes whose current SID equals the original guarded child's PID. A grandchild created with `start_new_session=True` is therefore invisible. Once the direct child exits, `stop_tree` finds no original-session member, reaps the leader, and permits `cleanup_complete=true` / exit 0 even though that grandchild is still running. This is not merely the explicitly rejected *different process group within the same session* case: that case has a regression and fails correctly. The new-session case escapes both the cleanup action and its failure accounting. Separate-session launches are also used by the shared command runner (`test_selection_realization.py:67–69`), so SID stability cannot be assumed across arbitrary guarded commands.

**Reproduction:** run from the reviewed checkout; fake Docker fixtures, real host processes, no live Docker or selector custody claim:

```python
import os, signal, subprocess, sys
sys.path.insert(0, 'tests/selection')
from test_mdk_cleanup import LifecycleCleanupTests, BUILD
c = LifecycleCleanupTests(); c.setUp()
try:
    result, receipt, state = c.run_guard(BUILD + '''
orphan=subprocess.Popen([sys.executable,'-c','import time;time.sleep(60)'],
    start_new_session=True, stdin=subprocess.DEVNULL,
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
(state/'grandchild.pid').write_text(str(orphan.pid))
''')
    pid = int((state/'grandchild.pid').read_text())
    try:
        print(result.returncode, receipt['status'],
              receipt['cleanup_complete'], os.getsid(pid) == pid)
    finally:
        # Only the test's still-live, identity-checked descendant.
        if os.getsid(pid) == pid:
            os.kill(pid, signal.SIGKILL)
finally:
    c.doCleanups()
```

Observed: **`0 command_succeeded True True`**. The last `True` proves the new-session grandchild still exists after purported cleanup. The review explicitly killed that test process; no test descendant was intentionally left behind. The MDK receipt's existing `complete=false` does not correct the false process-cleanup assertion.

**Recommendation:** add chain-safe ownership/cancellation across session changes, or constrain the supported launch contract so complete process-tree cleanup is actually established. Do not infer it from the original SID alone, and do not substitute global/name-based kills or unreserved PID signaling. Add a `setsid`/`start_new_session` regression alongside the existing `setpgid` regression.

### F2 — Omitting the outer diagnostic flag hides a nested negative eligibility decision

**Location:** `scripts/test_selection.py:492–508`. **Tracking:** `nostrc-0cfsg.13`.

The new contradiction check compares diagnostic flags only when both layers contain them. Unlike the nested complete-eligibility check, the actual diagnostic rejection consults only `receipt.diagnostic_eligible`. A partial receipt that retains `backend_receipt.diagnostic_eligible=false` but omits its outer duplicate is accepted and re-emitted as `diagnostic_eligible=true`. Thus a lower-layer negative decision is promoted to positive diagnostic acceptance rather than rejected. Normalization normally copies the field; this finding concerns the aggregator's receipt-consistency boundary, including omission/legacy-shaped inputs, not an assertion that ordinary normalization always produces this shape.

**Reproduction:** existing native-shaped policy adversary, no fabricated native execution claim:

```python
import sys
sys.path.insert(0, 'tests/selection')
from test_diagnostic_eligibility import DiagnosticEligibilityTests
from selector_support import s
p, plan, receipt = DiagnosticEligibilityTests().native_shape()
receipt.pop('diagnostic_eligible')
receipt['backend_receipt'] = {
    'diagnostic_eligible': False, 'complete_gate_eligible': False}
a = s.aggregate(p, plan, [receipt], complete=False)
print(a['passed'], a['diagnostic_eligible'],
      a['complete_gate_eligible'], a['errors'])
```

Observed: **`True True False []`**. Complete aggregation remains blocked by the preserved negative completeness flag and inventory gaps; **no release bypass is claimed**.

**Recommendation:** reject a negative diagnostic decision from either layer independently of duplicate-field presence. Test omitted outer fields as well as contradictory present fields, while retaining complete/release restrictions and independent execution/job accounting.

### F3 — Non-UTF-8 command output can strand a running receipt and discard the output

**Locations:** `scripts/test_selection_realization.py:67–70,89–107,1315,1509`. **Tracking:** `nostrc-0cfsg.14`.

The new command path uses strict text-mode subprocess decoding. An arbitrary command can write non-UTF-8 bytes, close its output descriptors, and remain alive. `communicate()` then raises `UnicodeDecodeError`; process cleanup runs, but `run_resolved` does not catch that exception class to finalize its receipt. The initial receipt remains `outcome=running`, `job_conclusion=success`, with empty checks/observations, while the command log is empty because the offending bytes were never assigned to `output`. A process sidecar records failure in this reproduction, but it does not finalize the lane receipt or preserve the lost bytes. The caller raises, so this is **failure-evidence loss/incomplete accounting**, not a green gate.

**Reproduction:** actual new source-only execution path using the existing miniature fixture; no mocks of process success:

```python
import sys
sys.path.insert(0, 'tests/selection')
from test_command_execution import CommandExecutionTests
from realization_support import r
c = CommandExecutionTests(); c.setUp()
try:
    c.command("import os,signal; os.write(1,b'\\xff'); "
              "os.close(1); os.close(2); signal.pause()\n",
              required_evidence=[])
    c.inventory['checks'] = {'command.actual': c.inventory['checks']['command.actual']}
    c.profile['configure_args'] = []
    resolved, audit = c.ready(source_only=True)
    try:
        r.run_resolved(resolved, c.build, c.evidence,
                       input_audit=audit, build=False)
    except UnicodeDecodeError:
        d = r._attempt_directory(resolved, c.evidence, 1)
        receipt = r._read(d/'receipt.json')
        print(receipt['outcome'], receipt['job_conclusion'], receipt['checks'])
        print((d/'command-command.actual.log').read_bytes())
finally:
    c.doCleanups()
```

Observed: **`running success {}`**, then **`b''`**; process sidecar records `outcome=failed`, exit `-15`, and `UnicodeDecodeError`. A shorter command that immediately exits after emitting the invalid byte also loses its log; on this Darwin host its cleanup can instead raise zombie-group `PermissionError`, leaving process-status fields absent. The closed-output/live-child reproduction above avoids that secondary race and deterministically demonstrates the unfinalized receipt.

**Recommendation:** retain raw output bytes or use an explicit preservation-safe decoding policy; finalize failed attempt receipts for decoding errors and preserve the original error/output even if cleanup itself fails. Add a real invalid-byte command regression. Strict decoding also existed in the old subprocess primitive; the introduced defect assessed here is its integration into the new arbitrary-command/retained-receipt path, not a claim that strict decoding itself was newly added.

## Verification / coverage boundaries

### Independently executed at the target source

| Check | Result |
|---|---|
| `python3 -m unittest discover -s tests/selection -p 'test_*.py'` | **244/244 passed**, no skips, 59.600 seconds; includes actual miniature CMake/native/CTest/command subprocesses and fake-Docker lifecycle fixtures. |
| Exporter compatibility suite with actual CMake/CTest **3.22.6** first on PATH | **6/6 passed**, 0.517 seconds. Not a newer binary with an older minimum declaration. |
| Same suite with actual CMake/CTest **3.25.1** first on PATH | **6/6 passed**, 0.369 seconds. |
| F1/F2/F3 probes above | Reproduced the stated defects independently of the passing regression suite. F1's surviving test process was killed explicitly. |

Local transient evidence: `/tmp/nostrc-continuation-review-tests.log`; `/tmp/nostrc-continuation-review/{cmake322.log,cmake325.log,reproduce.py,reproduce.log,invalid-command-open.log}`. These are local diagnostics, not durable hosted acceptance receipts; executable reproductions are included above so findings do not depend on those temporary files surviving.

No additional introduced CMake exporter defect was confirmed in the reviewed cases. The exercised compatibility tests cover deferred registration/property mutation, both native `add_test` signatures and argument boundaries, directory-local properties, empty intermediate directories, standalone ownership, and failure on missed collection.

### Intentional incompleteness — not new findings or acceptance

- Actual profile source/tests retain **32** native libgo members with the stress registration explicitly unexecuted; root memberships **267/268** after fixtures/exclusions; Groundhog sanitizer metadata **62** registrations. Linux/arm64 capture is explicitly not declared hosted x86_64 execution evidence. Checked-in export summaries/hashes and source-owner assertions do not independently prove every semantic/runtime edge or full profile union.
- Prior native-root QA reports **437 cases / 432 passed / 5 skipped**, Groundhog OFF; no rendered GUI/live acceptance. It was at a separately identified source SHA, **not rerun here**. Native libgo's 32-case diagnostic run is not full coverage or completed stress/profile acceptance.
- `docker` / `external_peer` remain refused by the shared backend (`test_selection_realization.py:1363–1364`). The broken inherited-custody prototype is not present in this diff and is not treated as delivered functionality. Standalone MDK receipts are explicitly not selector admission.
- Phase-specific sanitizer execution remains explicitly unsupported (`test_selection_realization.py:303–305`, `test-gate.sh:219–220`, `linux-gate.sh:89–92`). Phase-aware `shared_config` parity is metadata validation only; it does not establish configure/build/test phase application, post-build/runtime closure or ASan/LSan execution eligibility. No relaxed skip policy or legacy-as-shared fallback was accepted.
- Production `selection.contract` source-input metadata remains an acknowledged open integration seam, not a newly discovered regression from this review. No full build, GUI/private-service/live-peer/provider, complete sanitizer matrix, hosted CI, W11 or operator cutover gate was executed or satisfied here.

## Recommendations / handoff

Parent should address **F1–F3** through the three open beads and add the targeted regressions before relying on the new lifecycle/diagnostic evidence. Preserve complete-gate refusal, legacy gates/triggers/schedules, explicit unsupported routes, and all keychain safety requirements. This review makes no implementation correction and grants no rollout approval. Only this report is committed; no merge or push.

## Resolution status after source fixes

**Dated continuation, 2026-10-16:** integration `ae36f5a2639ff1c28d6657621b8afb0d81771b2b`. The report committed as `24b7bb987` above remains the historical independent review of **`9c03e77126d8f2f092b22bafbd79280fbaf10950`**. Its F1–F3 findings, severity, reproductions and original evidence are intact. This appended section records fix commits, bounded tests and parent source spot-checks; it is **not a broad rereview, provider/Oracle review, or rollout approval**.

| Finding / bead | Resolution and bounded evidence |
|---|---|
| **F1 / `.12` CLOSED** | `85a2e2ab633e77157b2c8ede80449e68977ed0d4` removes the false cleanup attestation, **not the underlying arbitrary-tree ownership limitation**. MDK helper receipts now unconditionally set `cleanup_complete=false`, `complete=false`, `cleanup_results.arbitrary_process_tree=unverified`. Otherwise successful command phases retain `command_status=succeeded` / `command_exit_code=0` but produce nonzero `cleanup_unverified` (or `cleanup_failed` for scoped errors). Original-session and recorded-container results are separate; original command failure/cancellation and cleanup errors remain explicit. Real detached-descendant regressions prove refusal while the descendant remains alive, then release only the fixture through its private identity-bound connection/EOF. Owner reports **16/16 focused MDK tests** and **248/248 branch selection tests** passed, using fake Docker plus real host processes/CTest, not new live acceptance. **`.8` remains OPEN** for actual process ownership/lifecycle/custody. |
| **F2 / `.13` CLOSED** | `83a38d879c82429b07dd627a36a64e52e9810c1b` rejects `diagnostic_eligible=false` from either outer or nested backend receipt, including omitted outer duplicates. Focused eligibility **8/8** and A-branch suite evidence below; positive diagnostics still cannot establish complete-gate acceptance. |
| **F3 / `.14` CLOSED** | `732346a87bf22df15ea1e11040b94f9e7eca01bc` captures/stores/hashes **raw bytes**, exposes lossless UTF-8/`surrogateescape` text, marks in-flight checkpoints `job_conclusion=running`, and finalizes failed receipts for unexpected execution exceptions. Original errors/output and separate cleanup errors are retained; direct-child termination/reap fallback handles group-signal denial. Real byte-command regressions cover invalid bytes/CRLF/NUL, immediate exits 0/7/77, a closed-output/live-child timeout, group `PermissionError`, and unexpected strict-text consumer failure. A branch reports **251/251 selection tests passed** at this commit, plus four static checkers and compile/whitespace checks. Copied real miniature receipts are retained at `/tmp/nostrc-0cfsg13-14-proof-732346a87/summary.json`; they are not native-profile/live/complete acceptance. |

**Superseded cleanup claim:** the prior standalone MDK 0.11 smoke's **5/5 CTests** remains historical command/CTest evidence only. Its old `cleanup_complete=true` / successful-cleanup claim is superseded by F1 and `85a2e2ab`; it proves neither detached-tree quiescence nor current successful full lifecycle. There is **no current successful full-lifecycle proof**, no new live smoke, and no selector custody/finalization acceptance. The inherited-FD proposal remains failed/archived/not shipped.

Parent performed source spot-checks of the fixes, **not a broad rereview**. Parent confirms the combined suite at **`ae36f5a2639ff1c28d6657621b8afb0d81771b2b`: 255/255 passed, 0 skips, 66.169s** (`/tmp/nostrc-completion-reviewed-selection.log`); **all six shell suites, four static checkers, full actionlint including ShellCheck and whitespace passed**. Subsequent `4956fcf2` changes were docs only; this is result accounting, not a broad rereview. The earlier 244-case independent review run and the newer A-branch 251-case result are distinct receipts, not the combined result.

Local evidence anchors (temporary files, not durable hosted artifacts):

```text
7410fc6faca6474337230f0bcf1f8d53df104dc15b2210d091f32b27fe4427c6  /tmp/nostrc-0cfsg12-detached-reproduction.log
4da335925dcfd6a31287b8067798c4b70d5b5172254f78e002de3dd94ca1d940  /tmp/nostrc-0cfsg13-14-proof-732346a87/summary.json
```

**Version:** no component bump for these internal testing-tool fixes or this documentation continuation. Original `.7`/`.8`/`.9`/`.11` and rollout scopes remain open. No default/enforce/latency-threshold/exclusion/schedule/protection change; no completion or W11/W12 acceptance is claimed. Parent owns integration/push and remaining acceptance.
