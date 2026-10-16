# Dependency-aware testing CI — independent implementation review

## Context / Scope

Reviewed `ci/dependency-tests-integration-20261015` at `48e8d2b0e72cf0f31afa463d838d87e6f8585d0d`, against `9b8dc253c`, using the actual integrated source/diff and the specification in `docs/plans/dependency-aware-testing-ci-2026-10-15.md`. Scope: selection/aggregation, native realization and input audits, local snapshot/resource ownership, reusable workflow trust boundaries, and release applicability/artifact gates. The announced pending backend exclusion-accounting correction is **not included** in this reviewed tree.

This is not approval for cutover. The supported-profile union, non-CTest/live-MDK/sanitizer adapters and W11 evidence remain explicitly incomplete; their absence alone is not a finding below. Full native gates belong to the concurrent `.5` effort and are not claimed here. No implementation, plan or tracker changes; no merge, push, public-relay or live-MDK execution.

## Findings

### R1 — P1 / confirmed: reused build can attest a different checkout's commit

**Location:** `scripts/test-plan.py:71-86`; `scripts/test_selection_realization.py:341-354`.

The committed-tree admission check verifies HEAD and cleanliness of `args.repo`, but the reuse branch loads a realization without binding its `repo_root`/`source_dir` to that checked repository. `load_realization()` compares stored identity and generated metadata, not the actual source checkout's current identity. Subsequent audit/build/execution follows the realization's source/build paths.

**Reproduced:** using the existing small native CMake fixture, configure source checkout X at commit A, clone a clean checkout Y at A, then change `shared.h` from `SHARED 42` to `SHARED 99` and commit B in X. Run the real CLI's `realization_command()` path without `--configure`, with `--repo Y`, plan A, and X's existing build directory. It returned exit **0**, `job_conclusion: success`, and receipt SHA **A**, while all three executed tests wrote **`evidence-99`** from **B**. The post-build input audit and Ninja freshness checks passed: they freshly validated B's inputs under A's retained identity. No forged realization or receipt was used.

This breaks the exact-source receipt contract even though the compared JSON identities agree. The current Actions worker always configures a fresh build and the local wrapper materializes a unique checkout; this reproduction does **not** establish a bypass of their current release gates.

**Recommendation / regression tests:** reject a reused realization unless its canonical repository/source paths match the admitted execution checkout and the actual source still satisfies the plan's committed/snapshot identity. Add a two-checkout test exactly as above, plus same-checkout HEAD advancement and matching-checkout reuse controls. The mismatches must fail before build/execution and emit no successful lane receipt. Do not merely rewrite the stored identity or re-audit the wrong source under A.

### R2 — P2 / confirmed: fixture/order input ownership is not audited against consuming checks

**Location:** `scripts/test_selection_realization.py:601-637,697-705`; closure discovery at `:442-464`.

`check_inputs()` audits each registration against its own descriptor owners and checks which directly claim that registration. Support registrations are exempt from primary claim/owner agreement. The audit does not propagate each primary check's coverage requirement through its fixture/ordering closure. Thus a fixture setup script can be correctly owned in isolation yet have no impact path to any check that executes it, without producing a missing-edge gap.

**Reproduced with the checked-in native fixture** (`tests/selection/realization_support.py:48-87`): configure/build all targets, then run the post-build audit. It returns **`passed: true`, `gaps: []`**. `setup.cmake` belongs to `testing`, while all three primary checks belong to `application`, which depends only on `core` and `vectors`. The selector's reverse closure for `testing` contains only `testing` and selects **zero** primary checks. Yet resolving `a-model` executes `reset`, `second-setup`, `cleanup` and ordering prerequisite `z-first`. A setup-only change is therefore omitted while the input-completeness audit certifies the graph. This is a reproduced audit defect, not a claim that a particular production MDK case was observed omitted.

**Recommendation / regression tests:** for every primary check, audit the actual fixture/ordering support inputs and target closure against that check's selection coverage. Report a missing edge (and block selective acceptance) unless changing those inputs reaches the consumer. Extend the existing fixture with setup-only, cleanup-only, chained-fixture and `DEPENDS`-only changes, including multiple consumers with different owners. A logical check can own test-only fixture nodes; no production-library dependency on its own fixtures is required.

## Verification / Other reviewed boundaries

- **PASS:** `python3 -m unittest discover -s tests/selection -v` — **142 tests**.
- **PASS:** `actionlint -shellcheck= -pyflakes= .github/workflows/*.yml` — workflow syntax/expression validation, not hosted execution or shell/Python lint.
- **CONFIRMED locally:** both additional adversarial native-fixture probes above. They used disposable temporary CMake/Git projects, not project-wide native acceptance or live services.
- Direct-source checks found the current Actions aggregator independently accounts for selected worker conclusions, rejects incomplete inventory coverage, and requires broad-family success. The enforcing publication path additionally checks exact SHA/tag, broad/complete coverage, applicable package conclusions and selected same-run artifact IDs. Disabled/shadow publication decisions are explicitly distinguished from validation attestations.
- The new validation entrypoint uses `pull_request`, not an elevated `pull_request_target`/cross-run artifact consumer; validation workflows declare read-only contents permission and package publishing remains separate. These are source observations, not fork-hosted acceptance evidence.
- Legacy triggers/schedules/default gates remain staged; unsupported direct live-peer execution is refused rather than synthesized. No recommendation to waive those failures or to substitute local fixtures for W11 evidence.

## Recommendations / Merge assessment

Fix **R1** and **R2** with the narrow regression coverage above before master merge, then validate those changes against the actual integrated tree. Review the pending exclusion-accounting correction separately; this report cannot approve unseen changes. Keep existing native/keychain/push gates and the rollout hold intact. No additional broad production rewrite or fabricated passing adapter is requested.

**Version assessment:** this review report is documentation-only; **no component bump**. This review neither closes implementation beads nor establishes merge/promotion acceptance.
