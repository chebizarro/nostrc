# W11 runtime (A), AppArmor provisioning and original MDK selected route (D): source-bound handoff

## Context and scope

Base `44044fefe9c40ac095044e9c22eb06f2678aab1f`. Local commits only; parent integrates exact tips. Code tip
`8c385e460771b503d30131af0ad9433b791ff4fc`; this report is a later docs-only commit. Evidence root
`/tmp/nostrc-w11-runtime-20261023` (hashes in the JSON companion). CI/test tooling only: **no component
version bump**. W11 NOT MET; W12 NOT RUN; no push, default, protection or publisher change.

| Commit | Slice | Status |
|---|---|---|
| `9c7a82c5f` | A signer: instrumentation binding, ELF `__asan_init` proof, caller-attestation preflight, sanitizer-report scan, graceful approver, signer-route worker | frozen; not approved/merged |
| `b5de1dae0` | AppArmor v1 (static name, `--replace`) | **not approved as-is**; superseded by `c872f8da2` |
| `82fe09ff7` | D: selected original MDK controller, `mdk-docker`, `recover`, gate admission + recovery | for review |
| `c872f8da2` | AppArmor safety revision: hosted-only, fresh per-job name, `--add`, exact owned teardown | for review |
| `877bc14ba` | A: whole sanitizer-env projection (Oracle P1) | for review |
| `8c385e460` | D: production/use evidence, Docker access accounting, setup-recovery journal (Oracle P1) | for review |

## Findings (actual evidence, not inferred)

1. **Hosted policy failures (PR22 run 39922245779, tested `4527a06e`):** `policy_regressions` 120310801700 and
   `broad/static/regressions` 120310802627 both stop at `kernel-prerequisite`. On kernel 6.17.0-1022-azure x86_64 with
   `apparmor_restrict_unprivileged_userns=1`, an unconfined task is moved to `unprivileged_userns (enforce)` at
   `unshare(CLONE_NEWUSER)`, then `/proc/self/setgroups` open fails with EACCES. The native runner probe shows the
   same. `policy/execute` was never created. Everything after was not executed.
2. **Amd64-emulated StoreKey abort:** causal. Under Rosetta, `/proc/<pid>/exe` of a guest process is
   `/run/rosetta/rosetta` (different inode); `signer_caller_is_approver` correctly denies `StoreKey`. Native arm64
   control matches. Fixture now refuses with typed `caller_executable_identity_unavailable` at the bus stage before
   any key mutation (real Rosetta run: only bus started, reaped, private namespace absent). Attestation unchanged.
3. **Hosted sanitizer signer refusals** (`core_asan` 120310801461, `grotto_asan` 120310801692): `signer_endpoint` is not
   authored on those profiles; refusal kept. A engine contract added; B profile authoring required.
4. **Hosted MDK failures** (`mdk08` 120310801546, `mdk011` 120310801961): resolve refuses before any Docker:
   six checks claim MDK-profile registrations needing `gnome_keyring`/`nak`/`signer_endpoint` that the MDK
   profiles do not author (B decision). Behind that the engine and gate refused any `docker` lifecycle.
5. **MDK ordering (physical):** configure-only real MDK011/MDK08 trees (native macOS structural proof): no
   `build.ninja` edge references the wrapper; configure succeeded with the wrapper refusing outside a lifecycle; the
   wrapper appears only in three `build -t` image-producer fixtures (`FIXTURES_SETUP`) and in consumer
   `ENVIRONMENT` strings that `FIXTURES_REQUIRED` them (`ctest-mdk-trace.json`). Order is configure -> prerequisite
   build -> full post-build audit (binds contexts/helper/wrapper) -> lifecycle -> fixtures -> peers.
6. **Pidns + Docker socket:** kernel keeps supplementary kgids under `setgroups=deny`; a new Linux cell observes the
   namespace target's `Groups:` from the original namespace. **Written, not yet executed** (Docker held). Lifecycle
   records socket identity before/after (no chmod) and states broker processes are external, not owned.
7. **Bounded Oracle review** (export retained `oracle-review-20261023.md`): P0 (`systempaths` in `SecurityOpt`)
   refuted by real local and hosted inspects; both P1s fixed with regressions (setup-kill recovery regression shown
   to fail without the fix).

### Linux execution status (native arm64, not x86/hosted/profile)

* Valid run on `91d7772be` (before review fixes): kernel/custody/service/signer cells 158/160, 0 skips (2 B
  placement-witness pins); real ordinary fixture 6/6; real ASan fixture 6/6 with `detect_leaks=1:halt_on_error=1`;
  ELF proof `asan:true` for daemon/approver/client; injected approver leak fails with `ERROR: LeakSanitizer`.
* Retained invalid: run1 (image lacked uid-1001 passwd entry; bus closed), run3 on `b5de1dae0` and the final full
  suite (host APFS ENOSPC at ~09:30Z, `capacity-boundary.json`). No result from them is counted.
* **Not yet run on the final tips:** Linux kernel/fixture proof, supplementary-group cell, full selection suite,
  any real Docker. Held for parent capacity/daemon confirmation; two owned containers
  (`5944b107…`, `de340b27…`) await exact-ID removal.

## Recommendations / interface for B and parent

* **B enrollment (exact, at `8c385e460`):** add `selection.contract` inputs `scripts/mdk-docker`,
  `scripts/test_selection_mdk.py`, `tests/selection/apparmor/nostrc-namespace-custody`,
  `tests/selection/namespace_apparmor_setup.sh`, `tests/selection/namespace_apparmor_teardown.sh`,
  `tests/selection/test_mdk_selected_route.py`, `tests/selection/test_namespace_apparmor_provisioning.py`; refresh
  `dependency-worker.yml:execute` (workflow `914af0df…`, source `fc0c139a…`) and
  `namespace-policy-regression.yml:regressions` (workflow `428a5816…`, source `940ee3f4…`); refresh placement
  witness for `scripts/test-gate.sh` and the historical policy/worker-setup source pins. Full values:
  `b-enrollment-tip.json`.
* **B profile authoring:** `interface-signer-runtime-v1.json` (sanitizer signer variants) and
  `interface-mdk-original-v1.json` (MDK domain `mdk_original_linux_v1`, `GROUNDHOG_DOCKER={source_root}/scripts/mdk-docker`,
  capability-check membership decision).
* **B engine wiring (MDK):** `_runtime_capability_evidence('docker')` -> `test_selection_mdk.contract`; in
  `run_resolved`, span CTest with `test_selection_mdk.lifecycle(realization, resolved, input_audit, dir, environment=env)`
  (alongside `provisioned_signer` when both apply), store `receipt['mdk_runtime']`; aggregation calls
  `validate_runtime(backend, plan, lane, profile)` with `backend['tests']`.
* **Seam 4, normal contract (proposal, no schema change yet):** keep `mdk_original_runtime_v1` / signer records
  `complete=false`. A B-owned `normal_execution_v1` acceptance may be granted only by B's validator independently
  re-checking: authored profile + full pre/post audits; outer pidns post-exit seal with no MDK recovery needed;
  typed MDK/signer runtime passed with `production_complete` and `docker_cleanup_complete`; every selected
  registration executed (declared XFAIL only); matching-architecture hosted evidence. Legacy `run`, `images`,
  prepared011 and older diagnostics lack the typed kind/domain and stay unpromotable. No field-clearing.
* **AppArmor:** ratified only for disposable GitHub-hosted runners; real effect unproved until a hosted run of the
  exact revised tip; teardown result must be inspected (`teardown.json`).
