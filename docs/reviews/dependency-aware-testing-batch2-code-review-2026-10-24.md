# W11 batch2 independent code review (.36.25 / .36.18 / .36.23)

## Context and scope

| Identity | Exact value |
|---|---|
| Base | `1d53e6ecdbfa2028a031974a8a66a43f55535167` |
| Reviewed integration (frozen) | `719616996ad313eb4e3724c88400de95b4cdb3a2` |
| Runtime bytes | unchanged in `scripts/` since `edeec682a`; .25 runtime is `276145d1c`, later .25 change `bbecd93ac` is test-only |
| Diff | 34 files, +2,448/−100 (6 added, 28 modified) |

Authors: **.25** sealing of immutable native originals; **.18** per-registration signer credential delivery, consumer JUnit qualification, offline scope proof and the strict historical inverse layer (`490967961`); **.23** four Gnostr loader/link fixes, the recording toast boundary and the removal of their Linux smoke exclusions.

This is a bounded static review. It is not profile, complete-gate, W11, W12 or release acceptance. Defaults, publisher modes, protections, legacy triggers and schedules are unchanged. **No component version bump** is needed: every change is a test, CI, test-registration or diagnostic-tool change; no shipped artifact changes.

Method:
- Read-only review of the exact diff and the callers it depends on.
- Two throwaway CTest projects in `/tmp` (CMake 4.3.2), checking resource-allocation exposure and JUnit status encoding. No repo tests were run.
- One Oracle review call (below).

No source, tracker or evidence was modified. This report is the only file edited.

## Oracle call

- Chat `untitled-chat-C9C6F4`, review mode, a single call. The selection was bound to the `nostrc` worktree `ci/w11-b2-integration-20261024@7196169`, not the default `whitenoise-ios` root. Fresh git artifact snapshot `2026-10-24/1524` (`1d53e6ecd..719616996`: `MAP.txt`, `all.patch`) plus caller slices: `test-gate.sh` finalizer and outer anchor, `provisioned_signer`, `_native_files`, `write_json`, `_run_resolved`, the approver, and the post-1640 inverse.
- Export: `/Users/bizarro/Documents/Projects/nostrc/prompt-exports/oracle-review-2026-10-24-152902-untitled-chat-c9c6f4-aa4b.md`.
- The Oracle found one fail-open issue, in the offline proof tool (F1 below, which I confirmed in source). It confirmed my prior L1, L2, L3 and L4 and the M1 scope assessment. It found no admission bypass at runtime. There was no second review loop.

## Findings (most severe first)

### F1 — Medium, confirmed: the offline signer proof does not bind evidence to the qualified receipt (fail-open in a diagnostic tool only)

`scripts/signer-scope-proof.py:118–129`. `verify_receipt` checks summary fields and only that `original-native-receipt.json` *exists*. It does not validate that file, and it does not tie the qualified receipt to the backend receipt, JUnit and logs that `lane()`/`verify_lane()` inspected (`:67`, matched only by profile and SHA). The positive test writes `{}` as the original (`tests/selection/test_signer_scope_proof.py:66`). **Failure scenario:** pair a genuine qualified same-profile/same-SHA receipt with altered or unrelated lane evidence; the tool still prints `proved: true`. Engine admission (`qualify`) is unaffected.
**Owner .18:** verify the preserved native attestation and bind the inspected receipt, JUnit and log bytes to the hashes behind qualification. Until then, `proved: true` from this tool is not a delivery/isolation receipt.

### F2 — Low, confirmed (resolves prior L3): the proof cannot succeed for three profiles it lists (fails closed)

`signer-scope-proof.py:25–29,109–111` requires all four `WITNESSES`. Checked against the inventory's `ctest_tests` maps at `719616996`:
- `signer/room-flow` is mapped in **no** check for `core.linux`, `core.linux.asan` or `grotto.linux.asan_ubsan`, so those `PROFILES` entries can never succeed.
- In `groundhog.linux`, `groundhog.mdk08` and `groundhog.mdk011`, the witnesses come from `supported.apps.grotto.tests.private_bus` and `.display.private_bus`. In `grotto.linux` they come from `grotto.signer.native`.
- Nothing forces these checks into the same lane as `supported.nips.nip5f.signer_endpoint`. A selected lane succeeds only if they happen to be co-selected.

No receipt exists or is implied for any profile.
**Owner .18:** restrict `PROFILES` to witness sets each profile can actually realize, or co-select them explicitly. Don't weaken the isolation claim.

### F3 — Low, fail-closed: the credential exposure check is narrow (retention gap)

`scripts/test_selection_services.py:699–728`.
- The scan runs only after a normal `yield` (`:719–722`). On cancellation, timeout or an exception it is skipped. Qualification then refuses, because `exposure_checked` is absent, but any secret-bearing bytes stay unchecked in retained output.
- It covers only `ctest.log` and `results.xml`. The retained `build/Testing/Temporary/LastTest.log` and the fixture's own `signer/` logs are left to the offline `scan` (which F1 weakens).
- A detected exposure refuses but leaves the bytes on disk.

Scratch evidence: CMake 4.3.2 put no resource id into `LastTest.log`, JUnit or stdout, and JUnit includes `system-out` for passing cases. Hosted CMake 3.28.3 was not checked.

### F4 — Low, fail-closed: a double-signal window loses the seal

`scripts/test-gate.sh:692–699`. A second SIGTERM between entering `except BaseException` and `pthread_sigmask` raises a new `KeyboardInterrupt` from `stop()`, so nothing is sealed. The anchor stays empty and the outer `attest_native_lane` refuses, so this is a narrow exception to "every outcome is sealed", not an admission defect.

### F5 — Scope note: the approver's attribution is sufficient only for the scoped one-shot-consent claim

`tests/runtime/nip5f_approver.c:44` ignores `app`. Attribution to `test_nip5f_tcp` rests on four things:
- the registration-scoped resource allocation;
- the approver refusing a second request;
- the approval snapshot `(1,1,0)`;
- the consumer's own JUnit case being a real passing run (`signer_consumer_junit`).

Same-UID processes in the namespace can still read the 0600 spec or `/proc/<pid>/environ`. A theft makes the consumer's request the second one, the approver aborts, and qualification fails, so there is no false pass.

**Assessment:** sufficient for "the provisioned signer served exactly one explicit, non-remembered, unidentified `get_public_key` to the delivered consumer". **Not established:** caller identity or `app` attestation, OS-level credential isolation, or any product approval policy. No policy broadening is proposed.

### F6 — Informational: qualification re-checks registration metadata, not ambient absence

`scripts/test_selection_execution.py:283` calls the admission check with `{}`. Absence of ambient `NOSTR_SIGNER_*` / `CTEST_RESOURCE_*` rests on the runtime admission in `_run_resolved` plus the `base_hash` binding to the realized test environment.

### F7 — Evidence-retention gap: .18 integration results are reported by the agent, not verified

The .18 HANDOFF (`E/agent-handoffs/nostrc-0cfsg-36-18-b2-integration-490967961/HANDOFF.md`, with `SHA256SUMS`) states that the per-module logs and generator under `/tmp/b2i` were **deleted before handoff**. No retained copy exists. The reported **39 modules / 465 tests / 0 failures / 8 explicit platform skips** and the eight neutered negative controls are therefore *agent-reported only*. They are not durable, source-bound receipts, and this review does not cite them as verified. The HANDOFF preserves the exact module and control lists and hashes; nothing was reconstructed, backdated or retried. The frozen hosted DEFAULT run of the final source must provide the physical regression proof. This is a retention gap; it is no reason to loosen source rules or authority.

## Verified as sound (no finding)

- **Original bytes vs the trusted writer (.25):**
  - `_trusted_output_bytes`/`_original_output` (`services:1076–1113`) compare raw bytes exactly against `test_selection.write_json`'s serialization (`sort_keys`, `indent=2`, `ensure_ascii`, `allow_nan=False`, trailing newline), with no parse or normalization. A test guards against drift.
  - Re-hashing `_native_files` against the trusted digest closes the compare/hash swap.
  - An early failure (`result=None`) refuses any published-but-unobserved file.
  - Nonzero, cancelled (130) or refused outcomes keep or force a nonzero code, and publication (`native_public_receipt`) happens only after `command()` succeeds.
  - Finalizer outcome text reaches `lane.log` before the seal, and `os._exit` follows.
- **Relative symlink chains:** `sealed_claim_entry` (`custody:121`) follows only relative chains inside the retained tree. It returns the raw link for absolute, `.`, `..` or empty components, and gives up after 40 links. Post-exit attestation still compares every link entry. Legitimate `../` namelinks would be falsely refused, which is safe.
- **Delivery (.18):**
  - Only `test_nip5f_tcp` may declare `nip5f_signer`, checked across the whole fresh universe. The `{'.type','slots':1}` group must match exactly; other selected resource groups and ambient variables refuse.
  - The spec is written with `O_EXCL|O_NOFOLLOW` 0600 into a `mkdtemp` under the private resource root, never under evidence, and removed in `finally`.
  - The argv binding (`--resource-spec-file` before `--timeout`) matches between `_run_resolved` and `qualify`, and the CTest environment overlay must be empty.
  - Scratch CTest confirmed that registrations without the resource get no allocation, and that json-v1 `RESOURCE_GROUPS` has the shape the code compares against.
- **Consumer non-skip:** scratch JUnit encodes pass as `status="run"`, a 77 skip as `status="notrun"` with `<skipped>`, and failure as `status="fail"` with `<failure>`. `signer_consumer_junit` and the proof's `_ran` accept only exactly one real run. Under resource scheduling, `test_nip5f_tcp.c` fails rather than skipping, and never falls back to inherited credentials.
- **Unsupported profile:** legacy macOS ASan has no spec, so `test_nip5f_tcp` still exits 77. That remains accounted as the `.36.9` non-validation, with its bindings re-pinned to the changed source and registration.
- **Gnostr (.23):**
  - Real `mute_filter.c` linkage replaces the unresolved-symbol and `dynamic_lookup` options.
  - The test-defined `gnostr_main_window_show_toast` records calls and is asserted on; it is not a success stub. The real function's behaviour remains untested, as the test states.
  - `BROKEN_ON_LINUX=''` and the smoke script's `'(.+)'` handle the empty list.
  - Both NDB tests still report `not ok # TODO` through `g_test_incomplete`: these are loader fixes, **not** a fix for the main-thread violations, and no changed file claims otherwise.
  - The .23 `summary.json` is honest: bound to `21b2f7b23`/`c326500c2`, arm64 only, not integration-HEAD or profile evidence.
- **Strict historical layer:** `w11_batch2_integration_support.before_source` maps only exact after bytes to before bytes and refuses anything else. The two `restored_sha256` passthroughs (`:36`, on `test_selection_execution.py` and `test_normal_execution_contract.py`) equal post-1640's own before digests exactly; a test proves they differ from this layer's before/after and that a one-byte change refuses. Live drift is still caught by the direct after-hash assertions. The closeout short-circuit and the coreasan/groundhog hand-offs keep each older layer's own strict guard. No masking was found.

## Evidence status at `719616996`

| Claim | Status |
|---|---|
| .25 LinuxKit/aarch64 kernel cohort at `bbecd93ac`: 76/76, zero skips, 45.069 s, including cancellation, early setup, failure and tamper | Parent manifest-verified (57 manifest/cleanup entries, containers/images removed); earlier setup and invalid-fixture failures retained in `E/nostrc-0cfsg-36-25-linux-kernel-cells`. Not hosted x86 or profile proof. |
| .23 retained 71-file proof (`E/nostrc-0cfsg-36-23-batch2-evidence`) | Parent manifest-verified; focused + legacy smoke only. |
| .18 39 modules / 465 tests / 8 skips + 8 negative controls | **Agent-reported only; logs deleted (F7).** |
| Hosted signer delivery/isolation receipt | **None exists.** Do not cite the F1 tool's output until it is fixed. |
| Final frozen hosted DEFAULT | Pending; this is the required physical regression proof. |

## Limits carried forward from the prior review

These still hold, quoted from the [PR25 source receipt](dependency-aware-testing-ninja-readiness-2026-10-24.md):
- `.36.18` credential delivery scope (now implemented, but hosted proof is outstanding); `.36.16` three true ASan exclusions; `.36.24` 58 Grotto sanitizer cases; original MDK `.8`/`.36.13` finalization; `.36.21` hosted Xvfb startup cause; `.36.22` sanitizer partition renewal; native authority and the broader profile union.
- Reviewed acceptance remains **0/20** qualifying representative PR comparisons, **0/2** successful complete broad integrations, and zero real product samples. **W11 NOT MET / W12 NOT RUN.** No cutover or release authority.
- Failed originals stay preserved; no diagnostic is promoted.

## Recommendations to owners (not implemented here)

1. **.18 (F1):** bind `verify_receipt` to the validated original native attestation and the exact inspected lane bytes.
2. **.18 (F2):** restrict proof `PROFILES` to witness sets each profile can realize, or co-select the witness checks explicitly.
3. **.18 (F3):** run the exposure scan on exceptional cleanup without masking the original failure; include `LastTest.log` and the fixture logs; keep detected secret-bearing files out of uploads while recording the failure explicitly.
4. **.25 (F4):** optional. Accept and document the double-signal window, or narrow it, keeping the fail-closed anchor semantics.
5. **Parent (F7):** rely on the frozen hosted DEFAULT for regression proof, and keep the .18 HANDOFF as a record of evidence that was lost.

## Parent resolution after the one review

The findings above remain the historical review of `719616996`; no second Oracle/review pass was performed. Parent inspected the concrete corrections, source bindings, restored guards and retained negative controls:

- **F1/F2 — corrected in `12370c9f4`.** The offline tool reuses `transported_receipts` and `validate_receipt` through `verified_transport`, checks the preserved original against verified public-evidence bytes, and inspects only the verified sealed transport. It now requires the same run's plan and a clean tested-commit checkout. Unsupported witness-profile promises are removed; all witnesses must physically execute in the same qualified lane. Synthetic tests never establish real signer success.
- **F3 — source corrections in `12370c9f4`, `36de6cc94`, `77557f3ab`.** Exact-token scans run through exception/cancellation cleanup and include CTest's `LastTest.log` plus finalized fixture logs. Because this repository is public, the additive worker withholds both raw receipt and evidence artifact sets unless publication is authorized. The no-signer exception uses the existing committed-inventory and canonical-plan validators against trusted workflow plan-id/tested-SHA inputs, exact checkout and exactly one selected lane. Unknown, missing, duplicated, changed or malformed lanes cannot declassify sensitive evidence. Signer lanes additionally require successful qualification, bound original bytes, complete clean exposure records and clean credential-shape scans. Missing/failed/cancelled publication decisions withhold by default. Public withholding metadata is not a receipt or coverage.
- **Confidentiality/retention limit.** Detected originals are never redacted or resealed. Withheld originals remain only on the ephemeral worker until job completion, not durable private escrow; public diagnostics are lost. Outside named exact-token scan paths, detection is by credential-shape patterns, not an arbitrary bare-token guarantee. No existing protected failed originals were removed or rewritten.
- **Retained correction evidence.** Parent independently checked 52 checksum entries for the F1 correction, 125 for publication and 114 for final plan binding. Final focused logs record respectively 333, 420 and 425 tests, each with nine explicit platform skips and zero failures; these overlap and are not additive coverage. Earlier three-failure publication attempt, invalid control harnesses, restore/refresh errors, and initially undetected controls remain separate. Historical unretained 465-test results remain unverified (F7), not replaced or backdated.
- **F4/F5/F6 — limits retained.** The double-signal race fails closed; one-shot unidentified consent is not caller/app attribution or OS isolation; ambient absence rests on runtime admission and bound environment evidence.

Final corrected **code** is `77557f3ab` (the following documentation-only commit records this resolution). Actual final-source native build, hosted DEFAULT/kernel and hosted signer qualification remain pending at this note. No whole-profile, complete-gate, performance, release, W11 or W12 acceptance follows from this review. All source changes remain internal CI/test-only: no component bump. Legacy triggers, defaults, protection and publisher modes are unchanged.

The consumed Oracle export was retained privately at `E/agent-handoffs/batch2-oracle-review-C9C6F4.md`, SHA256 `d3b9100b0a300b1fdbababfdbcff49f33ce49c4b2c0477b29846a31859b85f4d`; the temporary worktree export was removed only after verifying that copy. `E` denotes the protected evidence root used above.
