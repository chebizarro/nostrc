# Ordinary-profile libgo nonmember policy — review proposal

**Status: implemented; exact ordinary records admitted from reviewed x86 evidence.**
**Date:** 2026-10-19. **Repository baseline:** `6ce261c4234c35d5f6c95822edeab83fdae48b60`.
**Tracking:** `nostrc-0cfsg.9` (profile accounting), `.2` (backend acceptance), `.5` (rollout evidence).

Implementation source: `48af17055f17d88f0392ab716cc93ae4644bd3d5`. See the
[source-bound admission receipt and verification limits](../reviews/dependency-aware-testing-ci-2026-10-18-acceptance-audit.md#approved-ordinary-libgo-subset-admission).
The specification below records the approved scope; it does not claim migration
completion or change rollout/release acceptance.

The owner initially authorized preparation only, then approved proceeding with
this scoped implementation. The document itself changes no executable policy.
The exact records may be admitted only after their source/profile proof obligations
are met and reviewed; no default/enforcement, protection, release-mode or acceptance
cutover is authorized. Routine implementation/evidence review does not require
another owner approval loop.

## Approved implementation scope

Implement an **explicit diagnostic-only subset contract** for exactly the 33 libgo
registrations below in exactly `groundhog.linux` and `grotto.linux`. Admission
requires a separate technical review of fresh, source-bound evidence on each
authored Linux/x86_64 profile. Approval does not authorize defaults/enforcement,
complete/release eligibility, W11 credit, or broader exclusions.

**Recommendation:** implement the bounded subset mechanism only after review,
without generalizing the existing full-partition sanitizer contract. This accounts
for intentionally out-of-scope registrations honestly; it does not make either
ordinary profile pass. If rejected, retain the current blocking findings unchanged.

## Evidence and the actual problem

- Groundhog's ordinary job runs `^(groundhog-|nostrc-test-bus-selftest$)` and
  excludes `^groundhog-mdk011-`: `.github/workflows/groundhog-ci.yml:272–276`.
- Grotto's native job runs `^(signer/|nip55l_|nostr_signer_webext_host_)`:
  `.github/workflows/grotto-ci.yml:171,194–202`. Its sanitizer-specific filters
  are separate and are not part of this proposal.
- Fresh Linux/arm64 configure/export diagnostics found all 33 names below in
  **both** ordinary build trees. None matches either ordinary job's test filter.
  Building those executables is not evidence of running them.
- Existing libgo checks already have precise ownership. Enrolling them in these
  ordinary profiles would silently expand the source workflow's execution scope.
  Treating them as passed or dropping their input audits would be worse.
- Current code admits only `groundhog.linux.sanitizers`:
  `scripts/test_selection.py:45–102`. Its `_nonmember_partition` requires the
  nonmember list to equal **every registration outside the required primary/fixture
  closure**, not an arbitrary subset:
  `scripts/test_selection_realization.py:898–907`. Simply adding a profile ID or
  putting these 33 names in that schema is not a valid implementation.
- Fresh post-store-key diagnostics retain 406 unclaimed registrations in
  Groundhog and 400 in Grotto, with thousands of other semantic/runtime findings.
  This proposal addresses only 33 unclaimed findings per profile; the other
  findings remain failures. See the [acceptance audit](../reviews/dependency-aware-testing-ci-2026-10-18-acceptance-audit.md#ordinary-groundhog-store-key-membership-repair).

### Evidence identities and limits

The tracked witness is
`tests/selection/fixtures/store_key_membership_exports.json`, SHA-256
`f1c285da7bec291a13e650b0c9a809998a1a60309367eb018a53ee655a20134e`.
Its configure source is `fb7335cb0e9546463293b6adf2044fe54178f5ac`; all eight
recorded source-file hashes still match this proposal's `6ce261c42` baseline.
The exports are **aarch64 diagnostics**, not authored x86_64 admission.

| Profile | Authored profile SHA-256 | Diagnostic registrations | Diagnostic realization export SHA-256 |
|---|---|---:|---|
| `groundhog.linux` | `f024669154f7c94561f96174c2b0a35ae2713cb138d1efeabbc64e3497e0b63e` | 422 | `c7c0d67c114da3449f09ec8bd560229179abb330faca03bcf4fbec0f8f597885` |
| `grotto.linux` | `5613add713ace7afb946d6d783facff8fecf3a6291f6b67d750585deac1f22f1` | 459 | `a795f9ffb58c80956b714d7f0379b28cdc5fef75c46ba00079a629e1ef1d598d` |

Both libgo closure exports have SHA-256
`1a7838c876523b157f98fb3bdfb5a32729a074a982ecc7f1564d447a1458e55e`.
The immutable diagnostic checkpoint is
`/tmp/nostrc-registration-membership-20261019/checkpoint-61d7ecb2a.json`, SHA-256
`19359b878854ecaa1093615959bef9d84fde85ea007bce2548f0417728ba0f3c`.
That local path is evidence provenance, not promised permanent raw retention;
the tracked fixture preserves the names, input closure summaries and source hashes.
No new hosted executions or acceptance-ledger refresh were performed for this
proposal. The [versioned ledger](../reviews/dependency-aware-testing-ci-2026-10-18-run-ledger.md)
remains a dated snapshot, not a live counter.

## Exact candidate scope

The following identical, sorted 33-name list applies **independently** to the two
named ordinary profiles. It is not a regex, a node-wide rule, or permission for any
other profile, architecture, sanitizer, prepared route, fixture or future test.
SHA-256 of the compact JSON array of these names, in this order, is
`0da7c61982c88b736fb04dd1ba31fad46d33d99d28bacd39005e793b995747c5`.

| # | Exact CTest registration |
|---:|---|
| 1 | `GoChannelCloseTest` |
| 2 | `GoChannelMpmcSlotTest` |
| 3 | `GoChannelStressTest` |
| 4 | `GoChannelTest` |
| 5 | `GoContextCancelTest` |
| 6 | `GoContextStressTest` |
| 7 | `GoContextTimeoutTest` |
| 8 | `GoErrorTest` |
| 9 | `GoFiberBasicTest` |
| 10 | `GoFiberBasicTestW4` |
| 11 | `GoFiberChanExtTest` |
| 12 | `GoFiberChanExtTestW4` |
| 13 | `GoFiberChanTest` |
| 14 | `GoFiberChanTestW4` |
| 15 | `GoFiberContextTest` |
| 16 | `GoFiberIoDeadlockTest` |
| 17 | `GoFiberIoDeadlockTestW4` |
| 18 | `GoFiberIoTest` |
| 19 | `GoFiberIoTimeoutTest` |
| 20 | `GoFiberIoTimeoutTestW4` |
| 21 | `GoFiberStarvationTest` |
| 22 | `GoFiberStarvationTestW4` |
| 23 | `GoHashMapTest2` |
| 24 | `GoIntArrayTest` |
| 25 | `GoLongAdderStressTest` |
| 26 | `GoRefPtrTest` |
| 27 | `GoSelectCancelTest` |
| 28 | `GoSelectClosedTest` |
| 29 | `GoSelectMultiTest` |
| 30 | `GoSelectTest` |
| 31 | `GoStringArrayTest` |
| 32 | `GoTickerTest` |
| 33 | `GoWaitGroupTest` |

## Proposed contract and fail-closed behavior

### 1. Separate subset authority; preserve the current contract

Proposed inventory namespace: `diagnostic_nonmember_subsets`, with a version-1
record for each explicitly supported profile. Proposed receipt authority:
`reviewed_diagnostic_nonmember_subset_v1`. These are **design names, not existing
APIs**. Do not reuse the sanitizer-only `NONMEMBER_ID` or reinterpret its version-1
full partition as a subset. Reject unknown profiles, schema versions, fields,
duplicates, wildcards, empty lists and automatic profile aliases. No new CLI flags
are needed: execution remains in the existing explicit profile-restricted
diagnostic route (`scripts/test-gate.sh --profile ...`), never implicit local or
required-CI defaults. `--explain` remains plan-only.

Each proposed record must bind:

- The exact profile ID/hash, unchanged source-workflow hash/filter, sorted candidate
  names and explicit reviewer/approval-document/tracking identity.
- Reviewed source SHA and full diagnostic/postbuild-audit evidence hashes, including
  failed findings; provenance is not a success attestation.
- The sorted full registration-name universe/hash and full authored primary/fixture
  closure. A new, removed or renamed registration anywhere in the reviewed universe
  requires re-review; do not quietly absorb it into this subset or the remainder.
- A normalized contract/hash for the 33 candidates: original commands, properties,
  owners, target mappings and fixture dependencies, with source/build roots
  normalized explicitly. Paths must not be erased in a way that hides custody
  differences. Changed candidate semantics or newly required fixtures refuse.

All values require fresh authored-profile evidence before activation; this document
supplies candidate provenance, not populated approval records. The existing
sanitizer policy and its refusal behavior must remain byte-for-byte semantically
compatible, including rejection of unapproved ordinary-profile records today.

### 2. Classify only the reviewed subset, not the whole remainder

For the fresh registration set, recompute the entire authored primary/fixture
closure using the existing fixture-closure semantics. Verify it against the
reviewed closure and preserve existing explicit fixture accounting.

Every candidate must exist exactly once, retain precise `libgo` ownership and
resolvable real target/input closure, fall outside its source workflow filter,
and be disjoint from authored primary registrations, required fixtures and the
current selected execution closure. A collision, missing target, missing input,
changed filter or fixture dependency refuses **before lane launch**. Never resolve
a collision by silently omitting a selected test or required setup/cleanup.

Only the matching `unclaimed_registration` finding for an admitted candidate may
be reclassified as `not_executed` with this authority. Every remaining unclaimed,
owner-disagreement, source/header/semantic/runtime/target finding remains visible
and blocking. Never replace full audits with a selected-only audit or mark the
entire unaccounted remainder reviewed. Before/after comparisons must use the same
fresh graph and report the exact finding set difference, not just totals.

If only these 33 findings per graph change, the prior diagnostic unclaimed counts
would arithmetically become 373 Groundhog / 367 Grotto. **These are proposed
expectations, not measured implementation results.** Neither resulting audit
would pass; the current gate still refuses on its other preaudit findings.

### 3. Keep build/input custody and execution accounting intact

Resolve and retain the full transitive target closure of all 33 nonmembers. Keep
those targets built wherever the audited contract requires them, alongside required
selected and fixture targets. Do not infer build coverage from configure metadata,
a prior artifact, a subset build or a target's name. Continue full source/Ninja/
header/runtime audits, with prebuild and postbuild evidence and original failures.
Unsupported target mapping or backend custody stays an explicit refusal.

Reuse the existing fresh binding requirements from
`scripts/test_selection_realization.py:884–895`: current source/inventory/profile,
profile fingerprint, phase/configuration/environment context, registration export,
descriptors, generation/cache/Ninja bytes and toolchain. Independently compare
profile, resolved and phase configuration; no architecture substitution, stale
build reuse or byte/hash disagreements. Policy approval provenance and runtime
freshness bindings serve different purposes and must both be present.

Do not execute the 33 tests merely to manufacture coverage for this policy, and do
not suppress their execution in another legitimate standalone/libgo or broad lane.
Standalone/native/sanitizer/stress results are separate source/profile-specific
receipts. A failure in another lane remains a failure; this policy does not cancel,
quarantine, retry, suppress leaks or alter its thresholds.

### 4. Incomplete is transitive and cannot be laundered

All admitted omission records and wrappers retain the exact names and
`status=not_executed`; never report pass, skip-as-pass, successful stress coverage
or a complete profile. Any admitted subset forces `complete=false` and
`complete_gate_eligible=false`, including otherwise successful selected diagnostics.
A failed preaudit still produces failure, not successful diagnostic eligibility.

Update receipt normalization, aggregation and release admission to recognize the
new authority at every nesting level. They must refuse complete/release promotion
even if a caller strips outer flags, omits the authority's outer wrapper, relabels
an omitted registration passed, or supplies success in a different profile.
Missing required records, stale provenance or unknown authority versions refuse.
A broad integration's separately complete evidence cannot be copied onto these
incomplete ordinary receipts. Required-gate and release validation remain governed
by their existing independent acceptance contracts.

## Implementation and verification, only if separately approved

| Work item | Goal / done when | Key files | Dependencies / size |
|---|---|---|---|
| A — schema and admission | Two explicit profile subset records, exact names and fresh bindings validate; legacy full-partition behavior unchanged. No records activated from ARM64 provenance alone. | `scripts/test_selection.py`, `scripts/test_selection_realization.py`, `tests/selection/inventory.json` | Approved design; medium |
| B — audit/build/receipt integration | Only the 33 matching unclaimed findings can change; full target/input custody and other blockers remain; every nested complete/release promotion refuses. | Same backend/aggregator modules; `scripts/test-gate.sh` only if accounting propagation needs it | A; medium |
| C — source-bound proofs and documentation | Positive and adversarial regressions, authored x86 profile witnesses, exact audit set differences and operator-reviewed activation records retained. `.9`/`.2` remain open for wider acceptance; `.5` refreshed before W11/closure claims. | Existing diagnostic-nonmember/eligibility/ownership/workflow tests; proposed subset-specific tests/fixtures; `AGENTS.md`, `docs/TESTING.md`, audit/ledger | A+B; medium |

Any new test/fixture files must also be explicit `selection.contract` inputs.
No shipped component bump is expected for internal CI metadata/tests; assess again
if implementation changes a supported public contract. Do not increment versions
for this review-only document.

Required proof matrix:

1. Exact positive sets: 33 names in each of the two ordinary profiles; distinct
   per-profile records even though the lists currently coincide. No admission in
   core, sanitizer, latest/TSan, prepared, arbitrary native or unknown profiles.
2. Fresh authored Linux/x86_64 configure/export and target/input/fixture audit for
   each profile. ARM64 runs remain diagnostics. Reconcile any graph difference
   through review, never an automatic architecture/profile alias.
3. Mutate name, count, duplicate, profile/workflow hash, universe, selected collision,
   fixture closure, command/property/owner, mapping, source, inventory, generation,
   toolchain, configuration, phase or retained bytes: refuse before execution.
4. Compare complete raw before/after audits on the same fresh graph. Remove only
   the exact 33 admitted unclaimed findings per profile; all other findings and
   incomplete/failure outcomes survive. Do not assume an all-green audit.
5. Tamper source/Ninja/header/runtime inputs, omit a required build target, or reuse
   stale build/audit data: fail; no selected-only fallback or fabricated build proof.
6. Groundhog-only/Grotto-only edits retain savings and do not enroll these libgo
   tests. Libgo edits still select their legitimate libgo coverage; existing true
   downstream and MDK dependencies are not hidden to satisfy a savings assertion.
7. Direct, nested, stripped-flag, malformed/omitted-record and cross-profile receipt
   promotion attempts all fail complete/release admission. No omitted execution
   becomes a pass through aggregation or retry handling.
8. Existing Groundhog sanitizer full-partition positive/negative tests remain
   unchanged in meaning. New subset mode cannot admit that profile or relax its
   exact reviewed complement/fixture closure.
9. Run the complete selection suite, six shell gate suites, four statics, actionlint
   and required builds. For the observed macOS launcher context, the reproducible
   suite command is `/usr/bin/env XPC_SERVICE_NAME=0 PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/selection -v`.
   Preserve original failures and setup attempts; a nonroot Linux suite must not
   be relabeled native proof. The literal shell-assignment launch has a retained
   exit-133 failure and is not the tested command.
10. Update the acceptance audit/ledger and `.5` with exact SHA/profile/artifact
    receipts before claiming W11 progress or closing acceptance. Unit counts,
    diagnostic success and reclassified findings do not satisfy PR/broad quotas.

## Alternatives, risks and approval boundaries

- **Run all libgo tests in both ordinary lanes:** rejected for this proposal;
  silently expands workflow scope and defeats the requested dependency savings.
  An execution-scope change would require its own reasoned approval.
- **Treat these 33 as the existing complete nonmember partition:** invalid; hundreds
  of other registrations remain. It would either refuse correctly or require
  weakening the current invariant. The distinct subset authority avoids that.
- **Approve every registration outside the workflow regex:** rejected; hides
  unsupported ownership/runtime/fixture cases and auto-approves future tests.
- **Reconfigure each build to contain only its test subset:** potentially useful
  longer-term, but changes legacy compile/feature coverage and requires a separate
  build-contract design; not a shortcut authorized here.
- **Keep the current restriction:** safe default if this proposal is rejected.
  All findings and blockers remain; continue other independently supportable repairs.

The benefit is honest accounting and a bounded reduction of false scope alarms,
not an immediate green gate. The costs are another explicitly versioned authority,
reviewed evidence maintenance and re-review on graph/filter/contract drift. Avoid
schema/framework generalization beyond these two profiles and this exact family.
Further nonmember families need separate evidence and owner approval.

**Delivery stages:** scoped implementation approval (received) → reviewed
implementation and fresh per-profile authored evidence → admission of only the
proven exact diagnostic records. None bypasses remaining audits, W11/operator
rollout approval, or release acceptance. Missing proof blocks admission, not
implementation of the fully tested mechanism; broader scope changes still require
the owner. The two exact records have now been admitted through the reviewed evidence and
implementation, not merely by updating this document. Runtime freshness checks,
remaining audit failures and incomplete/release restrictions still apply.
