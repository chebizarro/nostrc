# Dependency-aware testing: acceptance reconciliation

**Audit date:** 2026-10-18. **Audited implementation:**
`2743f17354ac13527d8d8919f81abc711396c959`. This is an acceptance/evidence audit,
not another implementation wave or authority to change rollout policy.

The [run ledger](dependency-aware-testing-ci-2026-10-18-run-ledger.md) is the
canonical versioned run-count snapshot. Beads remains the task tracker. Earlier
[staged receipts](dependency-aware-testing-ci-2026-10-15-staged-acceptance.md)
retain their original sources, failures and limits; this audit does not relabel
old passes or discard old failures.

## Result and scope

| Bead | Reconciliation decision | Why |
|---|---|---|
| `nostrc-0cfsg.1` | **CLOSED — finite logical contract accepted** | Its stated logical-selector, Git-range, aggregation, explicit ownership/gap and real-adapter contract is proven below. W11 is not an extra prerequisite for this finite scope. No coverage/enforcement readiness is claimed. |
| `nostrc-0cfsg.2` | **KEEP OPEN — missing root acceptance** | The engine's behavioral regressions and actual standalone execution are proven; a clean current root profile's end-to-end configure/audit/build/selected-test receipt is not. Current hosted root configuration/input audits fail visibly. |
| `nostrc-0cfsg.3` | **KEEP OPEN — implementation/lifecycle incomplete** | Shell/adapter regressions pass, but actual safe native custody and original MDK lifecycle remain incomplete. Prepared-container diagnostics do not fulfill that scope. |
| `nostrc-0cfsg.4` | **KEEP OPEN — hosted/release acceptance incomplete** | Workflow contracts and actual broad execution exist, but selected-worker/full-union failures remain and no new exact-SHA release-validation execution is established. |
| `nostrc-0cfsg.5` | **KEEP OPEN — W1/W11/W12 owner** | This pass completes a source-linked status reconciliation and ledger backfill, not the representative measurements, accepted comparisons, successful broad integrations or approved cutover assigned in its description. |

Decisions use the existing descriptions/acceptance criteria, not a newly invented
universal requirement. A proven logical-selector criterion does not wait for
20 PRs; conversely, a passing miniature cannot prove a production root profile.
No scope is removed, silently transferred or closed merely because a follow-up
bead exists. Unresolved work remains explicitly tracked in the open scopes.

Status vocabulary: **PROVEN**, **MISSING EVIDENCE**, **IMPLEMENTATION INCOMPLETE**.
An observed negative that correctly refuses is evidence for a guard, not positive
coverage for the refused profile.

## Evidence collected in this audit

- From a clean isolated worktree at exact `2743f1735`, actual
  `PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/selection -v`:
  **437/437 passed, zero skips, 151.176s, exit 0**. This includes real temporary
  Git repositories and CMake/build/CTest mini-projects, not 437 hosted runs.
- All six existing shell suites (`test-pre-push.sh`, `test-full-gate.sh`,
  `test-linux-gate-lifecycle.sh`, `test-linux-gate-smoke.sh`,
  `test-sanitizer-gate.sh`, `test-test-gate.sh`), four mandatory static checkers,
  and actionlint with ShellCheck passed against that same clean source.
- Complete API-visible histories for the four new workflow IDs: **6 shadow
  attempts**, no standalone broad/worker/release runs. Jobs and artifacts were
  fetched with pagination for every candidate attempt. All attempts returned by
  the history have attempt number 1; deleted/inaccessible history is not inferred.
- Repository-wide API context since 2026-10-15: **146 root runs**, comprising
  **133 pushes, 9 scheduled and 4 manual runs; zero PR events**. Pagination totals
  match returned records. This window starts before the new workflows' creation
  at 2026-10-16 01:11 UTC.
- **59 small plan/receipt/aggregate artifact archives** downloaded; all published
  archive digests match, ZIP CRCs pass and member hashes are retained. Every plan,
  aggregate and normalized lane with a source identity matches its parent run.
  Installer binaries were not downloaded/executed.

Raw capture directory: `/tmp/nostrc-acceptance-audit-20261018/`. The committed
ledger retains curated identities/verdicts/hashes and source links, not raw log
or API dumps. Local raw files and GitHub artifacts are not permanent storage;
future acceptance must not assume expired/unavailable bytes still exist.

## `.1` — selector policy, Git ranges and result aggregation

**Existing acceptance criteria:**

> Schema/range/golden/aggregation tests pass including rename/delete/submodule/shallow/newline paths, selected skips/cancellation/stale identities and release applicability; supported-root ownership is explicit, unsupported gaps visible; no success stubs; integration resolve/run delegated to real sibling module. Version: no bump for internal tooling; assess any public build change separately.

| Criterion | Status | Concrete evidence and boundary |
|---|---|---|
| Schema, deterministic impact/goldens, unknown-path broadening | **PROVEN** | `tests/selection/test_selector.py:8–138`: consumer changes do not select prerequisite tests; shared changes select consumers; malformed/cyclic/contradictory policy rejects; current inventory goldens and dirty identity/determinism are checked. All 20 cases pass. |
| Rename/delete/gitlink/shallow/newline and event ranges | **PROVEN** | `tests/selection/test_git_ranges.py:7–154`: 20 actual-Git cases cover both rename paths, base policy for deletions, gitlinks, a real shallow clone, Unicode/newline paths, force pushes, PR tested-merge identity, merge groups and multi-ref unions. |
| Missing/skipped/cancelled/stale result accounting | **PROVEN** | `tests/selection/test_aggregation.py:15–124`: 16 cases reject missing/duplicate/foreign lanes, bad source/plan/profile identities, selected nonpass outcomes, missing independent worker conclusions and hidden sanitizer failures; advisory/quarantine/retry distinctions remain visible. |
| Release applicability, not an actual release claim | **PROVEN** | `tests/selection/test_release_plan.py:26–105`: 12 real-Git/unit cases cover annotated/unknown/global/component applicability, source/version disagreement, dry runs and required package accounting. Current manifest verifies 17 independent components. The existing aggregate identity-negative method is confounded by an empty package map; independent guards are instead verified by the targeted passing-baseline probe below, not overclaimed from that method. |
| Supported-root path ownership explicit; unsupported gaps visible | **PROVEN** | `test_inventory_covers_all_tracked_paths_or_explicit_broadening` (`test_selector.py:124`) classifies every tracked path; current inventory is explicitly `enforcement_ready=false` with coverage gaps. This is path-policy correctness, not a claim of zero production semantic-input gaps. |
| Real sibling integration, no successful execution stub | **PROVEN** | `tests/selection/test_realization_run.py:99–134` executes the actual CMake/CTest backend and normalizes its actual receipt through checked-in `scripts/test-plan.py`. Checkout-binding/CLI modules add 11 passing cases. Production standalone evidence is separately attributed below. |
| Version decision | **PROVEN** | Internal logical testing tooling; no tracked shipped component change in this audit. No component bump. |

These logical modules account for 79 of the 437 passing cases; production
inventory tests and actual realization integration are additional, not silently
included in the 79. Closure is finite logical implementation acceptance, **not**
CI enforcement, broad completeness, all-profile population or W11 approval.

**Independent acceptance review:** reviewed the supplied criterion audit, complete
ledger and source tests; found no blocking issue with finite `.1` closure or the
0/20 / 0/2 accounting. It did not independently execute GitHub queries. One
specific evidence overclaim was corrected: the existing
`test_release_validation_exact_tag_sha_broad_identity` passes an empty package
map, so its negative assertions can reject for missing packages alone. No source
fix is claimed here. A targeted local probe at exact `2743f1735` first passed with every
required package marked successful, then independently changed `tested_sha`,
`tag`, `mode`, `inventory_hash`, `passed` and `partial_profiles`; all six refused
publication for mismatched broad exact-tag validation. This is one synthetic
contract baseline plus six adversaries, not a real publication, PR comparison or
broad integration. The structured probe/result hash is retained in the ledger;
strengthening the permanent regression is tracked under `.4`. No acceptance
scope or rollout requirement was removed.

## `.2` — CMake/CTest realization, runtime ownership and exact execution

**Existing acceptance criteria:**

> Fresh/stale/failed configure, root/standalone export, fixture/reset evidence, model/GUI split, unexpected skip, dirty input cache and missing edge tests pass; real focused configure/build/test smoke runs documented. Zero silently unowned supported registrations, gaps explicit. No production-target refactor or new platform/harness. No version bump unless actual shipped contracts change.

| Criterion | Status | Concrete evidence and remaining gap |
|---|---|---|
| Fresh/stale/failed configure and exact-source freshness | **PROVEN** | `tests/selection/test_realization.py:6–52`, source-identity/toolchain/compatibility modules: real miniature generates, stale/missing replies refuse, failed/external configure invalidates reuse and dirty identities change fingerprints. |
| Fixtures/reset, model vs GUI, single exact invocation, unexpected skip | **PROVEN** | `test_realization.py:54–94` and `test_realization_run.py:15–77`: primary/support closure, one indexed CTest invocation, reset-surviving attempt evidence, GUI skip as failure and absent postbuild audit refusal. |
| Missing source/runtime/header edges are detected rather than ignored | **PROVEN** | `tests/selection/test_realization_inputs.py:5–66`: actual compiled header discovery, omitted-test missing edges, missing/deleted runtime files, empty runtime directories, dirty identity and external symlinks. A failed real audit remains failed. |
| Real standalone configure/build/audit/execute smoke | **PROVEN, bounded** | Current hosted run 39132650260 retained macOS standalone libgo **32 executions** and Linux standalone **33 executions**, registration/input audits passed. Artifacts 12138776972 and 12138414686. Their `complete_gate_eligible=false` is preserved; excluded stress coverage is not invented. |
| Real root end-to-end selector smoke and complete supported runtime ownership | **MISSING EVIDENCE / IMPLEMENTATION INCOMPLETE** | Latest current-source root lanes fail configure or preaudit, detailed below. Ordinary full-root builds/439 native tests from 6299 are valuable separate evidence, not an admitted selector/audit receipt. Root/standalone finalize text assertions at `test_realization.py:97–104` explicitly do not simulate full native exports. |
| Boundaries/version | **PROVEN** | No new platform/harness or shipped target refactor in this audit. Existing resource fixes keep their prior component version assessment; this audit is documentation/evidence only. |

**Next closure evidence:** repair actual authored root configuration and audited
membership/semantic/runtime mappings under existing `.31` / `.9`; obtain an
honest fresh root configure → resolve/audit → build → full postbuild audit →
selected CTest receipt. Unrelated W11 sample counts are not a new prerequisite
for `.2`; current root failures are the concrete missing implementation evidence.

## `.3` — plan-driven local gates and sanitizer parity

**Existing acceptance criteria:**

> All existing and new shell regression cases pass; real focused integration uses shared logical plan/resolve/run; unknown changes broaden; selected missing capability fails rather than skipping; cross-checkout driver lock and cancellation tests pass; sanitizer config parity verified. No component bump for internal gates.

| Criterion | Status | Evidence / blocker |
|---|---|---|
| Shell regressions, unknown broadening and capability refusal | **PROVEN** | All six shell suites rerun successfully; 437 cases include local command, sanitizer-route, prepared-profile and cancellation regressions. `scripts/test-gate.sh:243–250` refuses unsupported capabilities and wrapped non-CTest checks before launch rather than fabricating execution. |
| Ordinary shared-plan execution and separate sanitizer phase contracts | **PROVEN, bounded** | Actual mini-project/command/phase tests execute the shared APIs. Existing explicit shared sanitizer route preserves authored jobs/configuration/environment/input agreement. This does not establish all production profiles. |
| Safe native/wrapped custody and original MDK cross-checkout image/run lifecycle | **IMPLEMENTATION INCOMPLETE** | `.23` / `.8` remain open. `scripts/mdk-gate.py:327–349` deliberately returns nonzero `cleanup_unverified` after otherwise successful commands and always reports `cleanup_complete=false`. Prepared locked routes and 13 historical contained custody cases do not prove the original image-build/arbitrary native-tree path. |
| Real full shared sanitizer profile parity | **MISSING EVIDENCE** | Current hosted Groundhog sanitizer selector refuses its stale exact registration partition. Narrow nonmember admission cannot erase semantic/runtime/input gaps or authorize a different architecture. |

**Next closure evidence:** finish the specifically assigned custody/lifecycle
work and production sanitizer parity; exercise two checkouts, partial/cancelled
image builds, escaped stubborn children and sibling survival with retained raw
status/cleanup evidence. No sleep-based cleanup, PID-name matching, weaker
assertions, successful lifecycle stub or selected-only audit fallback.

## `.4` — reusable CI, broad tiers and exact-SHA release gates

**Existing acceptance criteria:**

> Workflow lint/static and event/result routing tests pass including merge_group, selected skipped/cancelled, validation-only/unknown/annotated/legacy tags and branch/tag dry runs. Existing publishers cannot publish partial validation; no new publishing prefixes or platforms. Broad replacement is executable; retained temporary duplicate triggers are explicit. No version bump for CI-only changes.

| Criterion | Status | Evidence / blocker |
|---|---|---|
| Lint, event/result/tag routing and nonpublishing branch/tag contract tests | **PROVEN, contract level** | Current 36 workflow contract cases and 12 release-plan cases pass; actual actionlint/ShellCheck passes. These are not actual protected-merge or tagged publication executions. |
| Broad replacement launches real legacy workloads | **PROVEN, execution only** | Five nested broad calls reached actual family accounting. Latest run 39132650260 has 47 nested broad jobs: 44 success, 2 failures (JSON fuzz and family accounting), and 1 skipped fuzz coverage-report job. Success of package/interop/BP subjobs is not full broad acceptance. |
| Complete same-source selected CI/family acceptance | **IMPLEMENTATION INCOMPLETE** | Latest selected root/Grotto/Groundhog/MDK workers remain failed for specific configuration, input and custody reasons. Aggregate correctly has `passed=false` and `complete_gate_eligible=false`. |
| Exact-SHA/all-applicable publisher validation is actually established | **MISSING EVIDENCE** | No new release-validator standalone record; reusable calls cannot be inferred absent merely from that fact. No qualifying exact-tag broad validation/publisher gate execution was found in the audited shadow/repository window. Windows branch build successes are not publication-validation attestations. Repository release-mode variable lookup returns 404; checked-in fallback remains disabled. Enforced release routing must be exercised nonpublishing before approval. |
| Temporary duplication/platform boundaries/version | **PROVEN** | Legacy triggers/schedules and publisher/platform prefixes remain; no protection or mode changes. CI-only work has no component bump. |

**Next closure evidence:** repair selected worker coverage, resolve broad JSON
fuzz failure under `.29`, strengthen the permanent identity-negative test to use
an all-success package baseline and independently mutated validation fields,
and demonstrate the release validation/publisher contract at
exact source with every applicable artifact result, and retain explicit advisory,
validation-only and fork/merge-group behavior. Do not enable `enforce`, remove
legacy gates or create/publish a release as a side effect of this audit.

## `.5` — operational baseline, progress and integration acceptance

**Existing acceptance criteria:**

> Source-grounded baseline and exact commands/receipts recorded; current plan/progress accurate and AGENTS/testing instructions consistent with actual staged behavior. Integrated quality gates and review findings recorded; external rollout prerequisites remain open tracked work, never fake completion. Internal tests/docs no component bump.

| Criterion / assigned phase | Status | Evidence / blocker |
|---|---|---|
| Source-linked implementation status, commands and retained failures | **PROVEN for this checkpoint** | This criterion matrix, actual current-source local gates, source-bound hosted artifacts and ledger preserve distinct native/diagnostic/hosted results. Older dated receipts stay historical. |
| Actual repository protection/status baseline | **PROVEN, read-only snapshot** | Master reports `protected=false`; classic protection API explicitly says `Branch not protected`; effective branch rules and repository/inherited rulesets return empty arrays. Token has repository admin permission. No settings changed. This is not approval to add/remove protection; merge-queue feature availability was not independently queried, and no effective merge_queue rule was returned. |
| Current local/CI/release policy documented consistently | **PROVEN for staged policy** | Defaults stay legacy, publisher source fallback disabled, incomplete/negative receipts ineligible. Ledger links and refresh obligation are added to operational docs/AGENTS in this pass. |
| W1 representative cost baseline and W11 comparison/performance evidence assigned in description/plan | **MISSING EVIDENCE** | No complete seven-condition, five-sample baseline. Actual accepted counters now verified **0/20 PR comparisons and 0/2 broad integrations**, not inferred from unit counts. Repo window has no PR events at all. |
| W12/approval and external prerequisites accurately tracked | **PROVEN as status accounting, not completion** | W11 NOT MET / W12 NOT RUN. `.5` remains the owner of this ledger and assigned W1/W11/W12 work; recording those phases as incomplete does not complete them. |

The plan's original acceptance still requires five samples per representative
condition, median/range/IQR, 30% Grotto-only and 25% isolated non-MLS Groundhog
median runner-minute savings, zero unrelated live MDK and its 10% regression
limits. It requires 20 representative PR comparisons and two successful complete
broad integrations, reviewed omission accounting, release/protection checks and
operator approval before cutover. This audit changes none of those requirements.

The repository currently works through pushes/manual/scheduled events, so the
PR counter cannot advance by counting those events as PRs. Deliberately exercise
representative PR comparisons, or obtain an explicit amendment permitting a
specified equivalent historical-range replay protocol. Do not invent 20 dummy
PRs or silently relabel pushes to satisfy the quota.

## Concrete current hosted blockers, not generic pending runs

Source: [run 39132650260](https://github.com/chebizarro/nostrc/actions/runs/39132650260)
at `2743f1735`. Lane receipts and archive/member hashes are in the ledger.
These are prebuild emitted findings, **not counts of distinct root-cause bugs**.

| Profile / job | Retained failure | Follow-up owner |
|---|---|---|
| `core.linux`, `groundhog.mdk08` | Configure rejects authored Debug versus realized empty configuration. Current profile configure arguments omit `-DCMAKE_BUILD_TYPE=Debug`; do not weaken the independent agreement check. | `.31` / `.9` authoring |
| `core.linux.asan`, `core.macos.asan` | Each preaudit reports 3 unclaimed registrations: `MockRelayUnit`, `concurrency_subscription_shutdown_asan`, `test_json_envelope`. | `.9`; existing exclusion/admission policy remains binding |
| `groundhog.linux` | 408 unclaimed registrations, 9,473 missing-impact findings, 1 owner disagreement and 1 target mapping finding. | `.9` / `.31` |
| `grotto.linux` | 402 unclaimed registrations, 9,104 missing-impact findings and 2 generated-runtime-without-target findings. | `.9` / `.31` |
| `grotto.linux.asan_ubsan` | 322 unclaimed registrations, 330 missing-impact findings, 1 target mapping and 2 generated-runtime-without-target findings. | `.9` / `.31` |
| `groundhog.linux.sanitizers` | Resolve refuses `diagnostic nonmember fresh registration partition differs`. | `.9` / `.8`; only exact authorized policy may be reconciled |
| `groundhog.mdk011` | Capability refusal: live peer/image lifecycle requires existing guarded adapter. | `.8` / `.23` |
| Broad JSON fuzz | Required job 117691683008 reports LeakSanitizer 514 bytes / 15 allocations through `json_parser_load_from_data` at `fuzz_json_event.c:55`, exit 77. Retained input is `leak-7396472d70f69483739531904dd1db7271706023`. Fuzz coverage-report is skipped; family accounting fails. No skip is counted as success. | `.29`; current log observed, no new fix or upstream attribution claimed |

Approving more nonmembers alone cannot erase missing semantic/runtime/target
findings. Production fixes must preserve full audits and scope/architecture
binding. Root build/type, membership and custody failures are not remedied by
another unmodified 437-case rerun.

## Keeping the count from now on

The ledger stores run/attempt/source identities, actual nested accounting,
normalized lane findings, artifact hashes and explicit dispositions. Its displayed
counters are checked against the structured data. A refresh is required before
reporting W11 progress or proposing bead closure, not every time any CI job ends.
That avoids a documentation-commit/CI/ledger-commit feedback loop.

A future session owns fetching and reviewing new candidates under `.5`; the
repository owner is not expected to keep a manual tally. The ledger is not a CI
auto-updater or proof that future/expired artifacts were reviewed. No counter
advances from a green subset, retry, diagnostic/native test run or merely
`complete=true` in an unsuccessful aggregate's requested-mode field.

## Follow-up repairs after the frozen ledger snapshot

The following local repairs are separate from the historical hosted results above;
they do not revise or erase those failures or advance W11 counters.

- `24d361f19607ea8de9a703054c89390bf1ce7ce8` adds
  `-DCMAKE_BUILD_TYPE=Debug` to `core.linux` and `groundhog.mdk08` in
  `tests/selection/inventory.json`. `AuthoredDebugConfigureContracts` in
  `tests/selection/test_profile_contracts.py` executes miniature real CMake
  configurations, verifies Debug cache/realization/phase agreement, and proves
  missing or contradictory Release arguments still refuse without a realization
  receipt. Independent configuration guards, capabilities and membership are
  unchanged. A separate macOS/arm64 minimal cache/codemodel probe passed for both
  argument lists; this is not production Linux, full-profile or hosted proof.
- `3946ff8d4` integrates the permanent `.4` regression correction in
  `test_release_validation_exact_tag_sha_broad_identity`: a passing publication
  baseline with all required package results precedes six independent validation
  mutations, each rejected with the exact identity/eligibility error. Existing
  missing/failed-package tests remain. This replaces the earlier confounded test
  evidence, not the historical audit-only probe or actual release acceptance.
- At integrated code SHA `3946ff8d49a4d31c89bf5d35e6646b54a5a2eab1`,
  `PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/selection -v`
  passes **439/439, zero skips, 147.860 seconds**. Log SHA-256:
  `9f06926b96fb675904f8b8bbfe9daf9e00d8e108d3b856da8f002023bc61dfe7`.
  All six existing gate shell suites, four static checks and actionlint pass.
  After initializing pinned submodules in the clean worktree, native macOS/arm64
  root configure and default-all build pass with
  `-DNIP55L_SECRET_BACKEND=libsecret` and
  `-DGROUNDHOG_STORE_KEY_DEFAULT_BACKEND=secret-service`, build parallelism 4.
  Groundhog remains default-off; no full-root CTest execution, production Linux
  selector admission or hosted success is inferred. The minimal probe JSON hash
  is `f055bc89fb31b38186f07a06ad6a03ae9fe4288449092a981bcb2ea749004eb4`.
  Logs are local diagnostic retention, not durable hosted artifact acceptance.
- The independently reviewed three-file change has no identified blocker. `.31`,
  `.4` and their parent acceptance beads remain open. A new source-bound hosted
  run and its artifacts must be reviewed before claiming the configure blocker
  resolved in production. Membership, runtime/target mappings, sanitizer parity,
  MDK lifecycle, exact-tag publication validation and rollout prerequisites remain.

### Bounded hosted follow-up: configuration repair

At `2026-10-19T03:40:29Z`, run
[39194781902](https://github.com/chebizarro/nostrc/actions/runs/39194781902)
was still active. Two completed attempt-1 lane receipts bind committed source
`2c5b18c23344b3a4c32faf2738e6c34064ab1af7` and retain both resolved and phase
configuration `Debug`. The previous configure mismatch is no longer their failure.
Both remain failed, `complete=false`, `complete_gate_eligible=false`:

| Profile | Receipt artifact | Verified archive SHA-256 | Current failure |
|---|---|---|---|
| `core.linux` | `12161152656` | `6ce38875cc72169731aabdd4e3734003cd1dbf4484c2e54b1eae89d721979331` | Full prebuild input audit; original findings retained |
| `groundhog.mdk08` | `12161662149` | `099737fc758330b9aaaab114386c71520d16bad47664f4e25e7f2f9e0dc7cd3e` | Live peer/image lifecycle requires existing guarded adapter |

Both downloads match GitHub's published artifact digest and pass ZIP CRC checks.
This is a **two-lane observation**, not a complete run-ledger refresh or a successful
profile/integration. The frozen ledger snapshot and its counters are unchanged;
no W11 progress or bead closure follows. `.31` retains the wider remaining scope.

### Wrapper input/target mapping follow-up

Commit `224209d37838f2a28fd5baaaab248e8df57e53cf` corrects two actual
`target_mapping` findings from hosted source `2743f1735`, run `39132650260`:
artifact `12138577637` is GNostr/GrottoASan; `12138633221` is relayd/GroundhogLinux.

- `tests/CMakeLists.txt` now mirrors app-enable options and GTK/GLib availability
  before registering GNostr wrapper tests. `tests/` precedes `apps/`, so checking
  `TARGET gnostr` there would be premature. Explicitly disabled apps no longer
  register an impossible executable; enabled apps retain sanitizer commands,
  settings and timeouts. The enabled ASan descriptor names real target `gnostr`,
  schema source/generated input and required display capability.
- `apps/relayd/tests/CMakeLists.txt` describes `relayd_bind_iface_integ` with real
  target `nostrc-relayd` and `run_bind_iface_integ.sh`; its temporary configuration
  is generated inline by that script. Neither wrapper command changes.
- Existing `tests/selection/test_realization_inputs.py` covers enabled/default,
  each disabled option and missing dependency states, unchanged sanitizer command
  contracts, File API mappings and real schema/target dependency closure.
  Review correction `abcf04bfd6c0dc74264d06278a8f64834ef19b40` pins the miniature
  fixture to Ninja/Release and proves ambient Debug/Ninja Multi-Config cannot
  change its descriptor/configuration contract. No inventory input addition is
  needed because this existing test file is already authored.
- Actual full-source Linux/arm64 configure/CTest-export/File API/Ninja probes
  record Groundhog 422 registrations (relayd present), GrottoASan with GNostr
  disabled 336 (GNostr absent), and GNostr enabled 400 (GNostr present). The
  corresponding export SHA-256 values are
  `215f3c675933269150219777036c0399b44b21f40b6ba78a585e0f9ebbffa11d`,
  `7afa0116f0b7d4fcee2bb1e01155fa37f8756b7ba49d0c7dce9e2e1a67dffd11`, and
  `360f4fb9904c7e1692e55b42aebed0ecdb1c3e31943509a9e863b50d284afa48`.
  Local checkpoint `/tmp/nostrc-wrapper-mapping-20261019/checkpoint.json`
  (SHA-256 `33e19b3484fdca9bc51848a5a299d6e6b1a713067ff97c1d2c7e057792dccf4c`)
  records arguments, image, pins, source/file identities and setup failures.
  This is **configure/export/graph evidence only**: no Linux product build,
  application execution, full input-audit pass, x86 profile admission or hosted
  acceptance. No exclusions, nonmember admission or audit guards are changed.

At final code SHA `abcf04bfd6c0dc74264d06278a8f64834ef19b40`, independent
`PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/selection -v`
passes **444/444, zero skips, 154.531 seconds**. All six shell gate suites, four
static checks and actionlint pass. The native macOS/arm64 default-all root
configure/build passed on the unchanged CMake/product bytes at `224209d37`, using
`-DNIP55L_SECRET_BACKEND=libsecret` and
`-DGROUNDHOG_STORE_KEY_DEFAULT_BACKEND=secret-service`, build parallelism 4;
Groundhog is default-off and the latter variable is unused in that configuration.
No native full-root CTest or application execution is claimed. Bounded independent
review found only the fixture portability issue corrected above.

`.9`, `.2` and the wider acceptance scope remain open. Other semantic, membership,
missing runtime inputs and lifecycle gaps are not repaired by these two mappings.

**Version assessment:** this audit, ledger, tracker reconciliation, internal CI
profile authoring and test-only changes require no shipped component bump.
