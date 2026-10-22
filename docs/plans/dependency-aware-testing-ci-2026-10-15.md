# Dependency-aware testing and CI/CD

> **Current acceptance accounting: [§17](#17-acceptance-reconciliation-and-run-ledger--2026-10-18).**
> The status summary immediately below and earlier progress sections are
> **HISTORICAL / superseded snapshots**. Their pending statements and failed
> audits retain their original source identities; §16 records compiled gates and
> §17 records the independently backfilled acceptance snapshot.

**Status (2026-10-16):** Integration `97e393cba6c8aa3fb3bf076b3862a39482f0e656` implements actual separate phase environments/fingerprints/audits, ordinary outer command/build contracts, an explicit shared sanitizer phase route, 98 exact command inputs/87-job accounting and independent workers for all ten authored profiles. This is not production/hosted or W11 acceptance. Review R1 / `.22` is CLOSED **by prelaunch guard/refusal only** at `8179275b2`: selected command/build-contract lanes needing display/private bus, including mixed lanes, are refused without dropping capabilities or fabricating receipts. No drain fix or wrapped positive acceptance; full custody `.23` and unknown full-gate fixture failure/lost-trace `.24` stay OPEN. Parent final combined at `2d64a7ac2`: **307/307 passed, 0 skips, 125.303s**; subsequent changes are docs/failure forensics only. Parent confirms at `97e393cba`: **all six shell suites, four static checkers, full actionlint including ShellCheck and range whitespace passed** (`/tmp/nostrc-phase-final-test-*.log`); no rereview/full-slice acceptance is claimed. Parent native combined **306/306 passed, 0 skips, 146.958s** at `90a1b675f`; production Linux/x86_64 command **306 passed, 0 skips, 405.309s** at `7e3745463` with 99 audited hashes/no gaps. These do not invalidate the wrapped-cancellation adversary or confer full-slice acceptance. Fresh native root build at `7fdadd251` passed, BUILD ONLY/Groundhog OFF. MDK always refuses full lifecycle success; `.11` cause, full union and W11/W12 remain open. No default/enforce/threshold/schedule/protection cutover. Current progress is §13; §§8–12 and earlier review/verification receipts retain their dated SHAs and limits.

**Date:** 2026-10-15 · **Plan tracker:** `nostrc-bunud` · **Implementation epic:** `nostrc-0cfsg` · **Source baseline:** `d25102afa480` (`master`, historical). **W1 audited checkout:** `9b8dc253c`.

Sections 2–7 retain the implementation specification and historical planning evidence. Sections 8–15 retain dated execution snapshots, not current pending/completion claims. Sections 16–17 and `docs/TESTING.md` distinguish compiled diagnostics and acceptance accounting from still-proposed or unaccepted behavior. The specification alone is not implementation or rollout evidence.

## Goal

Run only checks affected by each local or pre-merge change, while building their prerequisites and retaining downstream consumer coverage. Run broader suites after merge to master, on releases and manually; unknown dependencies broaden coverage with an explanation. Future implementation updates `AGENTS.md` to make this the default.

## 1. Summary

Implement one shared, versioned selection contract for local gates, pre-merge CI, post-merge validation, and releases. A small checked-in dependency inventory will describe source ownership, downstream test impact, execution profiles, and non-build dependencies; CMake/CTest and Ninja will validate its realization against the actual build. Selection will identify impacted tests first, then build their prerequisites—not run every prerequisite library’s tests. Preserve the existing execution safeguards, platform coverage, advisory classifications, and artifact ownership. This requires a coordinated orchestration refactor, not a production-code or build-system rewrite.

This is the implementation specification, not an implementation report. This planning run changes documentation only. All new commands, files, workflow interfaces and thresholds below are proposed unless explicitly described as current behavior.

## 2. Background and current-state analysis


### Scope and owner decisions
- Owner chose up-front involvement and answered: selective local and CI gates become the default; implementation must update `AGENTS.md`. Broad costly suites run after merge to master, on releases and manually. No nightly tier. Unknown paths/incomplete dependency information broaden coverage and explain why.
- No version bump for this documentation-only deliverable. Implementation must assess internal CI/test-only changes versus any shipped build-contract changes per `AGENTS.md:25-85`.

### Actual build/test graph seams
- Build prerequisites and test selection are different graphs. Root CMake enters core libgo/libnostr/libjson unconditionally; optional components include libhanami, Marmot/wrappers, applications and Groundhog (`CMakeLists.txt:352-399,471-545`). Groundhog is not under the apps switch. NIPs have individual option/dependency handling (`NipOptions.cmake:3-26,163-188`).
- `nostrgo`/`libgo` feeds `nostr`/`libnostr` (`libgo/CMakeLists.txt:94-95,259-262`; `libnostr/CMakeLists.txt:129-130,201-203`). `marmot` links nostr, NIP44 and crypto/storage providers (`libmarmot/CMakeLists.txt:173-197`). nostr-gobject bridges core, JSON, libgo, conditional NIPs and nostrdb (`nostr-gobject/CMakeLists.txt:214-255`). nostr-gtk portable/legacy layers build on that (`nostr-gtk/CMakeLists.txt:263-315`).
- Grotto links nostr-gobject/NIP55L/04 plus conditional 46/19/49/06/44; daemon uses control/bunker cores and NIP5F/46 (`apps/grotto/CMakeLists.txt:210-250,340-390`). Grotto Meson consumes prebuilt CMake libraries, not an independent full graph (`apps/grotto/meson.build:46-79`). GNostr has its own broad wrapper/NIP closure (`apps/gnostr/CMakeLists.txt:294-360`).
- Groundhog MLS service depends on store-Marmot, DM inbox, auth, conversations, account relays and NIPs17/59 (`gnome/groundhog/CMakeLists.txt:2227-2263`); shared identity/relay/UI/store/media changes can affect MLS. Store-Marmot links shared store and Marmot (`:1230-1255`); MLS UI links MLS service and NIP29 UI (`:2906-2915`); MLS media/files link shared media/service (`:3157-3192`). `groundhog-poll-ui` links MLS UI despite not having MLS in its name (`:2917-2926`). Same MLS UI executable has model and GUI CTest executions (`:2936-2972`).
- Shared/core tests also live in root `tests/`, not only component roots (`tests/CMakeLists.txt:102-120`). `BUILD_TESTING=OFF` is not universal: independent libnostr/test options and unconditional libgo registrations exist (`CMakeLists.txt:202-203,511-513`; `libnostr/CMakeLists.txt:333-339`; `libgo/CMakeLists.txt:286-315,378-402`).
- Grotto has labels and a build-only `signer-tests` aggregate (`apps/grotto/tests/CMakeLists.txt:105-125,405-408`). Groundhog lacks MLS/non-MLS ownership labels; common `gn_add_gtest` lacks ownership labels (`cmake/GnTest.cmake:21-54,105-106`). Need a registration inventory, not regex-only inference.
- Isolation fixtures are directory-local and registration-order-sensitive (`cmake/NostrcTestIsolation.cmake:8-23,35-57`). Groundhog adds GStreamer registry setup at its final isolation pass (`gnome/groundhog/CMakeLists.txt:3686-3724`). Preserve fixture and resource closure, private bus/HOME/XDG, schema setup, SQLCipher ordering and real display execution. A registered GUI test returning 77 is not executed GUI acceptance.
- Other component roots to cover in inventory: libhanami/libjson/marmot-gobject, components/nostrdb, signet, nson, pkcs11, tests/testkit, Testing/tools/benchmark, relayd/grelay/relayctl/blossom-cache, optional nostr-homed/dav/dispatcher/seal/share/search-provider/wallet-agent/nautilus/settings/publish, Seahorse/common/Shell integrations. Unknown or unclassified roots cannot silently disappear.

### Current orchestration and expensive-test boundary
- `scripts/pre-push:19-85` is static-only by default, checking pushed detached trees; `NOSTRC_FULL_GATE=1` delegates. `AGENTS.md:89-133` still mandates broad clean build and CTest. Update the future policy coherently; preserve pushed-ref isolation and multi-ref handling.
- `scripts/full-gate.sh:110-145,175-270` runs broad host build/CTest and parallel Linux smoke, with separate changed-path sanitizer selection. `scripts/linux-gate.sh:90-145,170-298` has incremental-volume locks, architecture guards and sanitizer mode. `scripts/linux-gate-smoke.sh:19-68` owns display wrapping, timeout/quarantine/report failure handling. Keep selection separate from execution safety.
- `scripts/sanitizer-gate-ci.py:9-38,60-93,446-489` provides a precedent for workflow-derived test/build commands and Ninja-input coverage auditing; unsupported workflow shape fails, but unmatched paths currently skip. New contract must broaden unknown paths rather than reuse that skip behavior blindly.
- Most normal CI workflows use coarse overlapping path sets. `libgo-ci.yml:85-125` configures/tests standalone libgo; its fuzz-driver job uses the repository root (`:197-252`). `nostrc-ci.yml:117-178` repeats its explicit audit tests in broad CTest. Grotto builds default targets but narrows execution by regex (`grotto-ci.yml:145-184`), with advisory TSan rather than a met blocking gate. Current Grotto paths omit libjson/NipOptions despite dependencies. Signet paths omit shared dependencies (`signet-ci.yml:9-45`).
- Normal Groundhog CI explicitly enables MDK011 and runs the voice compatibility case (`groundhog-ci.yml:118-122,257-279`). Local Marmot vector test `marmot_test_interop` is ordinary coverage (`libmarmot/CMakeLists.txt:395-413`); do not move all tests named interop/integration into expensive-only tiers.
- Live MDK options default OFF; Docker fixture builds permit 3600 seconds, cases use resource locks and skip semantics (`gnome/groundhog/CMakeLists.txt:2491-2679`). MDK 0.8 and MDK011 workflows are currently manual+daily schedules (`marmot-mdk-interop.yml:9-19`; `marmot-mdk011-interop.yml:15-26`). Migrate coverage to master/releases/manual before removing schedules.
- `bp-matrix-nightly.yml:3-7,23-87` is manual-only despite its name. `grotto-fuzz.yml:17-54,94-119,150-154` is PR/manual/weekly. `bp-matrix.yml:3-44,61-89` is main-only push + PR/manual. Benchmarks are opt-in (`benchmark/CMakeLists.txt:3-10,119-140`). Reconcile obsolete nightly wording in `docs/TESTING.md:260-266`.
- Workflow migration inventory: nostrc-ci, libgo-ci, libhanami-ci, libnostr-gi, groundhog-ci, grotto-ci, nip55l-libsecret-ci, nostr-homed-ci, signet-ci, static-checks, gnostr-appimage, windows-build, bp-matrix, bp-matrix-nightly, grotto-fuzz, marmot-mdk-interop, marmot-mdk011-interop, release, groundhog-release. Preserve special native architecture, display/private bus, install/build-contract and privileged-run constraints; do not equate packaging smoke with unit/integration coverage.

### Release and required-check seams
- Release workflow maps only some component prefixes plus legacy v* (`.github/workflows/release.yml:9-96`); manifest has more components (`VERSION_MANIFEST.md:12-30`). Groundhog has a separate release workflow. Windows currently only publishes legacy v* (`windows-build.yml:18-21,255-259`). Preserve existing artifact ownership; no new packaging platforms in scope.
- Shared release configure disables tests (`scripts/ci/configure-release.sh:33-44`). Main release publication currently ORs successful artifact jobs (`release.yml:343-350`); must require release validation and every applicable required package result, without rejecting intentionally non-applicable jobs. Groundhog already rejects failed/cancelled packaging (`groundhog-release.yml:107-254`). Validation must attest the exact tag SHA, not unrelated green master.
- Actual server-side required checks/branch rules were not found in repo; inventory them during migration. Do not invent current protection state. Need stable required gate and a no-gap operator cutover.

### Verified external constraints
- GitHub built-in path filters have finite changed-file coverage (current docs: 3000 files), event-dependent diff semantics and do not apply to tags. Use an unfiltered entrypoint with in-job complete git diff/classification rather than making path-filtered workflows required. [Workflow syntax](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax#git-diff-comparisons).
- A path-skipped workflow can leave required checks pending; conditionally skipped jobs can look successful. A final always-evaluated gate must inspect selection and every required result, rejecting selected-but-skipped/failure/cancelled/missing results. Include merge_group support if using merge queues. [Required-check behavior](https://docs.github.com/en/pull-requests/how-tos/merge-and-close-pull-requests/troubleshooting-required-status-checks).
- Keep fork PR execution read-only/no secrets and validation in the same workflow/reusable-call graph, not privileged workflow_run artifact trust. [Events](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows); [Secure use](https://docs.github.com/en/actions/reference/security/secure-use).


### Established architecture

The source evidence above grounds four distinct graphs:

| Graph | Existing source of truth | Required use |
|---|---|---|
| Build prerequisites | CMake targets, link dependencies, custom commands, generated resources | Build everything selected checks need |
| Test impact | Currently implicit across sources, links, fixtures, workflows, and application behavior | Select checks affected by changed inputs, including downstream consumers |
| Execution requirements | Workflow jobs and local gate scripts | Preserve platform, compiler, instrumentation, display, keyring, resource, and privilege constraints |
| Release applicability | Release workflow conditions and `VERSION_MANIFEST.md` | Validate the tag and require all applicable artifacts before publication |

The important dependency direction is prerequisite → consumer: `nostrgo`/`libgo` → `nostr`/`libnostr` → NIPs, Marmot, wrappers, and applications. Selecting a Grotto test therefore builds shared libraries without automatically selecting their own test suites. Changing a shared library, however, selects its downstream consumers.

Groundhog needs finer ownership than one application-wide node:

- `groundhog-store-marmot` depends on shared store and Marmot.
- `groundhog-mls-service` depends on that adapter, DM inbox, auth policy, conversations, account relays, and NIP17/59.
- `groundhog-mls-ui` depends on MLS service and NIP29 group UI.
- `groundhog-poll-ui` depends on MLS UI despite its name.
- MLS media and files depend on shared media and MLS service.
- Several tests compile production sources directly rather than link their production library. Their impact must follow those source inputs too.

`groundhog-mls-ui` and `groundhog-mls-ui-gui` are separate CTest executions of one executable. Building that executable does not prove either execution ran.

Grotto’s Meson configuration consumes CMake-built libraries. Its validation must include that prerequisite closure; it is not an independent dependency graph.

### Existing mechanisms to retain

- `scripts/pre-push` already validates detached pushed trees and handles multiple refs.
- `scripts/full-gate.sh` already separates host and Linux source trees and cleans up background stages.
- `scripts/linux-gate.sh` owns architecture checks, incremental-volume invalidation, and cross-gate locks.
- `scripts/linux-gate-smoke.sh` owns display wrapping, retry evidence, sanitizer-report blocking, perf serialization, and named quarantine.
- `scripts/sanitizer-gate-ci.py` demonstrates configuration parity and Ninja-input auditing.
- `cmake/NostrcTestIsolation.cmake` supplies private HOME/XDG profiles and CTest fixture dependencies.
- Groundhog’s final isolation pass also registers GStreamer setup. Selection must preserve both setup tests, schema prerequisites, and SQLCipher link ordering.

These are execution mechanisms, not alternative selectors.

### Current gaps to address

1. Independent workflow path filters overlap and omit dependencies. Grotto omits `libjson`/`NipOptions.cmake`; Signet omits shared dependencies.
2. Groundhog and common `gn_add_gtest` registrations lack sufficient ownership metadata.
3. `BUILD_TESTING=OFF` does not universally eliminate test targets or registrations. Selection cannot use it as evidence that a build contains no tests.
4. `libgo-ci.yml` primarily configures standalone `libgo`; only its fuzz-driver job configures the repository root. Preserve these distinct profiles rather than treating all jobs as root builds.
5. Normal Groundhog CI currently enables and executes live MDK011 voice compatibility. Moving that coverage requires an explicit dependency rule, not deleting the step.
6. Local Marmot vectors, including `marmot_test_interop`, are ordinary coverage; “interop” is not an expensive-tier classification.
7. Main release publication currently accepts any successful artifact job. Groundhog’s release condition is stronger, but still needs explicit validation and success requirements for every applicable job.
8. Existing skipped, best-effort, quarantined, and advisory checks are not equivalent to blocking passes.

## 3. Design

### 3.1 Scope and invariants

#### Policy

Use two execution modes:

- **Affected**: default for local development, pre-push, PRs, and merge groups. Select ordinary and expensive checks whose dependencies are affected.
- **Broad**: post-merge pushes to `master`, compatibility pushes to `main` while supported, release validation, and manual invocation. Select every applicable supported check, including costly unaffected suites.

There is no scheduled or nightly execution tier.

“Expensive” is descriptive metadata, not permission to defer an impacted check. Unknown changes select broad coverage, including expensive checks.

#### Non-negotiable invariants

1. Building prerequisite target B for selected test A does not select B’s tests.
2. Changing B selects all test consumers reachable through the impact graph.
3. All changed paths receive an explicit classification or a recorded broadening reason.
4. Selected checks cannot disappear because a configure option, missing package, test regex, job condition, or runner capability excluded them.
5. Fixture/setup/cleanup and runtime-resource closure is mandatory.
6. A selected blocking check must execute successfully; skipped, missing, cancelled, or unregistered is not success.
7. Advisory and named-quarantine outcomes remain distinguishable from clean passes.
8. Every receipt identifies the exact tested commit and selection contract.
9. Release publication requires exact-tag validation and every applicable package job.
10. No package platform, signing model, or supported production feature is added by this work.

### 3.2 Inventory: one policy file and one generated realization

#### Checked-in policy

Add `tests/selection/inventory.json`, read by `scripts/test-plan.py`.

Use JSON and Python’s standard library. Do not introduce a graph service, database, YAML parser dependency, or separate local and CI rule sets.

The top-level shape is:

| Field | Type | Contract |
|---|---|---|
| `schema_version` | integer | Initially `1`; reject unsupported versions |
| `nodes` | map keyed by stable ID | Source ownership and impact dependencies |
| `path_rules` | ordered records | Exact paths or directory-prefix ownership rules |
| `test_overrides` | records | Explicit ownership/runtime information that cannot be derived reliably |
| `profiles` | map keyed by stable ID | Supported configure/toolchain/environment combinations |
| `checks` | map keyed by stable ID | CTest groups or non-CTest checks |
| `release_components` | map keyed by canonical component ID | Validation scope and existing package applicability |

All IDs are stable lowercase identifiers composed of letters, digits and underscores, optionally separated by dots. A rename of an ID is a contract change and requires updating golden cases and consumers atomically.

A node owns:

- `id`;
- `owner_paths`: paths of responsible build/test declarations, not personal names;
- `depends_on`: prerequisite node IDs, meaning changes to those prerequisites can affect this node;
- optional `conservative`: whether finer dependency information is intentionally unavailable;
- a short rationale for every dependency not derivable from build inputs.

A path rule owns:

- exactly one `exact` path or `prefix`;
- `nodes`: one or more ownership IDs;
- `effect`: `impact`, `global`, or `non_execution`;
- `reason`.

Rules use normalized repository-relative paths and component boundaries. A prefix for `libgo/` also matches the gitlink path `libgo` if applicable, but never `libgopher/`.

Resolution is longest matching path first; equal-specificity matches union their nodes. A more-specific rule replaces its parent’s ownership deliberately. Contradictory equal-specificity effects are invalid.

`non_execution` is a narrow allowlist, initially:

- prose-only files under `docs/`;
- `.beads/` tracking records;
- `LICENSE`.

Executable fixtures/scripts under docs are not prose and override that allowlist. `AGENTS.md` and testing guidance select static/policy-contract checks, not runtime suites merely for being Markdown. `VERSION_MANIFEST.md` selects version/release-contract checks and affected packaging checks. This plan is prose-only. Do not ignore every Markdown file: a manifest can affect release behavior.

#### Profiles and checks

A profile owns:

- platform, architecture, compiler, build type, instrumentation, and source directory;
- explicit configure arguments and environment;
- available capabilities;
- setup workflow/job;
- local availability;
- applicable feature variants;
- execution parallelism and timeout policy.

A check owns:

- impact node IDs;
- eligible profile IDs;
- kind: `ctest`, `command`, or `build_contract`;
- `cost`: `ordinary` or `expensive`;
- `requirement`: `blocking` or `advisory`;
- expected outcome: `pass` or an exact documented expected-incompatibility case;
- build targets and runtime resources not derivable from CTest;
- command arguments, working directory, and environment for non-CTest checks;
- for CTest checks, exact registration-name membership per eligible profile (`ctest_tests`), normally one primary execution per stable check ID; aggregate aliases list check IDs, never duplicate physical executions. Fixture registrations are support executions, not separate logical pass aliases. Resolve only against the freshly generated registration set; reject missing or multiply claimed primary executions.

CTest descriptors are authoritative for runtime inputs/ownership attached to repository-owned registrations; `test_overrides` covers external/legacy registrations that cannot yet carry descriptors. The same field cannot be independently specified in both places: disagreements or duplicate authority fail validation rather than using silent precedence. Check membership/capabilities must agree with descriptors and the realized profile. Generated build dependencies supply target facts; JSON contains only selection policy and semantic edges, not a parallel hand-copied linker graph.

Commands are argument arrays with only documented source/build-root substitutions. Do not generate shell fragments from filenames or use `eval`.

Named quarantine remains in `scripts/grotto-ci-quarantine.tsv`; do not duplicate it in this inventory.

#### Generated build realization

Add `cmake/NostrcTestInventory.cmake` and generate an inventory under each build directory. Combine:

1. CMake File API codemodel for target IDs, source files, artifacts, dependencies, and configurations.
2. CTest `--show-only=json-v1` for exact test names, commands, environments, fixtures, resource properties, labels, working directories, and timeouts.
3. Explicit additive CMake test descriptors for runtime inputs and ambiguous command-to-target mappings.
4. Ninja inputs and discovered header dependencies after building.

**Bootstrap owner:** the Python configure adapter creates `<build>/.cmake/api/v1/query/client-nostrc-test-selection/codemodel-v2` before invoking CMake for that profile. After successful configure/generate exits, Python consumes that generation's client reply, descriptor export and CTest JSON. The CMake helper only exports descriptors; it cannot consume a codemodel reply before generation completes. Missing, errored or stale replies fail realization, not produce an empty inventory. Root CMake includes/finalizes the helper once after all test and isolation registrations. Standalone `cmake -S libgo` must include/finalize the same helper under a guarded top-level-libgo condition; root-with-libgo must not double-export. This is additive metadata, not a CMake-minimum increase. See [CMake 3.22 File API lifecycle](https://cmake.org/cmake/help/v3.22/manual/cmake-file-api.7.html).

Generated data is build-local, never checked in. Identify it by:

- execution source identity: committed SHA for clean exact-tree runs, or HEAD plus complete snapshot-content digest for dirty runs;
- inventory content hash;
- profile ID;
- configure arguments/environment hash;
- generator, compiler identity, and tool versions.

Never reuse an index merely because its build directory exists.

#### Test descriptors

Introduce an additive helper with the conceptual interface:

`nostrc_describe_test(name, OWNER…, TARGETS…, INPUTS…, REQUIRES…, EXPECTED…)`

It supplements an existing `add_test`; it does not register a second test.

- `OWNER`: impact node IDs.
- `TARGETS`: executable and runtime targets.
- `INPUTS`: scripts, schemas, vectors, templates, configuration, or other runtime files.
- `REQUIRES`: capabilities such as display, private bus, Docker, FUSE, or external peer.
- `EXPECTED`: ordinary pass, fixture, or the explicitly named incompatibility probe.

For straightforward executable tests, match the resolved CTest command to the File API artifact unambiguously. Wrapped commands, scripts, or multiple candidate executables require descriptors; do not guess from test names.

Extend `gn_add_gtest` and `gn_add_gtest_xvfb` with optional ownership/runtime metadata. Existing calls remain valid; strict selection validation reports unresolved ownership until classified.

Preserve existing labels and append selection labels. Labels aid inspection; they are not the authoritative dependency graph.

#### Ownership and maintenance

- Component maintainers update ownership and semantic edges alongside source/build changes.
- Test authors declare runtime files and capabilities alongside registrations.
- CI maintainers own execution profiles, requirements, and release applicability.
- The static validation job enforces consistency.

No test or root is silently excluded during migration. Unclassified entries are visible inventory records and force broad coverage; enabling selective enforcement additionally requires zero unclassified executable registrations in supported profiles.

### 3.3 Initial graph and inventory coverage

#### Required ownership nodes

Use coarse nodes for ordinary components and finer nodes only where savings require them.

| Area | Initial ownership |
|---|---|
| Core | `libgo`, `libnostr`, `libjson`, `nostrdb`, `libhanami`, `libmarmot`, `marmot.gobject`, `nostr.gobject`, `nostr.gtk` |
| NIPs | One node per actual `nips/*` subtree, including schema, Blossom, CAS modules, NIP55L core/GLib where separable |
| Applications | `grotto.gui`, `grotto.daemon`, `grotto.native_host`, `gnostr`, `signet`, `relayd`, `grelay`, `relayctl`, `blossom_cache` |
| Groundhog | Nodes described below |
| Shared tests/build | `testkit`, `testing.framework`, `shared.tests`, `tools`, `benchmarks`, `build.global`, `selection.contract` |
| Other build roots | `nson`, `pkcs11` |
| GNOME integrations | Separate nodes for common, Seahorse, D-Bus assets, homed, DAV, dispatcher, seal, share, search provider, wallet agent, Nautilus, settings, publish, and Shell extension |
| Packaging | Existing application/component package recipes and shared packaging inputs |
| Vendored code | Per known submodule/vendor root; unknown third-party changes broaden globally |

Groundhog nodes:

- `groundhog.identity_auth`;
- `groundhog.account_relays`;
- `groundhog.relay`;
- `groundhog.store`;
- `groundhog.conversations`;
- `groundhog.dm`;
- `groundhog.net`;
- `groundhog.media`;
- `groundhog.group_ui`;
- `groundhog.poll_ui` (the poll test/app source, distinct from poll widgets compiled into MLS UI);
- `groundhog.mls_store`;
- `groundhog.mls_service`;
- `groundhog.mls_ui`;
- `groundhog.mls_media`;
- `groundhog.mls_files`;
- `groundhog.shell_ui`;
- `groundhog.resources`;
- `groundhog.voice`.

Use exact source/target associations to separate these, including files outside `src/mls/`. Ambiguous shared UI/store files are multi-owned or assigned conservatively—not forced into a non-MLS bucket.

Required explicit edges include:

- `groundhog.mls_store` → shared store and `libmarmot`;
- `groundhog.mls_service` → MLS store, DM, identity/auth, conversations, account relays, NIP17, and NIP59;
- `groundhog.mls_ui` → MLS service and group UI;
- poll UI → MLS UI;
- `groundhog.media` → net, store, and conversations;
- MLS media → shared media and Marmot;
- MLS files → MLS service and MLS media;
- MLS UI → MLS files where the actual target linkage adds that edge.

Groundhog identity/relay prerequisites linked indirectly by DM/account targets must also be represented.

#### Full inventory coverage

Inventory every tracked top-level root and every registered check across the union of supported profiles. This includes components without a dedicated workflow today.

Use two explicit defaults:

1. A recognized component without fine metadata receives a coarse component node and all its registered tests.
2. An unrecognized path/root receives `unclassified`, selecting all profiles.

Do not fabricate fine-grained dependencies for unprovided code. During implementation, obtain the complete source/test list from the generated realization and populate exact ownership. Until validated, the conservative default remains active.

For optional components, inventory the compile/tests already realized by existing supported profiles, including indirect root-build coverage. Do not add a new platform or invent a test harness as part of this optimization. Components absent from every supported profile receive an explicit coverage-gap record and follow-up bead; changes there broaden existing coverage and report that the component itself remains unvalidated. They cannot receive a component-validated receipt. A selective production gate cannot claim that existing gaps were fixed by broader unrelated tests. Any new lane to close such a gap is separately scoped and approved. Absent desktop/system acceptance remains **not implemented/not run**, not a unit-test pass.

Unsupported or broken optional configurations must be documented with their existing issue and cannot count as coverage. Validate the referenced defects before treating them as current exclusions.

### 3.4 Deterministic diff and selection interfaces

#### Command surface

Add `scripts/test-plan.py` with these subcommands:

| Command | Purpose |
|---|---|
| `plan` | Resolve Git range, classify changes, produce logical checks/profile lanes |
| `explain` | Render the plan and dependency witnesses |
| `validate` | Validate policy schema, ownership coverage, references, cycles, and build realization |
| `resolve` | Resolve selected logical checks to exact tests, commands, targets, and fixtures after configure |
| `check-inputs` | Audit actual build/runtime inputs against declared impact coverage |
| `run` | Execute a resolved check phase and write evidence |
| `aggregate` | Validate required lane/check results |
| `release-plan` | Validate component/tag identity and package applicability |

Required `plan` inputs:

- repository root;
- exact `head` SHA;
- explicit context: `local`, `ci`, or `release`;
- mode: `affected` or `broad`;
- base SHA or event metadata;
- optional profile restriction for a deliberate local/manual partial run.

A profile restriction must appear in the receipt and cannot satisfy the complete CI gate.

Exit semantics:

- `0`: valid plan, including a valid fail-broad plan;
- `1`: validation/check failure;
- `2`: malformed contract, invalid invocation, or unrecoverable Git/tool failure.

No-change and documentation-only plans are valid explicit outcomes, not selector errors.

**Working-tree invocation:** without `--head`, `test-gate.sh` compares the merge base/default branch with HEAD and unions staged, unstaged and relevant untracked inputs. Generated build/output directories are excluded by explicit build-root rules, not by ignoring unknown source files. It executes a private snapshot containing those inputs, preserving the index/worktree untouched, and records HEAD plus a dirty-input content digest. Carry the execution source identity through realization/header-index keys, resolved plans, evidence paths and receipt matching. A changed snapshot digest forbids reusing coverage/audit evidence, even if HEAD is unchanged; compiled objects may still use content/configuration-validated incremental rebuilding. Unsupported untracked or submodule materialization is a visible failure, not omitted content. Dirty snapshots are developer feedback only and cannot satisfy pushed-tree, CI or release receipts. `--head` always executes that committed tree, not the working copy; an unmaterializable snapshot fails visibly. A plan cache key includes range and dirty digest, never only HEAD.

#### Git range rules

Use complete local Git diffs, with NUL-delimited status/path output and rename detection. Preserve both old and new paths.

| Context | Selection range |
|---|---|
| Existing pushed branch/ref | Remote OID → pushed OID |
| New local branch | Merge base with fetched default branch → pushed OID |
| PR | Union of merge-base(base SHA, PR head) → PR head and base SHA → tested merge SHA |
| Merge group | Event base SHA → merge-group SHA |
| Post-merge broad run | Broad mode; retain before/after diff for explanation |
| Release | Exact peeled tag commit; broad mode |
| Manual affected run | Explicit validated base/head; absent base becomes broad |

PR validation tests the merge SHA and records both PR head and tested SHA. Local pre-push tests the pushed tree; no receipt may imply those are necessarily the same tree.

Handling:

- **Rename**: classify old and new paths; union impacts.
- **Delete**: classify using base-tree policy as well as current policy.
- **Inventory change**: select broad before trusting narrower new rules.
- **Submodule change**: classify the gitlink itself; initialize the selected commit’s required submodules before configure.
- **Shallow history**: fetch the named endpoints/required ancestry; if a trustworthy range remains unavailable, select broad and state why.
- **New branch with no available default-branch base**: broad.
- **Force push/non-ancestor base**: compare the two supplied trees directly; do not silently substitute a smaller range.
- **Deleted ref**: no execution; record deletion.
- **All-zero OIDs**: recognize generically, without assuming SHA-1 length.
- **Unusual filenames**: preserve them through NUL input and JSON encoding; never split on whitespace.
- **Symlinks**: classify tracked paths without following them outside the checkout.
- **Diff failure**: never interpret as an empty change set.

For deletions and moves, union classifications from available base and head inventories. If either contract cannot be interpreted safely, broaden.

#### Selection algorithm

1. Validate event identity and inventory structure.
2. Produce the complete changed-path records.
3. Resolve path ownership.
4. Record every unmatched or ambiguous path.
5. If broad mode, unknown paths, graph/configuration changes, or incomplete relevant ownership applies, select broad coverage.
6. Otherwise traverse reverse impact edges from changed nodes. Test-only vector/fixture edits start at check-specific input nodes, not the production component node; this avoids selecting every production consumer for a test-data-only change. Source/header edges remain production impact nodes.
7. Select checks whose impact nodes intersect the affected closure.
8. Expand checks into supported profile variants.
9. Configure selected profiles and generate their realization.
10. Resolve exact CTest executions and non-CTest checks.
11. Close over fixture setup/cleanup, declared test dependencies, runtime targets, resources, schemas, and build targets.
12. Audit the realized graph before building.
13. Build only selected check targets and prerequisite closure.
14. Audit discovered Ninja/header inputs after building.
15. Execute, validate outcomes, and aggregate receipts.

Use sets and stable sorted output. Graph traversal is linear in nodes plus edges; path classification can remain a straightforward prefix/exact lookup at this repository’s scale.

Cycles in the semantic impact graph are allowed only as explicitly declared conservative groups; collapse those groups before traversal. Accidental unresolved cycles fail validation. The complete configured target/test realization is audited, not merely the checks initially selected; otherwise a missing edge to an omitted test could never be discovered. Preserve build-local discovered-header indexes with the profile identity, and make broad validation/input-audit evidence a prerequisite for enforcing a newly refined mapping. Candidate graph changes always run broad; never trust a stale remote cache as coverage proof.

#### Graph/configuration changes

These select all supported profiles:

- root `CMakeLists.txt`;
- `NipOptions.cmake`;
- shared `cmake/` modules;
- selection implementation/inventory;
- root configure presets;
- `.gitmodules`;
- shared toolchain, package, or gate-image definitions.

Component-local CMake/Meson changes select the component and its reverse consumers across relevant feature variants. Newly introduced or unresolved target relationships broaden globally.

A workflow-only change selects its owning execution profiles and contract tests. Shared entrypoint/aggregation/release-control changes select broad validation.

### 3.5 Explainability, receipts, and drift detection

#### Plan and receipt formats

Plans and receipts use JSON schema version `1`, plus a human-readable summary.

A plan contains:

- requested context/mode and effective mode;
- base, head, tested SHA, execution source identity (including dirty digest when applicable), and event identity;
- inventory hash;
- changed status, old path, and new path;
- classifications and broadening reasons;
- selected check/profile IDs;
- excluded checks and why they are unaffected or inapplicable;
- dependency witness paths;
- expected lane IDs;
- partial-run restrictions.

A resolved plan adds:

- exact CTest names;
- primary versus automatically included fixture tests;
- target IDs;
- runtime resources;
- required capabilities;
- expected outcomes.

An execution receipt adds:

- lane/check ID and profile fingerprint;
- actual execution source identity, with committed SHA and optional dirty snapshot digest;
- expected and observed executions;
- start/end times and configure/build/test durations;
- first-attempt, retry, quarantine, skip, and incompatibility outcomes;
- commands/tool versions;
- input-audit result;
- evidence filenames.

Check outcomes are a closed set:

`passed`, `failed`, `cancelled`, `missing`, `unexpected_skip`, `expected_incompatibility`, `quarantined_failure`, `advisory_failure`, `not_applicable`.

A successful retry remains `passed` with a recorded initial failure. It is not a clean first-attempt pass.

Write plans and receipts atomically. A terminated worker leaves incomplete evidence, never a synthesized success receipt. Keep artifacts separated by SHA/profile/lane/run attempt.

#### Validation rules

`validate` must detect:

- unmatched tracked executable/build roots;
- unknown referenced nodes/profiles/targets;
- unowned tests;
- ambiguous executable mappings;
- test registrations missing from required profiles;
- runtime inputs without owners;
- unsupported expected-skip exceptions;
- actual target input dependencies not covered transitively by the impact graph;
- changed build/profile settings not reflected in profile identity.

Do not treat every CMake configure input as a dependency of every suite. Root/shared configure files already have explicit global rules; target input audits concern actual compiled, generated, linked, and declared runtime inputs.

When realized dependencies exceed the authored graph:

- broaden the current run;
- report the exact missing edge/input;
- block selective-enforcement acceptance until metadata is corrected.

If discovered-header auditing finds a missing dependency after a targeted build, do not return success for that selection. Run broad recovery where feasible, but retain a blocking inventory-drift result; broad recovery is evidence, not permission to conceal drift.

Malformed inventory is fatal: an uninterpretable contract cannot establish that broad coverage itself is complete.

### 3.6 Execution, isolation, and local parity

#### CTest execution

Use exact test names, not suite-name regex inference.

Resolve stable exact test names against the freshly enumerated registration into a CTest `-I <selection-file>` index list and execute one CTest invocation per profile/attempt. The file uses CTest's start/end/stride plus explicit numeric entries to select exactly the resolved names; never persist numeric positions as stable IDs. An empty selection does not invoke CTest. Verify show-only names, fixture closure and declared ordering dependencies against the resolved plan before execution. This avoids shell argument limits without arbitrary batches that could reset fixtures or lose cross-batch ordering. Do not combine the selection with `--rerun-failed`, which overrides filters. CMake 3.22 supports the file interface; no minimum-version increase is needed. See [CTest 3.22 selection options](https://cmake.org/cmake/help/v3.22/manual/ctest.1.html).

`DEPENDS` is ordering-only, not failure propagation; include selected ordering prerequisites conservatively and preserve that distinction from fixture setup requirements ([CTest DEPENDS](https://cmake.org/cmake/help/v3.22/prop_test/DEPENDS.html)). Never run separate CTest processes concurrently against one isolation directory. Preserve each attempt's JUnit and compatibility-vector evidence before any retry/setup reset; MDK011 setup deletes `vectors.jsonl` (`gnome/groundhog/CMakeLists.txt:2590-2595`). Use distinct result/evidence paths per attempt. Count fixture executions separately from deduplicated logical check IDs.

Preserve:

- `FIXTURES_REQUIRED`, all matching setup/cleanup registrations, and `DEPENDS`;
- resource locks, serial execution, timeouts, and working directories;
- test HOME/XDG isolation and profile guards;
- schema/resource prerequisites;
- Groundhog’s GStreamer registry setup;
- SQLCipher-first target linkage;
- display/private-session-bus requirements;
- serial perf checks;
- sanitizer options and narrow third-party suppressions.

Validate expected tests against CTest JSON before execution and against JUnit/results afterward. A missing registration or empty selected check fails.

For new plan-driven execution, make structured results authoritative. Existing text logs remain diagnostic; do not rely solely on the current whitespace-sensitive log parser for result completeness.

#### Expensive checks

Define distinct expensive checks for:

- live MDK 0.8 compatibility;
- MDK 0.11 adopted matrix;
- MDK011 White Noise voice compatibility;
- extended backpressure matrices;
- extended fuzzing;
- existing benchmark/performance workloads;
- privileged/system integration where it already exists.

Keep ordinary local vectors, ordinary integration tests, short fuzz/corpus checks, and existing required perf budgets in affected mode. Expensive check IDs are per real execution/case, with aggregate aliases for full matrices; the voice case belongs to the MDK011 matrix and must not execute twice under both aliases. Deduplicate by check ID and compatible profile fingerprint; do not merge distinct compiler/instrumentation runs or model/GUI executions.

MDK checks depend on actual harness source/target/runtime inputs and explicit semantic dependencies:

- shared MLS service/store/protocol changes select both compatibility versions;
- MLS UI changes select checks from actual compiled/linked/runtime input closure, not a filename or `--gui` assumption. Default to all executions of a shared harness executable when that executable consumes the input. In particular `src/ui/gh-mls-copy.c` is directly compiled into the legacy 0.8 harness (`gnome/groundhog/CMakeLists.txt:2500-2504`) and also reaches the adopted executable through MLS UI (`:2582-2584,2908-2915`), so it selects both versions. Include late-added target/runtime relationships, including adopted voice (`:3310-3315`). Finer case-level overrides require explicit semantic ownership, a compatible input-audit rule and regression evidence; do not split production targets to manufacture savings;
- shared media/voice inputs select every compatibility case consuming those paths;
- a genuinely isolated non-MLS UI change does not select live MDK.

The MDK 0.9 incompatibility probe retains its exact expected outcome. Only that named case may produce the documented exit-77/XFAIL evidence. Other adopted/control cases must pass; an arbitrary skip cannot satisfy the matrix.

#### Local command surface

Add `scripts/test-gate.sh`:

- default: affected checks for the working change or explicit range;
- `--base` / `--head`: exact-tree validation;
- `--broad`: broad local validation;
- `--explain`: planning only;
- `--profile`: explicitly partial diagnostic run.

`pre-push` becomes static checks plus affected local execution on each detached pushed tree. It must:

- retain range whitespace checks and candidate-tree static checkers;
- initialize required submodules;
- deduplicate identical pushed SHAs while preserving each range’s impact union;
- retain per-ref evidence and fail if any required local execution fails;
- never validate the caller’s uncommitted worktree instead.

Local required profiles are the supported host-native profile and the existing Linux-container equivalents selected by the logical checks. Windows, native ARM CI, and other unavailable hosted profiles remain explicitly pending CI coverage; local success does not claim those ran.

Missing required local Docker/display/tool prerequisites block by default. Existing bypass variables remain loud overrides and produce an **incomplete/bypassed** receipt, never a valid CI receipt.

`NOSTRC_FULL_GATE=1` invokes broad mode through the same selector. `scripts/full-gate.sh` retains its public role and cleanup/safety behavior, but no longer maintains its own Groundhog/sanitizer path filters.

#### Sanitizer configuration

Move the Groundhog sanitizer profile’s test capability assignments, targets, configure arguments, and environment into the shared contract.

During migration, `scripts/sanitizer-gate-ci.py` remains a compatibility adapter:

- `config`: emits the equivalent profile configuration;
- `affected`: delegates classification, including unknown-path broadening;
- `check-inputs`: delegates the shared input audit.

After callers migrate, remove the bespoke workflow YAML parser and duplicated `SANITIZER_PATHS`. Keep wrapper commands until their existing shell tests and documented callers are updated atomically.

Do not change advisory TSan into blocking coverage or weaken LSan failure behavior.

#### Safety boundaries

The selector cannot bypass repository safety rules: never change macOS keychain defaults/search lists or delete developer keychains. Preserve the read-only keychain guard and private Secret Service configuration for host smoke/manual profiles (`AGENTS.md:217-241`). A failing guard stops execution and reports owner repair; no automatic repair. Keep Nostr protocol review mandatory when the underlying implementation actually changes Nostr behavior; orchestration-only metadata is not a protocol change. Do not add banned relays or contact public relays to validate selection.

#### Lifecycle and concurrency

Plans are immutable per source identity, inventory hash, event/range, effective mode and profile projection. Execution state is per build/profile/run/attempt; different base ranges for the same head are not interchangeable.

Retain existing Docker volume and test locks. MDK driver images also require cross-checkout ownership: use content/snapshot-specific image tags or immutable IDs where existing harness parameters support them (`MDK011_INTEROP_IMAGE`, `gnome/groundhog/CMakeLists.txt:2604-2613`). For fixed 0.8/0.9 tags (`:2529-2533,2640-2645`), hold a machine-wide image lifecycle lock through image setup and every consumer, rather than refactoring harness behavior here. CTest `RESOURCE_LOCK` is not a cross-process lock. Record resolved driver/image identity and build completion in receipts. Track this run's container IDs/labels; cancellation removes only its containers and releases its locks—never global Docker prune or sibling kills. Include profile/configuration fingerprints in cache invalidation; test selection alone need not invalidate compatible compiled prerequisites.

Cancellation must terminate child processes/containers, release locks, retain available logs, and clean temporary worktrees. Broad and release runs must not be cancelled merely because a newer branch commit arrives.

### 3.7 CI topology and required status semantics

#### Unfiltered entrypoint

Add `.github/workflows/dependency-tests.yml` with:

- `pull_request` targeting `main`/`master`;
- `merge_group` for compatible merge-queue validation;
- pushes to supported integration branches;
- tag pushes matching `v*` and `**-v*`, with routing described in §3.9;
- `workflow_dispatch`.

Do not use workflow-level path filters on this entrypoint.

Jobs:

1. **select**: complete diff, policy validation, logical plan.
2. **execution families**: reusable calls to migrated workflows, receiving exact SHA, mode, selected lane IDs, and plan.
3. **required gate**: stable job name `Dependency gate`, `always()` evaluated, directly dependent on selection and every execution family.

Retain distinct reusable families where setup materially differs. Do not replace native ARM, macOS, Windows, FUSE/systemd, AppImage, and sanitizer environments with one generic Linux worker.

Each family uses the plan to construct only applicable matrix rows. An empty family is an explicit unselected family; do not create a fake passing test row.

#### Worker contract

Each worker must:

- check out and verify the requested SHA;
- use the requested profile;
- resolve and validate its selected checks after configure;
- build selected targets;
- execute with existing safeguards;
- upload resolved plan and result receipt even on failure when possible.

Use same-run artifacts and reusable calls, not privileged `workflow_run` ingestion.

Fork PRs use read-only tokens, no secrets, and `persist-credentials: false`. Do not use `pull_request_target` to execute candidate code. Privileged-container tests may run only on disposable GitHub-hosted infrastructure under their existing restrictions; unavailable capabilities are explicit unavailable results, not passes.

#### Aggregation rules

`Dependency gate` succeeds only when:

- selection succeeded;
- all expected family/lane IDs are accounted for exactly once;
- every selected blocking check has an acceptable result;
- execution source identity, inventory hash, profile, and plan identity match;
- required registration/input audits passed;
- applicable worker job conclusions are successful.

Reject:

- selected-but-skipped jobs;
- missing receipts or tests;
- cancelled/failed required jobs;
- mismatched SHA/profile;
- duplicate or contradictory receipts;
- unexpected skips;
- malformed artifacts.

Advisory failures and valid named quarantine remain visible without silently becoming clean passes. Preserve current gating semantics; release validation must separately report whether any applicable existing promotion rule disallows quarantine.

A documentation-only plan can pass with static/contract validation and an explicit empty execution set. An empty matrix alone is not proof of correct selection.

GitHub path filters have finite diff coverage and do not protect tags; skipped required workflows can remain pending, while conditionally skipped jobs can appear successful. Complete in-job selection and explicit aggregation avoid both traps. See GitHub’s [diff documentation](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax#git-diff-comparisons) and [required-check guidance](https://docs.github.com/en/pull-requests/how-tos/merge-and-close-pull-requests/troubleshooting-required-status-checks).

### 3.8 Workflow migration matrix

All reusable validation calls remain within the entrypoint’s workflow graph. Existing manual entrypoints can call the same family directly in broad mode.

| Existing workflow | Future role and trigger class | Preserved contract |
|---|---|---|
| `nostrc-ci.yml` | Reusable core Linux/macOS sanitizer and short-fuzz family; affected pre-merge, broad integration/manual/release | Existing instrumentation and exclusions; audit tests execute once, with named reporting |
| `libgo-ci.yml` | Reusable standalone libgo variants plus root fuzz-driver checks | ASan/UBSan, TSan, MPMC, W4 compatibility restrictions, Linux/macOS distinctions |
| `libhanami-ci.yml` | Reusable focused family | Focused options, labels, Linux/macOS |
| `libnostr-gi.yml` | Reusable GI build-contract family | No-system-install generation, GIR/typelib artifacts, Linux/macOS |
| `groundhog-ci.yml` | Reusable Groundhog native, GUI, sanitizer, lifetime, keyring, and perf checks | Display/private bus, nak, SQLCipher, source-tree cleanliness; split live MDK011 voice into explicitly selected expensive check |
| `grotto-ci.yml` | Reusable Grotto signer/native/sanitizer/build-contract family | Native distro variants, advisory TSan, LSan counterfactuals, Meson prerequisite closure, deb inspection, Shell unit tests, named baseline evidence |
| `nip55l-libsecret-ci.yml` | Reusable refusing-session contract | Pinned image, private bus, network-disabled execution |
| `nostr-homed-ci.yml` | Reusable homed distro/build/unit and existing system integration family | Distro matrix, FUSE/systemd privilege constraints; best-effort checks stay advisory and cannot satisfy blocking coverage |
| `signet-ci.yml` | Reusable Signet variant family | Passkeys, SQLCipher, test hooks, Release coverage, CMake/Meson test-list parity |
| `static-checks.yml` | Unfiltered reusable/static contract checks | Existing checker self-tests and shell regression tests; add selector/aggregation tests |
| `gnostr-appimage.yml` | Reusable packaging/build-contract checks | Native x86_64/aarch64, install-tree and bundled dependency checks, packaging smoke; unchanged release artifact ownership |
| `windows-build.yml` | Reusable Windows installer build-contract family and retained legacy `v*` release owner | MSYS2/MinGW, NSIS workaround, installer/portable artifacts; no claim of unit coverage |
| `bp-matrix.yml` | Short capacity-regression check plus reusable extended matrix | Relevant backpressure changes select short and required expensive workload; broad mode runs all existing matrix workloads |
| `bp-matrix-nightly.yml` | Rename to `bp-matrix-extended.yml`; reusable/manual extended workload, also called by broad mode | Current consume-us variants and summaries; it is manual-only today, not a nightly service |
| `grotto-fuzz.yml` | Reusable short affected fuzz checks and extended broad/manual fuzz/coverage | Existing targets/corpora; remove weekly schedule after broad replacement is proven |
| `marmot-mdk-interop.yml` | Reusable expensive legacy compatibility family | Pinned MDK peer, local relays, required ordinary vectors, fixture/image timeout behavior |
| `marmot-mdk011-interop.yml` | Reusable expensive adopted compatibility family | Separate adopted evidence, control cases, real GUI execution, exact incompatibility probe |
| `release.yml` | Existing main package publisher plus broad exact-tag validation call | Existing component artifact mapping and native architectures; corrected all-applicable-job publication gate |
| `groundhog-release.yml` | Existing Groundhog publisher plus broad exact-tag validation call | Existing source/package ownership, distro/native architecture matrices, installed smoke checks |

Remove independent overlapping PR/push validation triggers after cutover, rather than leaving duplicate callers running broad jobs.

For fuzzing, fix manual target selection at matrix construction. Exiting one “filter” step currently does not skip subsequent steps. Use 60-second short affected runs and retain the existing 300-second default for extended runs; validate manual duration as a positive bounded integer, maximum 3600 seconds per target. Enable shell pipeline failure propagation.

Coverage-report generation remains diagnostic unless it was already blocking. Do not let its best-effort commands establish fuzz success.

Do not remove MDK schedules until equivalent broad `master` and release calls have run successfully. The final policy contains no daily, weekly, or nightly schedule for these workflows.

### 3.9 Release validation and artifact ownership

#### Exact-SHA validation

Add `.github/workflows/release-validation.yml`, reusable by both release workflows and the Windows legacy tag publisher.

Inputs:

- peeled commit SHA;
- tag name or dry-run identity;
- component ID;
- validation mode fixed to broad.

Every worker checks out that SHA. The validation output records tag, peeled SHA, inventory hash, profile results, and expected exceptions. A green run on another master commit is never substituted.

A tag push runs broad validation even if branch CI was previously green.

#### Tag event ownership and identities

`dependency-tests.yml` owns the validation-only tag dispatcher. Its `v*`/`**-v*` push path parses and peels tags before any worker checkout. Known component tags with `publisher=none` call broad exact-SHA validation using the same reusable families, with read-only permissions and no publication path. Known tags already owned by main/Groundhog/Windows publishers are explicitly routed there; the dispatcher records that route rather than launching duplicate validation. Each existing publisher calls `release-validation.yml` in its own run before publication. Legacy global tags intentionally have two existing artifact owners (main and Windows); each retains independent same-run validation, not privileged cross-workflow artifact reuse. Unknown component-shaped tags fail routing. Unrelated non-release tag formats are outside the dispatcher patterns and do not create releases.

Release identity is one of:
- **Component release:** canonical manifest ID plus validated `<component>-v<SemVer>` tag; compare base version to that component's authoritative sources and enforce allowed prerelease suffixes.
- **Legacy/global release:** `v<SemVer>` maps to an explicit `legacy_global` identity outside the component map. Validate SemVer syntax and source/manifest agreement for individually declared component versions; do not compare the legacy global number to every independent component version or invent a repository-wide version source. Preserve existing `global` package applicability and each publisher's draft/prerelease behavior.
- **Manual dry run:** selected supported component/global route plus exact checked-out SHA and a synthetic run identity; no tag-version equivalence is claimed for a branch ref. A tag-ref dry run additionally validates/peels its tag but remains non-publishing. Validation-only manual dispatch cannot publish. Preserve existing main-publisher opt-in tagged dispatch semantics and Groundhog's stricter tag-event publication guard; a new manual narrowing input is diagnostic only and cannot satisfy full release validation.

This routing is tested end to end as an event-to-worker contract, not only as a parser unit test. A reusable workflow alone does not subscribe previously unsupported tag prefixes.

#### Component mapping

`release_components` covers every manifest component:

- Existing main-release prefixes retain their current package mapping.
- Groundhog stays owned by `groundhog-release.yml`.
- Windows continues publishing only its existing legacy `v*` artifacts.
- Remaining manifest components receive validation-only tag handling. Preserve existing source-archive publication only for prefixes already handled by a publisher; do not expand source/binary publication to previously unsupported tags. Record publisher=none explicitly. Unknown prefixes fail rather than becoming global packaging.

Use canonical IDs for `grotto`, `nostr-shell-extension`, `nip46`, `nip19`, `nip34`, `nip55l`, and other manifest entries. Display names are not tag parsers.

Validate component/tag syntax and declared base version against authoritative sources, including duplicated CMake/Meson values. A prerelease suffix may differ only as allowed by `AGENTS.md`.

Unknown component tags fail validation; do not fall back to global packaging.

This work does not implement automatic version bumping or mutate release columns during validation.

#### Publication gate

Add an explicit release aggregate before publication.

Main-release applicability:

- source archive: always for existing main-publisher routes; not for validation-only tags;
- `global`: all existing main package jobs;
- `gnostr`: Linux GNostr packages, GNostr AppImage/Snap/Flatpak/macOS tarball;
- `grotto`: Linux Grotto packages, existing TCP-daemon packages, Grotto Snap/daemon Snap/Flatpak/macOS DMG;
- existing library/source-only publication prefixes: source archive only; validation-only manifest prefixes have no new publisher or package jobs.

Groundhog requires source, every existing deb/rpm/Arch/Flatpak matrix row, and Nix validation. Windows requires its build plus exact-SHA release validation.

All applicable jobs must be `success`; intentionally inapplicable jobs must be recorded as such. Failure, cancellation, missing results, or an applicable skipped job blocks publication.

Keep package build execution parallel to validation if desired, but publication depends on both. Download only expected package artifacts from the same run; validation logs must not be swept into release assets by unrestricted artifact downloads.

Dry runs produce evidence/artifacts but never publish. Existing prerelease/draft behavior remains unchanged.

## 4. File-by-file impact

The following are **future implementation changes**, not edits in this planning run. Avoid changing production target architecture just to make selection easier.

### New files

| File | Change and reason | Dependency |
|---|---|---|
| `tests/selection/inventory.json` | Authoritative ownership, impact, profiles, checks, release applicability | Foundation |
| `scripts/test-plan.py` | Planning, validation, realization, input auditing, execution receipts, aggregation | Inventory schema |
| `scripts/test-gate.sh` | Shared local entrypoint using the planner and existing execution mechanisms | Planner/profile contract |
| `cmake/NostrcTestInventory.cmake` | Additive test descriptors and build-local inventory export | Schema/descriptor contract |
| `gnome/groundhog/TestInventory.cmake` | Exact Groundhog execution ownership, runtime inputs, model/GUI distinctions | Descriptor helper |
| `tests/selection/test_selector.py` | Schema, graph, selection, and broadening tests | Planner |
| `tests/selection/test_git_ranges.py` | Synthetic Git histories and path handling | Range contract |
| `tests/selection/test_inventory.py` | CMake/CTest realization and drift fixtures | Exporter |
| `tests/selection/test_aggregation.py` | Required-result and release applicability tests | Receipt contract |
| `tests/selection/golden/` | Named input/expected-plan fixtures | Stable schema |
| `.github/workflows/dependency-tests.yml` | Unfiltered entrypoint, stable gate and validation-only tag dispatcher | Reusable families and release routing |
| `.github/workflows/release-validation.yml` | Broad exact-SHA validation | Shared CI families |

### Existing build/test files

| File | Planned change |
|---|---|
| `CMakeLists.txt` | Include inventory support early; export/finalize after all registrations and isolation helpers; no production target/default-option refactor |
| `cmake/GnTest.cmake` | Optional ownership/runtime arguments; append metadata/labels without replacing existing labels |
| `cmake/NostrcTestIsolation.cmake` | Describe generated setup fixture and inputs; preserve call ordering and isolation behavior |
| `gnome/groundhog/CMakeLists.txt` | Include exact test metadata; describe schema, app-process, MDK, GUI, GStreamer, and runtime dependencies; preserve SQLCipher ordering |
| `apps/grotto/tests/CMakeLists.txt` | Describe signer tests and runtime/schema resources; treat `signer-tests` as build-only, not test coverage |
| `apps/grotto/CMakeLists.txt` | Export explicit GUI/daemon/native-host and generated-resource relationships that command inspection cannot express |
| `tests/CMakeLists.txt` | Describe root-owned core/NIP tests by actual dependencies |
| `libgo/CMakeLists.txt` | Guard standalone helper inclusion/finalization so root and standalone configurations each export once; do not “fix” unconditional test options as incidental scope |
| `libnostr/CMakeLists.txt` | Export audit tests and preserve independent `LIBNOSTR_BUILD_TESTS` behavior |
| `libmarmot/CMakeLists.txt` | Declare vector runtime inputs and ordinary classification |
| `nostr-gobject/CMakeLists.txt` | Describe wrapper tests/runtime resources; reuse actual link closure |
| `nostr-gtk/CMakeLists.txt` | Describe object/portable/legacy test relationships, display and perf capabilities |

`apps/grotto/meson.build` and `NipOptions.cmake` need no production edits for selection. Inventory their existing dependencies and configure them through profiles. Any discovered correctness defect in those contracts is separately scoped.

For other roots, use the central inventory and generated realization rather than mechanically rewriting every `add_test`. Add a local descriptor only when a real runtime or command-mapping ambiguity requires it.

### Existing local scripts and tests

| File | Planned change |
|---|---|
| `scripts/pre-push` | Default affected execution; preserve detached trees, multi-ref handling, whitespace/static checks |
| `scripts/full-gate.sh` | Broad-mode adapter; remove independent path selection, retain safety/lifecycle handling |
| `scripts/linux-gate.sh` | Accept resolved profile/targets/checks; preserve locks, architecture guards, cache invalidation, container safety |
| `scripts/linux-gate-smoke.sh` | Accept plan-driven exact selection and structured-result validation; preserve retries, logs, quarantine, sanitizer and perf behavior |
| `scripts/sanitizer-gate-ci.py` | Compatibility adapter, then remove duplicated selector/YAML parser after migration |
| `scripts/test-pre-push.sh` | Selected default, multiple refs, deletion, cleanup, bypass, pushed-tree assertions |
| `scripts/test-full-gate.sh` | Broad adapter and cancellation regressions |
| `scripts/test-linux-gate-smoke.sh` | Exact selection, fixture closure, missing/skipped results, structured evidence |
| `scripts/test-sanitizer-gate.sh` | Shared-profile parity, unknown broadening, audit failures |
| `scripts/check-linux-ci-packages.py` | Update workflow-shape expectations only as needed; preserve package parity enforcement |

### Workflows and documentation

Every existing workflow in §3.8 changes according to its row. Reusable interfaces and callers must land together per family.

- Rename `bp-matrix-nightly.yml` to `bp-matrix-extended.yml`; retain compatibility artifact names initially if external consumers exist.
- `release.yml`, `groundhog-release.yml`, and `windows-build.yml` gain exact-SHA validation and explicit publication aggregation.
- `AGENTS.md` replaces contradictory broad-build/static-only instructions with affected local gates, broad override, prerequisites, bypass semantics, and CI responsibilities. Preserve versioning, keychain, protocol, tracking, and branch hygiene rules.
- `docs/TESTING.md` documents commands, logical-versus-build closure, profiles, receipts, expensive affected checks, expected incompatibility, quarantine, and desktop-acceptance limits. Remove nightly claims and misleading universal environment assertions.
- `docs/plans/dependency-aware-testing-ci-2026-10-15.md` receives this resolved specification and rollout evidence.
- `VERSION_MANIFEST.md` changes only if a separately assessed shipped build-contract correction requires it; internal CI/test metadata alone needs no version change.

## 5. Risks and migration

### Operational prerequisites

Before enforcement, record:

1. Actual repository rulesets, required status identities, branch applicability, and merge-queue use.
2. Availability of existing native ARM, macOS, Windows, Docker, display, and privileged-container runners.
3. Current status of referenced defect issues and exclusions.
4. Existing consumers of artifact names/manual workflows.
5. Measured baseline durations and counts.

These are implementation prerequisites, not unresolved architecture choices.

### Principal risks

- **Missing runtime dependencies:** addressed by descriptors, full input audits, shadow comparisons, and broad fallback.
- **Monolithic application targets:** may legitimately broaden app-process acceptance checks; do not remove runtime edges merely to improve savings.
- **Configuration-dependent coverage:** inventory the union of existing supported feature variants, not only default CMake.
- **Required-check gaps:** addressed by a dual-required-check cutover.
- **Cache contamination:** profile fingerprints and existing volume locks remain mandatory.
- **False confidence from skips:** exact expected exceptions and result completeness prevent it.
- **Known broken configurations:** cannot be made green by omitting them; retain tracked, explicit advisory/exclusion status.

#### Design rationale and rejected approaches

Choose a small Python/JSON policy layer because runtime files, external peers and semantic impact are not recoverable from linker dependencies alone. Validate it using the build system rather than maintaining a second hand-written target graph. Preserve existing worker scripts because replacing their platform, isolation and sanitizer behavior would expand the regression surface without improving selection. This accepts some conservative over-selection for monolithic binaries in exchange for demonstrable coverage; refining source/runtime ownership later must pass the same audit and shadow gates.

Rejected alternatives:

- GitHub path filters as authoritative or required selection.
- Directory/test-name regexes as Groundhog dependency classification.
- Selecting tests for every prerequisite library.
- Treating Grotto Meson as independent of CMake libraries.
- Universal broad pre-merge suites.
- A replacement nightly tier.
- Silently skipping unknown paths or missing metadata.
- A bespoke parser for every workflow when one shared profile contract suffices.
- Treating fake/private-bus tests or package launch smoke as real desktop acceptance.

### Shadow rollout and measurement

#### Baseline

Capture representative Grotto-only, isolated Groundhog UI, MLS, shared-core, shared-store/media, graph-change, and unknown-path changes.

For each, measure:

- clean and incremental runs;
- exact SHA, runner, architecture, compiler, profile, and tool versions;
- configured targets, built targets, selected tests, fixture counts, expensive checks;
- setup/configure/build/test durations separately;
- total runner-minutes and critical-path time;
- cache state and retries.

Use at least five samples per representative condition. Report median, range, and interquartile range; do not claim measured savings before collecting them.

#### Acceptance thresholds

Selective enforcement requires:

- 100% changed-path classification or explicit broadening;
- zero unexplained dropped checks;
- zero unowned/unresolved registrations in supported profiles;
- zero missing audited dependency edges;
- 100% required-result accounting;
- byte-stable logical selection for identical inputs;
- local/CI logical-selection parity, with only documented capability/profile projection differences.

Performance targets:

- Grotto-only: at least 30% median aggregate runner-minute reduction versus the legacy triggered workload.
- Isolated non-MLS Groundhog: at least 25% reduction and zero unrelated live MDK executions.
- Those representative critical paths must not regress by more than 10%.
- Broad-mode median time must not regress by more than 10% without an explicit measured coverage justification.

If savings miss these thresholds, retain correctness and refine ownership/build targets; do not weaken dependency edges.

#### Shadow comparison

Run legacy blocking gates and new non-required selection for at least 20 representative PRs and two successful broad integration runs.

Classify each difference as:

- prerequisite-only build;
- justified unaffected test omission;
- duplicate execution removed;
- missing coverage;
- deliberate profile/advisory distinction.

Every failure detected only by legacy coverage is investigated and represented in a regression fixture before cutover.

#### No-gap status cutover

1. Keep existing required statuses.
2. Deploy and observe `Dependency gate`.
3. Add it as required while old requirements remain.
4. Confirm PR and, where enabled, merge-group behavior.
5. Remove obsolete required statuses.
6. Disable duplicate legacy triggers.
7. Change local hook/AGENTS policy after shared execution is proven.

Rollback restores old broad required jobs before removing the new requirement. Keep additive CMake metadata: it does not prevent broad execution.

## 6. Implementation order and acceptance

### Execution index

**Latest acceptance accounting (2026-10-18):** [§17](#17-acceptance-reconciliation-and-run-ledger--2026-10-18)
links the criterion audit and source-linked run ledger. [§16](#16-migration-progress--2026-10-18-6299f003)
retains the compiled-source gates; earlier checkpoints remain historical.
W2–W10 progress is not full acceptance;
**W11 NOT MET / W12 NOT RUN**. The index's “Done when” column remains the required
acceptance contract, not a claim that those conditions have passed.

**Bounded W3–W5 membership checkpoint (2026-10-22):**
[`1ed8f087e` transport receipt](../reviews/dependency-aware-testing-ci-2026-10-18-acceptance-audit.md#headless-relaynetwork-transport-membership--2026-10-22)
adds six exact headless relay/network registrations, yielding **27 primaries / 29
expected executions**. Fresh Groundhog full findings **8,905 → 7,900** remove
exactly six unclaimed + 999 source findings, zero added; Grotto **9,146 unchanged**.
Both audits **FAIL**. Exact 33 omissions/29 required omitted-target closures and
193 source-review hashes per profile remain unchanged; only Groundhog selected
primary/fixture arrays renew. At that source, **521/521 selector** and **37/37
focused regressions PASS**, zero skips; safe native default-all build/static gates
PASS. Separate raw Linux/amd64 **8/8 CTest at base `53f2e822d` is not authored
gated execution**. Full native CTest, Linux all-target/omitted builds, production
postbuild headers and selected-profile backend execution NOT RUN. Custody
integration/two-path input listing and final combined gates await a verified
sibling commit. `.2` OPEN / `.9` in progress; no closure, source-authority renewal,
W11/hosted/release/default claim, ledger refresh or shipped version bump.

**Later W5–W6 integration checkpoint (2026-10-22):**
[Combined transport/custody receipt](../reviews/dependency-aware-testing-ci-2026-10-18-acceptance-audit.md#combined-transportcustody-integration--2026-10-22)
records preserved custody commits `8d87fff95` / `5b20780d3`, exact two-path
`selection.contract` input integration and authorized test-harness corrections.
Final code **`a4cfd1420`** passes **523/523 selector tests, zero skips**, six shell
suites, four statics, inventory validation, actionlint/ShellCheck and safe fresh
native default-all configure/build/keychain guards. First failed combined run and
source-proven XPC environment diagnosis remain retained; no fingerprint/refusal
relaxation. Custody's Linux-arm64 proof stays at `5b20780d3`, not refreshed here.
`--linux-custody legacy` remains default; explicit ptrace is unwrapped,
non-instrumented Linux diagnostic only, refusing instrumented/wrapped lanes before
launch, with complete/eligible/parity false. `.23` remains in progress; full native
CTest, production profile/input/header/omitted-build acceptance and W11/release
remain unclaimed. Earlier pending integration wording is historical; no authority
renewal, default/ledger change, new Oracle review or shipped version bump.

Sizes describe reviewable implementation slices, not time estimates: S = focused adapter or tests, M = one substantial seam, L = cross-family integration. Split L items into beads before implementation; this index is not a replacement for `bd` tracking.

| Item | Goal | Done when | Key files | Dependencies | Size |
|---|---|---|---|---|---|
| W1 — Baseline and contracts | Record current checks, runner capabilities, protection state and cost | Every existing job/profile has blocking/advisory/applicability provenance; representative baseline and actual required-check identities are recorded | Current workflows, local gates, `AGENTS.md`, measurement receipts | None | M |
| W2 — Logical selector | Implement deterministic policy and fail-broad planning | Schema, path/graph/range golden cases and unknown explanations pass; no execution changes | `tests/selection/inventory.json`, `scripts/test-plan.py`, selector/range tests | W1 inventory | M |
| W3 — Build/test realization | Validate authored policy against actual registrations and target inputs | Fresh/stale/failed configure and standalone/root libgo export tests pass; descriptors, fixtures, runtime inputs, dirty identities and synthetic drift covered | `cmake/NostrcTestInventory.cmake`, `GnTest.cmake`, isolation helper, component descriptors | W2 schema | L |
| W4 — Coverage population | Classify supported roots and Groundhog subgraphs | Zero unresolved supported registrations; unknown optional coverage gaps explicit; Grotto and isolated Groundhog golden cases select no unrelated tests | Inventory, Groundhog `TestInventory.cmake`, component test declarations | W3 | L |
| W5 — Resolution and evidence | Execute exact checks and aggregate complete results | Single-invocation exact selection, fixture/reset ordering, per-attempt evidence, dirty identity, input audits and adversarial aggregation tests pass | Planner resolve/run/aggregate, inventory/aggregation tests | W3; W4 before enforcement | L |
| W6 — Local adapters | Reuse execution safeguards under the shared plan | Shell regression, pushed-tree/multi-ref/dirty snapshot and concurrent MDK image/cancellation cases pass; legacy default remains during shadow | `test-gate.sh`, pre-push/full/Linux/smoke scripts and their tests | W5 | L |
| W7 — Sanitizer parity | Remove the duplicate Groundhog selector safely | Config/target/environment/input parity proven in workflow and local adapter; unknown paths broaden; LSan behavior unchanged | `sanitizer-gate-ci.py`, sanitizer tests, inventory, Groundhog workflow/Linux gate | W4–W6 | M |
| W8 — CI families and aggregate | Add selective workers behind one stable gate | Same-SHA fork-safe reusable families and expected-lane accounting pass; old checks still required | All §3.8 validation workflows, `dependency-tests.yml` | W5; family adapters W6–W7 | L |
| W9 — Broad tiers | Move scheduled coverage into integration/manual broad calls | Two broad runs include all existing MDK/fuzz/matrix workloads; replacement release dry run proven before schedules are removed | MDK, fuzz, BP and integration workflows | W8; W10 for release proof | M |
| W10 — Release gate | Prevent publication without exact-tag validation and applicable packages | Event routing covers annotated/validation-only/unknown/global tags and branch/tag dry runs; all existing publishers gated; no extra assets/platforms | `release-validation.yml`, release/Groundhog/Windows publishers, applicability inventory | W8 | L |
| W11 — Shadow and protection cutover | Prove omissions justified before making selection authoritative | Measurement, 20-PR/two-broad shadow gates, selected-skip/cancellation and merge-group checks pass; old/new protection transition has no gap | Receipts, GitHub rulesets, workflow triggers | W8–W10 | M |
| W12 — Default policy and cleanup | Make local and CI selective defaults consistent | Hook/AGENTS/testing docs switch together; obsolete selectors/triggers/schedules removed; full override still works | `AGENTS.md`, `docs/TESTING.md`, local adapters, obsolete workflow triggers | W11 | M |

### Ordered implementation

1. **Record operational prerequisites and baseline.** Use the corrected standalone/root libgo distinction above; assess version impact.
2. **Land schema, planner, and synthetic tests.** No execution-policy changes.
3. **Add CMake/CTest realization and descriptors.** Ordinary broad builds must remain unchanged.
4. **Populate complete ownership and profiles.** Use conservative component ownership first; refine Groundhog. Unclassified entries remain fail-broad.
5. **Implement resolution, input audits, receipts, and aggregation.** Test with small synthetic CMake projects.
6. **Adapt existing execution scripts.** Preserve legacy invocation until parity tests pass.
7. **Migrate Groundhog sanitizer configuration atomically** across inventory, adapter, workflow, Linux gate, and sanitizer tests.
8. **Migrate CI families individually.** Reusable input interface and callers land together; run in shadow.
9. **Deploy the unfiltered entrypoint and stable aggregate.** Keep legacy checks blocking.
10. **Add broad integration/manual execution.** Prove MDK/fuzz/matrix replacement coverage before removing schedules.
11. **Correct release validation/publication atomically** in each publisher, including applicable-job and artifact checks.
12. **Complete shadow/measurement gates and protection cutover.**
13. **Switch pre-push defaults and update `AGENTS.md`/`docs/TESTING.md` atomically.**
14. **Remove duplicated selectors, obsolete triggers, and nightly wording.** Retain compatibility adapters only where a recorded caller still needs them.

### Golden selection cases

| Case | Expected selection | Live MDK |
|---|---|---|
| Grotto GUI-only source/template | Grotto GUI/signer checks, applicable sanitizer and build contracts; build shared prerequisites | None unless an explicitly consumed shared input changed |
| Grotto daemon/bunker | Daemon/bunker, NIP55L integration, applicable fuzz/sanitizer and package contracts | None |
| Groundhog isolated About/shell UI input | Owning UI/resource tests and actual app-process consumers | None if no harness consumes it |
| `gnome/groundhog/src/mls/gh-mls-service.c` | MLS service/UI/files and consuming acceptance checks; store tests only if they compile/consume that source | MDK 0.8 and 0.11; voice where its harness consumes service |
| `gnome/groundhog/src/ui/gh-mls-copy.c` | MLS UI/model/GUI, poll and direct-compiled harness consumers | Legacy 0.8 plus adopted checks under conservative shared-executable mapping |
| Shared Groundhog store | Store consumers including MLS | Both versions and affected voice cases |
| Shared media | Ordinary DM/media plus MLS media/files consumers | Every media-consuming compatibility case, including affected MDK011 voice |
| Relay/auth/account-relay/shared conversation implementation | DM and MLS downstream consumers | Both versions when consumed by their harnesses |
| `gnome/groundhog/src/ui/gh-poll-ui.c` | Poll test/app consumers; do not run prerequisite MLS UI suites merely because the poll target links them | Only if an actual compatibility runtime/compiled input consumes it |
| `gnome/groundhog/src/ui/gh-create-poll-dialog.c` or `gh-poll-card.c` | MLS UI consumers, including the poll test, because these sources belong to MLS UI | Adopted harness checks consuming MLS UI; legacy only for an actual input edge |
| `libgo` source/header | Libgo variants and downstream core/NIP/wrapper/app checks | Both versions through core dependencies |
| NIP44 source | NIP44 consumers, Marmot/DM/signer downstream checks | Both versions |
| Local Marmot vector | Ordinary vector checks and direct vector consumers | Not automatically live MDK |
| Unknown path | All supported profiles, explicit unknown-path reason | Both versions and voice |
| Root graph/selection metadata change | Broad configuration/validation | Both versions and voice |
| Grotto-only workflow edit | Grotto profiles plus workflow/contract validation | None |
| Rename across ownership boundaries | Union of old/new ownership | As required by either side |
| Deleted runtime fixture | Base/head ownership plus missing-resource failure | Its consumers remain required |
| Submodule pointer change | Owning vendor dependency closure; unknown vendor broadens | Both if core/crypto inputs are affected |
| Unresolvable shallow range | Broad with recorded range limitation | Both versions and voice |
| Mixed Grotto + Groundhog MLS | Union, deduplicated by check/profile | MLS-required compatibility only |

### Named verification commands

The implementation must provide these documented checks:

- `python3 -m unittest discover -s tests/selection`
- `python3 scripts/test-plan.py validate --inventory tests/selection/inventory.json`
- `python3 scripts/test-plan.py validate --build-dir <build> --profile <id>`
- `python3 scripts/test-plan.py check-inputs --build-dir <build> --profile <id>`
- `scripts/test-gate.sh --base <sha> --head <sha> --explain`
- `scripts/test-gate.sh --base <sha> --head <sha>`
- `scripts/test-gate.sh --broad`

These commands are proposed interfaces, not commands run during planning.

#### Required acceptance scenarios

1. Synthetic rename, delete, force-push, gitlink, new-branch, shallow-history, whitespace/newline filename, and multi-ref tests.
2. Exact fixture closure, including multiple setup tests and cleanup registrations.
3. Model/GUI executions sharing one executable remain distinct.
4. Selected GUI exit 77 fails acceptance without a display.
5. Missing schemas, runtime files, target registrations, and header edges fail visibly.
6. Grotto Meson validation builds its CMake prerequisites without running unrelated library tests.
7. Selected expensive checks run before merge; unrelated live compatibility does not.
8. Sanitizer reports never pass through retry or quarantine.
9. Required aggregate rejects failure, cancellation, missing receipts, selected skips, stale SHA, duplicate lanes, and incomplete matrices.
10. Fork PR and merge-group workflow simulations preserve read-only, same-run validation.
11. Release simulations cover source-only, GNostr, Grotto, Groundhog, legacy Windows, dry-run, wrong-SHA, failed applicable package, and intentionally inapplicable package cases.
12. Broad runs execute all supported ordinary and expensive workloads after schedules are removed.
13. Source trees, developer keychains, HOME/XDG profiles, and resource locks retain their existing safety guarantees.
14. Fresh empty build root, stale/missing File API replies, failed configure, standalone libgo and root-with-libgo export correctly or fail visibly; descriptors never duplicate.
15. Reverse-sorted `DEPENDS` names retain ordering, setup failure remains explicit non-execution, multiple fixture setups/cleanup run, and a reset fixture's evidence survives per-attempt collection. JUnit paths never overwrite another attempt.
16. Known publisher=none annotated tags reach broad validation without any publish-capable job; unknown component-shaped tags fail; legacy/global and branch/tag dry-run routing preserve existing publishers' semantics.
17. Two dirty snapshots at identical HEAD with different header/generated content cannot share realization/audit evidence. Explicit `--head` ignores dirty content; unsupported snapshot materialization and cancellation leave the caller's index/worktree unchanged.
18. Concurrent build roots with different MDK driver inputs cannot use each other's images; cancellation cleans only owned containers/locks; partial/stale image builds cannot produce passing evidence.

## 7. Design-review resolutions and fidelity

The single bounded critique is retained at `docs/reviews/dependency-aware-testing-ci-2026-10-15-focused-critique.md` as a historical review of the pre-polish draft, not an outstanding finding list. Its six findings are resolved in this specification:
- Source-proven MDK inputs override UI-name assumptions; legacy `gh-mls-copy.c` and distinct poll paths have corrected goldens (§3.6, §6).
- Configure adapters own File API query/reply timing; standalone/root libgo exports are guarded (§3.2, W3).
- One indexed CTest invocation replaces arbitrary batches, preserving ordering, fixture reset evidence and per-attempt results (§3.6, W5).
- A named tag dispatcher, legacy/global identity and dry-run rules complete validation-only routing without new publication (§3.9, W10).
- Dirty snapshot identity propagates through every realization/audit/result key (§3.2–3.6, W3/W5/W6).
- Image lifecycle locks/content identity and owned-container cancellation cover concurrent MDK checkouts (§3.6, W6).

The final fidelity pass retained the planning response's applicable current-state, interface/schema, graph, edge-case, file-impact, rollout, measurement, sequencing and verification detail. Corrections are evidence/scope based: optional-component inventory does not authorize new CI support; previously unsupported tags validate without gaining a publisher; prose-only changes select policy/static checks; source/header and runtime ownership—not prerequisite tests or filenames—determine impact. Initial savings percentages are engineering acceptance targets, not measured results or additional owner decisions. No pipeline or live acceptance has been performed.

**Version assessment:** the plan itself requires no bump. Internal selector, workflow, test metadata, and documentation changes require no bump if shipped artifacts and public build contracts remain unchanged. Any separately necessary shipped packaging/build correction must receive the component-specific assessment required by `AGENTS.md`; do not hide it inside the CI-only exemption.

## 8. Execution progress — W1 first phase, 2026-10-15

**Evidence:** [W1 operational baseline](../reviews/dependency-aware-testing-ci-2026-10-15-w1-baseline.md), owned by `nostrc-0cfsg.5` (in progress, not closed). This phase changes documentation only; **no component bump**. The specification, acceptance thresholds, existing defaults, workflow triggers and schedules are unchanged.

- **W1 partial:** source-level job/profile and blocking/advisory inventory, read-only live rules/protection/merge-queue audit, local tools and bounded historical timings are recorded. GitHub access succeeded: no rulesets or classic master protection returned, master merge queue null, repository self-hosted runner list empty. These observations do not establish hosted capacity or authorize a protection change.
- **Delta `5d19e80f8..9b8dc253c`:** Groundhog alpha8 About/CLI version and `groundhog-packaging-version` now require four external packaging runtime inputs (`flake.nix`, Arch PKGBUILD, RPM spec, Debian changelog). The source-job tag/version guard is at `groundhog-release.yml:46-62`, not a completed W10 release gate. Shared RPM changes reach Grotto's zbar/GStreamer video dependencies and Control1/autostart/icon/daemon-example install contracts. Exact references and implications are in the report; the design remains unchanged.
- **W2/W3 work is concurrent and outside this phase:** `.1` owns selector/inventory/Python core; `.2` owns CMake/realization/runtime Python. Their changes are not integrated or accepted by this audit. No proposed command is promoted to current testing instructions.
- **W1 remaining / W11 NOT RUN:** all seven representative clean/incremental workload conditions, five samples per condition, registration/input completeness, cost/savings comparisons, 20 representative PR shadow comparisons, two successful broad replacement integration runs, release dry run, selected-skip/cancellation and merge-group acceptance, external artifact/manual consumers. Historical legacy runs and a tiny script-mode probe do not satisfy these gates.
- **W12 NOT RUN:** no hook/default/documented-command switch, old requirement removal, schedule removal, workflow rename or duplicate-trigger cleanup. Parent-directed integration verification must precede later docs/default work.
- **Environment limit:** `bd dolt pull` returned `fetch from origin/main: Error 1105: no remote`; local bead reads/updates succeeded. No direct tracking-file edits, remote sync workaround, destructive build, live MDK run, settings change, merge or push was performed. `.5` remains open for the later phase.

## 9. Phase 2 — native root QA, 2026-10-15 PDT

**Evidence:** [native root QA receipt](../reviews/dependency-aware-testing-ci-2026-10-15-native-root-qa.md), exact source `d9e8c6294a35f04d0c73870d4d27f28990759a24`, fast-forwarded from the integration branch into the assigned `.5` worktree. This includes the staged selector/realization corrections and upstream Signet fixture fix, but not acceptance of the still-concurrent `.3` local adapters or `.4` workflows/inventory.

- **Native root execution:** fresh throwaway build outside all worktrees; Debug/Ninja, all default targets, mandated `NIP55L_SECRET_BACKEND=libsecret` and `GROUNDHOG_STORE_KEY_DEFAULT_BACKEND=secret-service`, private HOME/XDG/session bus and test-only Secret Service. Configure/build/full serial CTest exited 0. Exact registration-to-JUnit accounting: **437 unique registrations, 432 passed, 5 skipped, 0 failed/errors/disabled**; 7 registrations are fixture setups, not additional logical check aliases. No retry/quarantine or production edit.
- **Skips remain gaps:** TCP endpoint/token absent for `test_nip5f_tcp`; existing Apple registration forces `GNOSTR_SKIP_UI_TESTS=1` for `signer/qr-scanner`, `signer/onboarding`, `signer/ui-layout`, `signer/ui-accessibility`. No GUI/live endpoint acceptance is inferred from CTest exit 0.
- **Scope/environment:** root default `BUILD_GROUNDHOG=OFF` was retained; Groundhog's backend argument was therefore unused and its tests were NOT RUN. Plain Homebrew `dbus-run-session` initially failed its launchd check-in before configure; an explicit private Unix-socket config then established verified Secret Service ownership. Before/after read-only keychain guards passed. Exact commands, times, environment, artifact hashes and retained preflight diagnostic are in the receipt.
- **Not selector/integrated acceptance:** parent-reported 112 Python tests and root/433-registration Groundhog zero-unowned probes are distinct evidence. Exact inventory membership/edge gaps still block audits; macOS native profile support remains pending `.4`. Final integrated Oracle/peer review is **DEFERRED** until parent steers after C/D integration; no routing retries were attempted.
- **No rollout advancement:** this single native sample is not the representative cost baseline, a broad replacement integration run or W11 shadow evidence. W11/W12 and `.5` remain open; defaults, protections, requirements and schedules are unchanged. Docs-only version assessment: **no bump**. No merge back or push.

## 10. Staged integration / final documentation receipt

**Historical snapshot:** `017039eea6620caea791162e1646982abf3d69f4`. **Bounded evidence:** [staged implementation receipt](../reviews/dependency-aware-testing-ci-2026-10-15-staged-acceptance.md). This supersedes the earlier phases' pending-interface/review status without rewriting their historical execution receipts. **No component bump:** documentation only.

### Implemented and verified, with distinct evidence provenance

- **Explicit commands exist:** `scripts/test-plan.py` and diagnostic `scripts/test-gate.sh` support planning/explain, exact-tree/private-snapshot operation, profile restriction, broad selection and retained evidence. Native `libgo.macos.asan` is standalone arm64 libgo ASan/UBSan, not root/Groundhog. Current command shapes/limitations are in [TESTING](../TESTING.md#dependency-aware-commands--explicit-diagnostics); `.5` reran all CLI helps and bounded explain/validation commands, not another build or broad source review.
- **Adapters/workflows are integrated, not cut over:** `NOSTRC_SELECTIVE_GATE=1` adds diagnostics to legacy local gates. Additive `dependency-tests.yml`/worker/broad families and `release-validation.yml` provide shadow/same-SHA routing and aggregation. Existing local defaults, broad verification requirements, overlapping legacy triggers and schedules remain through W11. Source presence is not hosted success or required-status acceptance.
- **Publication is staged:** `NOSTRC_DEPENDENCY_RELEASE_MODE` source fallback is `disabled`; `shadow` runs validation without adding a publication requirement; neither staged publication decision is a passing validation attestation. `enforce` requires successful broad/complete exact-source validation and applicable package results/artifacts. No variable/settings change or authorization to enforce before W11 is made here.
- **Review resolutions confirmed:** independent review `efd427e90` identified R1/R2. `e7cbf3cf6` binds caller checkout; `726f54fd5` adds actual-source identity and full consumer support-input audit; `b0a31b0ab` fixes build-path alias canonicalization. Fixture helper correction `d132bcc89` and EOF correction `db9bf4543` are integrated. Parent confirms **172/172 selection/regression tests passed, no skips, 34.193s**, all **6 relevant shell suites**, Actions syntax, all repository static checkers and committed-range whitespace passed at `017039eea`. The merged count is no longer pending; no new Oracle or broad review was attempted.
- **Native receipts are not interchangeable:** root compiled-source evidence at `d9e8c6294` remains **432 passed / 5 skipped / 0 failed**, Groundhog OFF; subsequent changes are tooling/workflows/regression fixtures. Selected native CLI evidence at `b0a31b0ab` actually executed **32/32 passed** in one CTest `-I`, about **9.14s**, with zero audit gaps. Nevertheless **CLI exit 1**, `job_conclusion=failure`, `complete_gate_eligible=false`: excluded `GoChannelStressTest` was built for header audit only and remains `not_executed`. **Both complete and partial aggregation reject.** This is execution proof, not a green lane or a completeness waiver; the prior alias failure remains preserved.

### Acceptance/ownership still open on existing beads

- `.1`/`.2`: complete ownership, exact registration/check membership, runtime/header/semantic-edge and supported-profile union; zero-unowned root/433-Groundhog probes and a narrow zero-gap native audit do not prove the full union.
- `.2`/`.3`: guarded non-CTest command/build-contract execution; unsupported paths remain visible refusals.
- `.3`/`.4`: real live-MDK cross-checkout image/driver/container lifecycle, cancellation/locks and hosted broad/release evidence; no fake/live acceptance equivalence.
- `.3`: central shared Groundhog sanitizer configuration/target/environment/input-audit parity; legacy selector/parser remains until proven replacement.
- `.2`/`.4`: complete native/compiler/architecture/optional variants; `.4` retains the known native stress execution gap, not an expected-pass exception.
- `.5`/epic: representative five-sample clean/incremental cost baseline, **20 PR / 2 broad integration runs**, release replacement/dry-run, skip/cancellation/merge-group and no-gap protection/cutover evidence. W12 default/trigger/schedule cleanup remains NOT RUN. `.6` separately tracks existing workflow ShellCheck cleanup.

All scoped beads remain open; no completion/savings/protection claim follows from the passing regression suite or from failed-lane native execution. Final docs pass is documentation-only; parent owns landing/push/branch cleanup after review. No merge back or push by `.5`.

## 11. Dated continuation — coherent integration, 2026-10-16 UTC

**Snapshot:** `9c03e77126d8f2f092b22bafbd79280fbaf10950`, fast-forward merged into the assigned documentation worktree. **Evidence:** [dated staged-acceptance continuation](../reviews/dependency-aware-testing-ci-2026-10-15-staged-acceptance.md#2026-10-16-continuation--integration-9c03e7712), [fresh native-root QA](../reviews/dependency-aware-testing-ci-2026-10-16-native-root-qa.md). Sections 8–10 remain historical: their native failures, aggregate rejections, counts and SHAs are not rewritten. This docs-only continuation requires **no component bump** and does not close the original scope beads.

### Implemented progress versus remaining acceptance

- **W2–W5 / `.7`: real contracts implemented.** The core backend executes real declared command/build-contract checks, with prerequisite builds, argv/cwd/source/input binding, capability/evidence checks, cancellation/nonzero handling and actual native miniature coverage. The outer local adapter still refuses non-CTest/unsupported capabilities; full production profile/input/semantic union and guarded integration remain open.
- **Native diagnostic eligibility is now truthful success, not completeness.** Actual `a21d25f7f` standalone libgo receipt: **32/32 passed**, no skips/failures, one CTest `-I`, 8.7923s, zero audit gaps, CLI exit 0 / `job_conclusion=success` / `diagnostic_eligible=true`. Partial aggregate passes (exit 0); **`complete_gate_eligible=false`**, complete aggregate fails (exit 1). Stress target is built for header audit only; `GoChannelStressTest` stays **`not_executed`**, never a stress pass. This newer receipt supersedes the current-status interpretation, not the actual failed `b0a31b0ab` receipt in §10.
- **Root active-language compiler fix `8db718cb1`: configure + resolve only.** Missing/ambiguous/incorrect active identities still fail closed. Root realization acceptance must not be expanded into root-ASan execution, shared sanitizer or full lane acceptance.
- **W7/W8 / `.8`–`.9`: faithful metadata, not execution.** Root CI ASan profiles retain actual configure/command/environment and configured registrations; shared Groundhog sanitizer metadata retains separate configure/build/test environments and configured membership. The observed poll-background consumer edge is retained. **Shared phase-aware sanitizer execution remains explicitly unsupported/refused** in core integration/outer/Linux shared routes; no legacy fallback is shared proof. Phase environment application/fingerprinting, input audits and actual ASan/UBSan/LSan execution acceptance remain open.
- **W6 / `.8`: historical standalone MDK 0.11 command/CTest smoke, not selector admission.** The prior helper configured/built and ran **5/5 CTests**, 0 skips/failures, about 12.91s, using Docker/private bus/throwaway Secret Service and passing keychain guards. Its old successful-cleanup claim is **superseded by F1 / `85a2e2ab`**: detached-tree cleanup was not proved, and the helper now always records `cleanup_complete=false` with nonzero lifecycle refusal after otherwise successful commands. No current successful full-lifecycle proof exists. Selector custody/finalization remains unsupported. The inherited-FD proposal failed actual CMake/Ninja/CTest traversal (`EBADF`, CTest exit 8); prototypes/negative probe remain archived/**not shipped**. No fallback/transport redesign or full 0.8/0.9/shared-sanitizer acceptance is implied.
- **Fresh compiled-source QA:** at `84ae0896b0abd835935c264b872e9b184d616364`, fresh native root configure/all-default-target build/full first serial CTest passed: **437 exact unique cases; 432 passed, 5 skipped, 0 failures/errors/disabled**. Receipt/JUnit/outcomes hashes were independently verified. Required private HOME/XDG/Unix bus/throwaway Secret Service and keychain guards passed. **Groundhog OFF** remains root QA, not GUI/live/profile union; no new run at `9c03e7712` is asserted.
- **`.11` remains unresolved.** Hosted sanitizer heartbeat failure is retained with original thresholds/exclusions unchanged. The root non-sanitized serial pass and bounded ASan/deterministic probes do not establish cause or authorize a runtime/test correction.

### Historical 9c03e7712 source verification and bead status

Parent confirms at clean `9c03e7712`: **244/244 Python tests, 0 skips, 58.163s** (`/tmp/nostrc-completion-final-selection.log`), and full actionlint including ShellCheck passes. B's `3a7c685e02ba` slice separately passed 244 tests, six shell suites and static checks. Parent now also confirms **all six shell suites passed** at `9c03e7712`: `test-sanitizer-gate.sh`, `test-full-gate.sh`, `test-linux-gate-lifecycle.sh`, `test-linux-gate-smoke.sh`, `test-pre-push.sh`, `test-test-gate.sh`; **all four static checkers and range whitespace passed**. The parent wrapper initially named nonexistent `test-linux-gate.sh`, then corrected its invocation to the real lifecycle suite; this was not a test failure/retry. Independent review was subsequently delivered as `24b7bb987` at `9c03e7712`; §12 records its source-fix resolution status, not a broad rereview. The B result is not substituted for parent verification. Limited docs verification at `9c03e7712`: all ten CLI helps, three plan-only explain commands and normal policy validation exit 0; strict policy validation intentionally exits 1 (`enforcement_ready=false`). Exact help/command receipts and hashes are in the staged continuation; no new build/test or Oracle review was run in this docs pass.

| Bead | Status and remaining acceptance |
|---|---|
| `.6` | **CLOSED** — existing workflow ShellCheck cleanup. |
| `.10` | **CLOSED** — declared CMake 3.22 inventory-export compatibility, old/current actual configure/export and regression evidence. |
| `.7` | **OPEN / in progress** — core contracts/diagnostic eligibility implemented; full profile/runtime/header/semantic union, stress gap and guarded capability acceptance remain. |
| `.8` | **OPEN / in progress** — chain-safe selector MDK custody/finalization, phase-aware shared sanitizer execution/audits and remaining actual matrix/concurrency/cancellation/hosted proof. |
| `.9` | **OPEN / in progress** — faithful configured membership/phase metadata integrated; complete executed workflow/profile/platform/optional/input union, root-ASan and native stress gaps remain. |
| `.11` | **OPEN** — causal evidence and deterministic correction/verification unresolved; original guard retained. |
| `.1`–`.5` / epic | Original scopes stay open; this continuation is not a completion attestation. |

**W1/W11/W12 unchanged:** representative five-sample cost baselines, **20 representative PRs / 2 broad integration runs**, exact release replacement/dry-run, selected-skip/cancellation/merge-group and no-gap protection/operator cutover proof remain unmet; W12 cleanup remains NOT RUN. No default, `enforce`, schedule, requirement or protection change is authorized before W11/operator approval. Parent owns remaining verification, independent review, integration/merge/push and cleanup. No sibling review file or production/runtime/helper/metadata source was edited here.

## 12. Post-review reconciliation — 2026-10-16, ae36f5a26

**Source:** `ae36f5a2639ff1c28d6657621b8afb0d81771b2b`, fast-forward merged for this documentation-only pass. [Review `24b7bb987`](../reviews/dependency-aware-testing-ci-2026-10-16-continuation-review.md) remains the historical independent review of `9c03e77126d8f2f092b22bafbd79280fbaf10950`; **F1–F3 are intact**. Its appended [resolution status](../reviews/dependency-aware-testing-ci-2026-10-16-continuation-review.md#resolution-status-after-source-fixes) and the [staged post-review continuation](../reviews/dependency-aware-testing-ci-2026-10-15-staged-acceptance.md#2026-10-16-post-review-continuation--ae36f5a26) distinguish fixes from rereview/acceptance. Parent source spot-checks occurred, **not a broad rereview**; no current review-pending or current-source approval claim is made.

| Fix | Closed scope and remaining limit |
|---|---|
| F1 / `.12` CLOSED — `85a2e2ab633e77157b2c8ede80449e68977ed0d4` | Eliminates false cleanup attestation: MDK helper **always** emits `cleanup_complete=false` / `complete=false`, arbitrary tree unverified. Otherwise successful commands retain separate success/exit fields but run returns nonzero `cleanup_unverified` (or `cleanup_failed` for scoped errors). Original command failure/cancellation and original-session/container results are retained. Real detached-descendant regressions prove refusal, not successful arbitrary-tree cleanup. **`.8` remains OPEN** for ownership/lifecycle/custody. |
| F2 / `.13` CLOSED — `83a38d879c82429b07dd627a36a64e52e9810c1b` | Rejects nested negative diagnostic eligibility even with outer duplicate absent; partial diagnostics cannot promote a lower-layer refusal or establish completeness. |
| F3 / `.14` CLOSED — `732346a87bf22df15ea1e11040b94f9e7eca01bc` | Raw byte logs/hash and lossless UTF-8/surrogateescape text; unexpected errors finalize failed receipts, with original failure/output and separate cleanup errors retained. Running checkpoints no longer claim success. Actual invalid-byte/exit/timeout/error regressions are retained miniature evidence, not native-profile/live/rollout acceptance. |

**Smoke/cleanup correction:** the prior live MDK 0.11 **5/5 is HISTORICAL command/CTest evidence only**. Its old successful-cleanup assertion is superseded; no current successful full-lifecycle proof or new live smoke exists. Selector custody/finalization and the failed/archived inherited-FD proposal remain unsupported, not shipped authority. Scoped cleanup attempts do not prove detached/reparented process-tree absence.

**Verification provenance:** A-branch core suite **251/251 passed** at `732346a87`, with static/compile/whitespace checks, separately from the parent combined suite. `.12` owner reports 16 focused MDK / 248 branch tests using fake Docker and real host processes/CTest. Parent confirms the combined run at **`ae36f5a2639ff1c28d6657621b8afb0d81771b2b`: 255/255 passed, 0 skips, 66.169s**, retained at `/tmp/nostrc-completion-reviewed-selection.log`; **all six shell suites, four static checkers, full actionlint including ShellCheck and whitespace passed**. Subsequent `4956fcf2` changes were docs only; no broad rereview or rollout approval is inferred. §11's 244-test/six-shell/static/actionlint results stay historical at `9c03e7712`. Root QA 432 passed / 5 skipped at `84ae0896b` and native libgo 32-case diagnostic evidence retain their original identities and coverage limits.

**Open work unchanged:** `.7`/`.8`/`.9`/`.11` and original scopes remain open; `.6`/`.10`/`.12`/`.13`/`.14` are closed only for their bounded work. Shared sanitizer execution remains refused; `.11` cause remains unresolved with original thresholds/exclusions retained. No default/enforce/threshold/schedule/protection change before W11/operator approval; W11/W12 and complete profile/input/ownership union remain unmet. **No component bump:** documentation/internal testing only. This pass changes only current docs and appends review resolution status, verifies links/whitespace, and leaves parent responsible for integration/push and remaining acceptance.

## 13. Bounded phase/adapter handoff — 2026-10-16, 2d64a7ac2

> **HISTORICAL snapshot; current status superseded by [§15](#15-final-focused-progress--2026-10-16-df2b23612).**
> Preserve the original receipts/negative results below at their recorded sources.

**Source:** `2d64a7ac22bcc08018d4ffdec91fee800f54df6f`, fast-forwarded into the assigned docs worktree. [Bounded phase acceptance](../reviews/dependency-aware-testing-ci-2026-10-15-staged-acceptance.md#2026-10-16-bounded-phase-acceptance--2d64a7ac2) records source-specific proof and remaining gates. This supersedes §§11–12's blanket execution refusals, **not their historical receipts**. No runtime/core/inventory/workflow changes; the historical phase review is preserved with an appended guard-only resolution. **No component bump** for internal testing/docs.

| Scope | Implemented / remaining acceptance |
|---|---|
| `.15` CLOSED / `7bd6dc4b4` | Actual separate configure/build/test environment application, source paths, fingerprints/cache validation and input audits, including all compatibility application targets. Real miniature execution is not the full production profile union. |
| `.19` CLOSED / `0b1e8705e` | Core raw pipe bytes survive actual SIGTERM; parent reports **24 Linux / 278 native tests passed**. This does not resolve the separate outer wrapper defect. |
| `.16` CLOSED / `c115cc711` | Bounded ordinary unwrapped command/build-contract execution and actual shared Docker configure/build/audit/test phases via explicit `NOSTRC_SANITIZER_SHARED_PROFILE`; no wrapped positive acceptance. Legacy default unchanged; unsupported capabilities and shared quarantine/retry overrides fail. Real native Linux/arm64 sanitized MINI **1/1**, not production Groundhog x86_64/full LSan. |
| `.17` CLOSED / `7e3745463` | **98 exact command inputs**, new tracked/discoverable-helper detection and **87-job accounting** include the final signal/adapter tests and shadow workers. Actual production Linux/x86_64 command **306 passed, 0 skips, 405.309s**, 99 same-SHA input hashes/no gaps; diagnostic eligible, complete false. |
| `.21` CLOSED / `853603b00` | All **ten authored profiles** route independent shadow jobs/conclusions, with distinct root Linux ASan, root macOS ASan and Groundhog sanitizer workers. **No hosted acceptance yet.** |
| `.22` CLOSED / `8179275b2` | R1 resolved **by prelaunch refusal only** for selected non-CTest command/build-contract lanes needing display/private bus, including mixed lanes; no dropped capabilities/fabricated receipts. Focused native/Linux **15/15** each, **30 refusal combinations**. Legacy CTest cancellation unchanged; no drain fix/rereview. |
| `.23` OPEN / `.24` OPEN | Full safe wrapped custody is not implemented; abandoned reserved-group draft left a stubborn wrapped CTest child alive, archived outside repo/not shipped. Original full-gate fixture invocation failed/lost trace; cause remains unknown and later diagnostic pass is not a causal fix. Forensics `967172f77` preserves failed private fixtures/logs plus command/line/phase/call stack, success cleanup unchanged; four injected negative exits **1/1/37/1** retained and B's fresh full-gate passed, not causal resolution. |

**Verification, with separate provenance:** parent native combined **306/306 passed, 0 skips, 146.958s** at `90a1b675f` (`/tmp/nostrc-phase-combined-tests.log`); production `static.policy` command **306 passed, 0 skips, 405.309s** at `7e3745463`, same-SHA post-input audit **99 hashes / `gaps=[]`**, clean source, `diagnostic_eligible=true` / `complete_gate_eligible=false`. Parent confirms six shell suites/four static checkers/full actionlint including ShellCheck/range whitespace passed at `83dfea3bb`. The review's **304 passed / 2 metadata errors** stays historical at that SHA. `.15`/`.16`/`.17`/`.18`/`.19`/`.21` are closed for bounded work, **not hosted acceptance**; `.20` closed as a duplicate, not an implemented fix. `.22` closes only the unsafe admission path by refusal; the original adversary is not invalidated. Parent final combined at `2d64a7ac22bcc08018d4ffdec91fee800f54df6f`: **307/307 passed, 0 skips, 125.303s**, `/tmp/nostrc-phase-final-selection.log`; only docs/forensics changed afterward. Parent confirms at `97e393cba`: **all six shell suites, four static checkers, full actionlint including ShellCheck and range whitespace passed** (`/tmp/nostrc-phase-final-test-*.log`); no final full-slice acceptance. MDK helper remains `cleanup_complete=false` with nonzero `cleanup_unverified` after otherwise successful commands; custody/arbitrary-tree cleanup is not accepted.

**Native/hosted evidence stays separate:** fresh root configure/all-default-target **BUILD ONLY** at `7fdadd251` passed, external private build/caches and unchanged keychain guards, **Groundhog OFF**. No compiled sources/CMake changed since prior 437-case QA, so no new CTest was requested/run; its **432 passed / 5 skipped** receipt keeps its old SHA. Historical hosted `33a101276` root ASan was Linux **266 passed / 1 skipped**, macOS **267 passed / 1 skipped**, while Debian found no tests. Heartbeat passed those samples but `.11` cause remains unresolved; static cancellation/shadow-routing failures led to `.19`/`.21`, not fresh hosted acceptance.

**Rollout unchanged:** `.7`/`.8`/`.9`, original scopes and `.11` remain open for complete ownership/runtime/header/semantic/profile union, production sanitizer/MDK/hosted evidence and causal heartbeat diagnosis. W11 still requires representative five-sample costs, **20 PR / 2 broad integration runs**, release replacement/dry-run, selected-skip/cancellation/merge-group and no-gap protection/operator proof; W12 cleanup is NOT RUN. No default, `enforce`, threshold/exclusion, requirement/protection, trigger or schedule change. Parent owns final verification/fix integration and landing; no merge back or push by this docs task.

## 14. Contained custody/retention progress — 2026-10-16, final evidence pending

> **HISTORICAL / superseded pending snapshot**, including the 49,158-findings
> audit. [§15](#15-final-focused-progress--2026-10-16-df2b23612) records final
> A/B/native evidence and completed parent gates; the old failed audits stay failed.

**Documentation base:** `7616080a34aa5fd6f19b173b3328d5e350704be1`.
[Source-grounded checkpoint](../reviews/dependency-aware-testing-ci-2026-10-15-staged-acceptance.md#2026-10-16-contained-custody-and-retention-checkpoint--final-evidence-pending)
records A `a9d6c8caa`, B `ab5cdbf05`, C review fixes and actual parent forensics.
Earlier §§8–13 counts/statuses/receipts remain historical, not rewritten. This
section supersedes blanket current refusals only for the new contained Linux
route; it does not claim full wrapped/native, profile-union or hosted acceptance.

| Execution-index scope | Latest bounded progress / remaining acceptance |
|---|---|
| W5–W6 / contained route | Explicit `contained-gate.py prepare` / `run`, attested locked 08/09/011 builds, local-only `groundhog.linux.arm64.prepared011`, inspected CID lifecycle and post-exit retained membership/byte verification. Actual escaped TERM-ignoring writer deletion/overwrite yields negative diagnostic/partial aggregate with cleanup independently true. No arbitrary native-tree claim. |
| W4–W7 / prepared admission | Revalidated earlier peer builds, not fresh builds. Raw 011 **18/18**, 08 **3/3**, 09 expected incompatibility remain consumer-only evidence. Historical full-root **20,977** findings still record the blocked attempt; no current rerun inferred. Original Docker image-build registrations remain `not_executed`. Optional selected-closure diagnostic **DEFERRED, not implemented**; full-root audit remains mandatory, no absence-of-false-negatives proof. |
| W4–W7 / sanitizer ownership | At B `ab5cdbf05`, selected primary/fixture closure semantic findings **zero**, full audit **FAILS: 49,158 = 347 unclaimed registrations + 48,811 nonmember semantic findings**. B is actively fixing nonmember metadata; await parent final checkpoint before new counts. No exception or profile acceptance. |
| W6 / `.23` and legacy MDK | Native arbitrary-tree custody remains OPEN/unsupported. Legacy `mdk-gate.py` unchanged: `cleanup_complete=false`, otherwise successful commands return nonzero `cleanup_unverified`. Container-bounded lifecycle does not promote legacy cleanup. |
| W5–W6 / original phase failures | A **348/348** selection regressions and **13 actual custody cases**, zero skips, at `a9d6c8caa`; six shell suites/four statics/full actionlint passed. Original two FIFO readiness causes **unknown** despite later passes; new raw child/exit evidence retention keeps original guards. Separate traced Git detached-maintenance race fixed at `60cd24c5c`, no global settings change or FIFO-cause inference. |
| W6 / `.24` retention | C `d1c20feb4` removes generic native deadlines; fixed cooperative timeout reaps only its known child, unsupported timeout commands refuse before launch. Parent actual forensics **8/8**. `.24` CLOSED **for retention only**, not the unknown original cause or generic native cleanup. |
| W7 / `.11` | `23ca21c19` restores 50 unpaced store cycles, original strict >50ms / <=2 missed guard and >5 real callbacks; burst-sensitive control fails with 49 misses. Native plain/native ASan/Linux-arm64 ASan bounded cases pass; historical hosted heartbeat cause remains unknown, `.11` OPEN. |
| W8–W9 / broad and Homed | Source-branch commits `1cc6af107`, `7b77046a0`, `20810a780`: pinned Blueprint/prerequisites/metrics/corpus setup, refused-rename unit correction, five registered Homed units with exact zero-test-refusing selection. Parent reports native **5/5**; not distro/systemd/FUSE or hosted broad acceptance. Final integration/fingerprints belong to parent/B. |
| W11 | Read-only audit at hosted `8a031f443`: voice LSan 120B with external libsrt-gnutls stack; shadow failed. **NOT MET: 0/20 accepted PR comparisons, 0/2 successful broad integrations**, representative five-sample costs, exact release/package/dry-run and no-gap protection/operator proof unmet. Local source repairs do not retroactively pass hosted runs. |
| W12 | **NOT RUN**. No default, `enforce`, threshold/exclusion, trigger/schedule, required-check or protection changes. |

**Focused follow-up pending:** parent QA513CA is running fresh full native QA at
`7616080a3`; no result claimed here. Parent will supply final B metadata audit /
source identity and integration evidence for one focused documentation follow-up.
The old root QA counts are not reused as that result. Temporary evidence paths
and verified hashes are in the staged receipt, not durable hosted acceptance.
**No component bump:** this docs pass and cited internal/test-only fixes add no
shipped API/artifact change. No code, tracker or settings edits, Oracle, push or
merge by this task; parent owns remaining integration/acceptance/landing.

## 15. Final focused progress — 2026-10-16, df2b23612

**Parent integrated source:** `df2b236124254450ac1645146fd0e3a4958a53c7`, not
master. [Final focused evidence](../reviews/dependency-aware-testing-ci-2026-10-15-staged-acceptance.md#2026-10-16-final-focused-checkpoint--df2b23612)
supersedes §14's pending A/B/native status only; prior SHAs, counts and failed
audits/wrappers remain unchanged. **Final parent gates completed at unchanged
`df2b23612`: selection 382/382, zero skips, 104.637s; actual Docker custody
13/13, zero skips, 12.771s; all six shell suites, four statics, full actionlint
including ShellCheck and range whitespace PASS**, no new failures/waivers.
Logs `/tmp/nostrc-final-reviewed-*`; exact suite names are in the staged receipt.
These integrated-source results remain distinct from A/B/native and hosted proof.

| Index scope | Completed bounded progress / still-required acceptance |
|---|---|
| W4–W5 / B metadata | At `8d052ca84`, **412** registrations have zero missing semantic/runtime/header edges; **357/357** selection tests pass. Original arm64/parallel-4 full audit still **FAILS: 347 unclaimed names**. No final-source or authored x86_64 production admission. **83/87** workflow-job mappings remain explicitly unsupported. |
| W5–W7 / exact diagnostic admission | `919b00173` allows **only `groundhog.linux.sanitizers`**, exact **347 reviewed nonmembers + 65 required fixture closure = 412** fresh registered partition and source/profile/context/export/build/audit binding. Only unclaimed membership is removed; all omitted source/Ninja/header/runtime inputs and required omitted targets remain audited/built. Omissions stay `not_executed`, complete/enforcement false; nested/complete/release promotion rejects. No prepared/arm64 automatic admission or selected-only fallback. |
| W5–W7 / actual review fixes | `995be8d9e` pins authored **parallelism 2 / timeout 2700s**, including explicit shared-route jobs. Confirmed P2 at `ac222f1c6` allowed rehashed Release receipt versus Debug retained phase; **`db9eb03a6`** independently checks declared profile, resolved/backend and retained phase configuration, with aggregate phase/profile composition. Original contradictory input and consistent Release-phase rewrite both now refuse; no broad rereview. |
| W5–W6 / exact A QA | At **`db9eb03a6`**, **382/382** selection tests, **13 actual Docker custody cases**, no skips; six shell suites/four statics/actionlint including ShellCheck/whitespace pass. Real miniature configure/build/full audit/CTest and promotion/configuration adversaries establish contract behavior, not production execution or full-root runtime/union. |
| W8–W9 / BP | `139aaa185` / `adf13917e` (A equivalents `b8975d3d2` / `bf3b5a0ad`) repair report layout/discovery; **14 unchanged actual regular workloads + 14 real report rows**, seven reporting regressions bound to `selection.contract` at `ac222f1c6`. **`.26` CLOSED**, not extended/full broad/W11 cost acceptance. |
| W8–W9 / Homed and compiler | **Five Homed units pass native and Linux** with mandatory exact listing/negative controls; **`.27` CLOSED**. Group test matches preexisting refuse-rename security; no runtime change or four-distro/systemd/FUSE acceptance. **`.25` CLOSED for compiler defect only**; **`.28` Adw build / `.29` JSON-GLib leak OPEN**, no source fix/waiver claimed. |
| Compiled root QA | Separate **`a47807050`** fresh configure/all-default build/full serial **437 CTests: 432 pass / 5 existing skips / 0 fail**; phases/wrapper **0**, owned child reaped, bus/socket gone, original-HOME keychain guards **0/0**. Groundhog/Homed opt-ins OFF; not sanitizer/profile-union/arbitrary-tree proof. Previous `7616080a3` wrapper **143 stays failed**. |
| W11–W12 / hosted | Feature branch pushed, master unchanged at checkpoint; nonpublishing **38873978927** at older `a47807050` remains **QUEUED**, not acceptance. **W11 NOT MET: 0/20 PR, 0/2 broad, cost/release/protection/operator proof unmet; W12 NOT RUN**. Prior `8a031f443` voice/shadow failures unchanged. |

**Unchanged boundaries:** native arbitrary-tree `.23` OPEN, legacy MDK
`cleanup_unverified`, optional prepared partial diagnostic **DEFERRED**; `.24`
closed for retention only, original FIFO/heartbeat causes remain unknown.
Prepared raw consumer receipts are not refreshed or promoted by sanitizer-only
accounting. No general default/enforce/schedule/required-check/protection changes.
No component bump: internal tooling/tests/docs only. Parent owns final gates,
source-specific acceptance and landing; no source/tracker/settings edits, broad
Oracle/review, push or merge by this docs task.

## 16. Migration progress — 2026-10-18, 6299f003

**Reviewed compiled source:** `6299f003399d0b99f269289baf88242e3340246d`.
[Current source-specific receipt](../reviews/dependency-aware-testing-ci-2026-10-15-staged-acceptance.md#2026-10-18-migration-checkpoint--6299f003)
supersedes earlier pending statuses only; historical failures and their original
counts/SHAs remain intact. This progress index organizes the original detailed
specification, not a substitute for its completion criteria.

| Scope | Completed bounded work | Remaining requirement |
|---|---|---|
| W5–W7 / `.30` CLOSED | Admitted CLI negative receipts retain exact lane/raw audits/logs/status; invalid admission still refuses. Configure diagnostics, unexpected exceptions and repeated-SIGTERM reporting corrections verified. | Negative evidence never qualifies as complete/release coverage; no hosted success inferred. |
| W5–W7 / `.33` CLOSED | Authored build/test jobs, legacy fallback and explicit phase flags; physical build-contract argv/global command identity and canonical nested context/hash validated. Parent 24 phase regressions pass. | Default-generator/all-build attestation `.34` remains unsupported; do not fake an `all` codemodel target. |
| W4–W7 / `.31` / `.9` OPEN | Worker libpeas-2 and ordinary/live MDK011 routes repaired. Grotto native/ASan build2/test1 projections, fresh raw arm64 56/56 and 15/15 names, 191 exact contract inputs, 13 profiles/152 checks/89 workflow hashes. | No authored x86_64 selector/whole-job acceptance; all 89 mappings retain unsupported union reasons. Core exclusions/new Gtk registration, full builds, Go/nak baseline, Meson/packages/shell-extension and ubuntu-latest/TSan remain gaps. |
| W4–W9 / `.28` / `.32` | Gtk.Spinner compatibility and build-tree-only shared resources; current-owned-output lookup, GLib dependency discovery and configured compiler/generator/make-program. Both installed contracts retain 57 entries. Source guard unchanged. | `.28` / `.32` CLOSED: exact6299 fresh full-root Groundhog-ON builds pass native and strict-read-only Linux/arm64, 4,770 tracked hashes unchanged and 57 installed resource paths verified; no CTest in that separate probe. Meson writes are separately `nostrc-ql5j0`; sanitizer/hosted acceptance is not inferred. |
| Final local gates | At `6299f003`: 437/437 selection, 13/13 actual custody; six shell suites/four statics/actionlint with ShellCheck/range whitespace all 0. Fresh native default configure/build 0 and awake 439 CTests: 434 pass / 5 existing skips / 0 fail; wrapper/cleanup/keychain 0. | Default native Groundhog/Homed opt-ins OFF, no production sanitizer/profile-union/W11 acceptance. Preserve first 422 pass / 5 skip / 12 timeout wrapper-8 run and actual host-sleep evidence. |
| W11–W12 | Independent source reviews corrected concrete receipt/phase/resource issues; final actual-diff Oracle findings corrected, not waived. | W11 NOT MET; W12 NOT RUN. Old `249f9657` hosted run38878691465 failed; run38873978927 canceled, neither accepted. No new accepted PR/broad/cost/release/protection/operator proof. |

**Unchanged policy:** only exact reviewed Groundhog sanitizer diagnostic
nonmembers remain authorized. Core/Grotto extension has no approval from the
unanswered optional prompt. No selective-default/enforcement, legacy trigger/
schedule/required-check/protection or release-publisher-default cutover. Original
`.1`–`.5` / `.7`–`.9` acceptance, `.23` native custody, `.29` JSON-GLib and
legacy MDK `cleanup_unverified` remain distinct open scopes; prepared partial
diagnostic remains deferred. Never close those scopes from these finite fixes.

**Versions:** internal CLI/CI/test/doc work is no-bump. Compatible shipped
resource/UI build fixes fold into already intended final Gnostr 0.1.0 and
unreleased nostr-gtk 1.2.0, not a second increment; published columns/tags unchanged.
Parent owns final authoritative tracker export, push and clean merged-branch/
worktree removal, preserving unrelated main-checkout changes.

## 17. Acceptance reconciliation and run ledger — 2026-10-18

The [criterion-by-criterion audit](../reviews/dependency-aware-testing-ci-2026-10-18-acceptance-audit.md)
reconciles `.1`–`.5` against their existing contracts. The [canonical run ledger](../reviews/dependency-aware-testing-ci-2026-10-18-run-ledger.md)
replaces assumed counts with a timestamped API/artifact-backed snapshot. It is
not a live dashboard or another production execution receipt.

At audited implementation `2743f1735`, 437/437 selection cases and six shell
suites/four statics/actionlint pass. GitHub backfill found six shadow attempts,
five failed nested broad accounting jobs and one prelaunch selection failure;
146 repository runs in the queried window include no PR events. All 59 selected
small artifact archive digests match. **Verified qualifying counts: 0/20 PR
comparisons and 0/2 successful broad integrations.** Do not count pushes,
standalone native tests, nested jobs or diagnostic receipts toward those totals.

`.1` is CLOSED for its finite logical contract after source/test evidence and
independent acceptance review; `.2`–`.5` stay open for the audit's named root, custody, CI/release and operational
requirements. No original scope is waived. The latest broad call still fails
JSON fuzz and skips its coverage report; actual selected workers also retain
Debug-configuration, registration, semantic/runtime and custody gaps.

The owning migration session must refresh the ledger and `.5` before reporting
progress or proposing acceptance. W11 NOT MET / W12 NOT RUN; no approval,
requirement, trigger/schedule, default or release-mode cutover. This pass changes
only evidence/documentation/tracker state; no component bump.
