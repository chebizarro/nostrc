# Dependency-aware testing/CI plan — focused critique

## Context / Scope

One bounded review on `nostrc` **master** (observed HEAD `7f1d0f708`), not whitenoise-ios. Read the complete 1,024-line plan and complete 1,097-line original export using `read_file`:

- Plan: `docs/plans/dependency-aware-testing-ci-2026-10-15.md` (references below abbreviated **P**).
- Preservation baseline: `prompt-exports/oracle-plan-2026-10-15-020001-plan-dependency-awar-e83e.md`, **only the generated response after `### Response` at line 142** (abbreviated **E**). Its composed prompt/selection is not a preservation requirement.

Focused source checks covered the named MDK linkage/runtime, standalone libgo, CTest, and release-applicability seams. No builds, live compatibility runs, tracker commands, or repository-setting changes were performed. This report is the only authored change; pre-existing plan, investigation and submodule changes were left alone. This is a design critique, not implementation acceptance or a comprehensive code audit. No version bump: review documentation only.

## Preservation and intentional corrections

**No supported implementation-bearing baseline requirement was found silently removed or generalized away.** The significant differences are explicit replacements/additions, not a reason to restore the original text wholesale:

- **Optional coverage:** P:309–321 replaces E:407–425's unauthorized new optional-component lanes with inventory of existing supported profiles, explicit uncovered-component records, fail-broad selection, and prohibition on claiming unrelated broad tests validate an uncovered component. This preserves inventory/accountability without pretending to close gaps or authorizing new CI support. W4 must retain those gap records in generated coverage reporting.
- **Previously unsupported tags:** P:718–733 correctly replaces E:818–832's expanded source-archive publication with validation-only handling and `publisher=none`. Existing publisher ownership is retained. The missing dispatch details are finding F4, not grounds to restore new publication.
- **Standalone libgo/root fuzz distinction:** retained correctly (P:36, 98, 676, 790). Verified `.github/workflows/libgo-ci.yml:85–125` configures from `libgo`, while `:232–252` configures the repository root and runs `fuzz_event_driver`. Do not collapse these profiles. Bootstrap remains underspecified (F2).
- **Dirty snapshots, complete realization auditing, broad evidence before refinement, per-case MDK deduplication, execution index and safety:** P:359, 414, 541, 590–600, 933–950 strengthen the response. They do not authorize production-target refactors, new platforms, or new acceptance harnesses. F5/F6 identify identity/lifecycle details needed to realize those requirements.
- Classifying this prose plan as non-executing while explicitly classifying AGENTS/testing/version guidance (P:173–181) is a concrete replacement for E:277's vague Markdown exceptions, not a lost execution requirement.

Settled owner policy is sound and not reopened: affected expensive checks run premerge; unaffected broad checks run on master/releases/manual; no scheduled tier; unknown paths broaden; future implementation updates AGENTS.

## Findings — ranked

### F1 — P1, confirmed: the MDK golden case excludes a real legacy input; UI-case granularity also needs an explicit conservative boundary

**Plan:** P:543–548, 977; inherited from E:644–649, 1047. Related direction ambiguity: P:981 / E:1051.

**Evidence:** `gnome/groundhog/CMakeLists.txt:2500–2504` directly compiles `src/ui/gh-mls-copy.c` into **the legacy 0.8 harness**. The same source belongs to `groundhog-mls-ui` at `:2908–2915`; the adopted executable links that library at `:2582–2584`, and all adopted cases use that executable at `:2656–2677`. The same executable also acquires the voice target and voice-fixture runtime path later (`:3310–3315`), so the realization must include late-added target relationships. This is not inferred from filenames. The adopted GUI case constructs the invitations panel in-process (`tests/mls/test_mdk011_interop.c:1038–1059`); there is no basis here to substitute an assumed external application process for its linked UI target.

Consequently the `gh-mls-copy.c` golden row's “no automatic legacy headless selection” contradicts a known compiled input. The plan's mandatory target/input audit would either reject the mapping or require the omitted legacy check. More generally, assigning a linked monolithic harness source only to one GUI case conflicts with conservative target-level input coverage unless a justified, separately testable per-case refinement is specified.

**Correction:** make this golden case select the legacy 0.8 check and the adopted checks required by the conservative executable/input mapping. Preserve distinct headless/GUI executions and display requirements; do not split production targets to manufacture savings. Permit narrower case mappings only with explicit semantic ownership, audit rules and regression evidence—not solely because a CTest invocation has `--gui` or a different `-p` argument.

Also clarify “Poll UI”: `test-groundhog-poll-ui` links MLS UI (`CMakeLists.txt:2918–2922`), so changing MLS UI impacts the poll test. That does **not** establish the reverse rule that changing `src/ui/gh-poll-ui.c` selects prerequisite MLS UI suites. That file is compiled by the poll test/app (`:2918–2919,2930–2933`), while poll widgets `gh-create-poll-dialog.c`/`gh-poll-card.c` belong to the MLS UI library (`:2908–2911`). Give these distinct paths golden cases instead of using “Poll UI” as an ambiguous ownership node.

**Required test/order:** correct the W2/W4 goldens before using them to judge W5 audit correctness. Verify that the legacy directly compiled input cannot be suppressed by a case override.

### F2 — P2, design seam: File API bootstrap and standalone descriptor finalization lack an owner and phase contract

**Plan:** P:207–225, 397–410, 778–790, 941. Inherited from E:306–324 and its file-impact table.

The plan specifies what to combine, but not who creates the File API query, when replies become available, or how the root-independent libgo configuration includes/finalizes descriptors. Adding the helper only to repository-root CMake does not reach `cmake -S libgo`: libgo declares its own project and enables testing (`libgo/CMakeLists.txt:18–24,83–84`). A CMake finalizer can export descriptor data, but cannot consume a codemodel reply for the generation phase that has not completed yet.

**Correction:** W3 should assign the Python configure adapter ownership of a client-specific codemodel query in the actual profile build root **before configure/generate**, then consume and validate that generation's reply and CTest inventory **after CMake exits successfully**. Make the CMake helper descriptor-export-only at that stage. Specify guarded standalone libgo include/finalization so root inclusion and standalone inclusion each export exactly once. Missing/errored/stale replies fail realization; they are not an empty inventory. This needs no new build framework or CMake-minimum increase. See the [CMake 3.22 File API lifecycle](https://cmake.org/cmake/help/v3.22/manual/cmake-file-api.7.html).

**Required tests:** fresh empty build root, existing stale reply, failed configure, standalone libgo, root-with-libgo, and no duplicate descriptors. Make these W3 completion criteria, not a W8 discovery.

### F3 — P2, confirmed lifecycle gap: arbitrary sequential CTest batches do not preserve dependency ordering or accumulated fixture evidence

**Plan:** P:511–527, 943; inherited from E:612–628.

Fixture closure per batch and non-concurrent invocation are necessary but insufficient. A `DEPENDS` edge crossing batches loses CTest's within-invocation ordering unless the runner schedules batches accordingly. `DEPENDS` itself is ordering-only, not success propagation; do not silently upgrade it to a success fixture. More concretely, Groundhog's MDK011 fixture deletes the shared `vectors.jsonl` on setup (`gnome/groundhog/CMakeLists.txt:2590–2595`), and every case requires it (`:2673–2678`). Re-closing fixtures in each batch can erase prior batches' evidence. Reusing a JUnit filename also overwrites prior results. See [CTest DEPENDS](https://cmake.org/cmake/help/v3.22/prop_test/DEPENDS.html) and [CTest 3.22 options](https://cmake.org/cmake/help/v3.22/manual/ctest.1.html).

**Named simpler replacement:** keep exact names authoritative, but resolve them against the freshly enumerated registration to a **CTest `-I` selection file**, and execute one CTest invocation per profile/attempt. CMake 3.22 already supports this file interface; it avoids command-line length batching without changing test registration or the minimum version. Verify the resulting show-only names plus fixture/dependency closure against the resolved plan before execution; never persist numeric indices as stable test IDs. Give every invocation/attempt its own structured-result path and retain evidence before retry/reset.

**Required tests:** dependent tests whose names sort in reverse dependency order; multiple fixture setup/cleanup tests; a reset fixture writing an evidence sentinel; unique first-attempt/retry result files; setup failure causing explicit non-execution rather than a pass. Count fixture executions separately from deduplicated logical check IDs.

### F4 — P2, design seam: validation-only tags have no named event owner; legacy/global and dry-run identities are not mapped

**Plan:** P:604–613, 703–751, 948. Partly inherited; validation-only routing is newly necessary after the correct publishing-scope replacement.

The proposed entrypoint lists branch/PR/merge/manual events, while release validation is described as reusable by existing publishers. Neither identifies the push caller for `nostr-homed-v*`, `nostr-shell-extension-v*`, or other **publisher=none** components. Current main-release triggers enumerate only their existing prefixes (`.github/workflows/release.yml:9–21`); adding only a reusable callee cannot cause unsupported-prefix tag pushes to validate.

In addition, the component schema is described as manifest-canonical, but the existing publisher accepts legacy `v*` as `global` and supports non-tag manual dry runs (`release.yml:43–57`). `global` is not a manifest component (`VERSION_MANIFEST.md:12–30`); applying a universal component-declared-version comparison leaves legacy and synthetic dry-run identities undefined. Existing Groundhog publication already has its distinct tag/dry-run guard (`groundhog-release.yml:249–254`); preserve it rather than generalizing one publisher's rules to all callers.

**Correction:** name one validation-only tag push dispatcher (either the shared entrypoint with explicit tag routing or the validation workflow with a push wrapper). It must classify known component tags, fail unknown component-shaped tags, call broad exact-peeled-SHA validation, and have no publication permissions/path for publisher=none. Define `legacy/global` as an explicit release identity outside the manifest component map, including its version-validation rule. Define dry-run identity validation separately from a component release tag; neither may become an unknown-prefix/global packaging fallback. Keep package applicability and artifact ownership unchanged.

**Required tests/W10 dependency:** end-to-end event-routing simulations, not only `release-plan` unit tests: a previously unsupported known tag reaches validation and no publisher; unknown tag fails; legacy `v*` still reaches the existing owners; non-tag and component-tag dry runs never publish; annotated tags attest the peeled commit. These are required before W9 relies on release replacement coverage.

### F5 — P2, new requirement propagation gap: dirty identity stops at the logical-plan cache contract

**Plan:** P:218–224, 359, 461–470, 596–598.

The new snapshot clause correctly requires HEAD plus a dirty-input digest and includes it in the **plan** cache key. The generated realization identity still lists source SHA, policy/profile/configuration/tool identity only; execution receipt identity still lists actual source SHA. For two different working snapshots at the same HEAD, that tuple is identical even though generated sources or header dependencies may differ. “Source identity” in lifecycle prose should not leave each adapter to choose whether dirty content participates.

**Correction:** define one execution source identity: committed SHA for clean exact-tree runs, or HEAD plus snapshot-content digest for dirty runs. Carry it through realization/header-index keys, resolved plans, receipt matching and evidence directories. Compiled prerequisites may be reused only under existing content/configuration freshness checks; a digest mismatch must not reuse coverage/audit evidence. Define snapshot construction failure and unsupported untracked/submodule materialization as visible failure rather than omission. No dirty receipt can satisfy committed-tree validation.

**Required tests:** two dirty snapshots at identical HEAD with different header/generated inputs cannot share a realization/audit receipt; explicit `--head` ignores dirty content; cancellation/failure leaves the caller's index/worktree unchanged. Add these to W3/W5/W6 acceptance, not just selector cache tests.

### F6 — P2, confirmed shared-resource seam: MDK image identity and cancellation ownership are not covered by build/profile locks alone

**Plan:** P:541, 594–600. The baseline also only says to retain existing locks (E:691–697).

MDK Docker image tags are machine-global: 0.8 and 0.9 use fixed tags (`gnome/groundhog/CMakeLists.txt:2529–2533,2640–2645`); MDK011 explicitly documents checkout races and exposes `MDK011_INTEROP_IMAGE` (`:2604–2613`). `RESOURCE_LOCK groundhog-mdk011` (`:2670–2671`) coordinates tests within CTest, not independent build trees/processes. A private source snapshot, profile fingerprint or local volume lock does not by itself prevent another checkout rebuilding a driver tag between image setup and use. Deduplicating voice aliases does not solve that race.

**Correction:** explicitly own the MDK driver resource namespace in the execution adapter. Use snapshot/driver-content-specific tags or immutable image IDs for build-to-run binding; where fixed tags cannot be parameterized without separately scoped harness changes, hold a machine-wide image lifecycle lock through setup and all consumers. Track only this run's container IDs/labels for cancellation; never globally prune or kill sibling containers. Record image identity in evidence. This preserves existing local-peer behavior and introduces no public relay or new compatibility lane.

**Required tests:** two concurrent build roots with different driver inputs cannot consume each other's image; cancelling one releases its locks/containers and leaves the other running; a stale/incomplete image build cannot yield a success receipt. Resolve this in W6/expensive-family adaptation before W8 enables concurrent calls.

## Recommendations and design-changing questions

Fix F1's code-disproven golden expectation first. Settle the small contracts in F2–F6 before treating W3/W5/W10 as implementation-ready; retain the rest of the plan's accurate detail and invariants. Do not restore unauthorized optional lanes or source publication to solve any finding.

Only these implementation questions materially affect design/order:

1. **Case granularity:** accept the conservative per-executable MDK impact boundary now, or explicitly scope/prove a finer per-case refinement? Until proof exists, F1's conservative selection is required; a savings target cannot override it.
2. **Tag event ownership:** which named workflow owns validation-only tags, and what is the validation rule for retained legacy/global releases? This determines W10's caller graph and W9 release-coverage proof, not the already-settled publishing scope.
3. **Shared MDK resources:** can existing profiles bind driver runs to immutable/content-specific images, or must local execution serialize fixed tags across checkouts? This determines concurrency and cancellation acceptance before affected MDK becomes a default local gate.

No owner-policy, scheduling, platform-expansion, tracker or production-code changes are requested by this critique.

### Review-method note

Jev `find-lines` was used only to narrow named source seams (28 requests; 214,538 input tokens; reported cost $0.009009). Its broad DEPENDS query misleadingly favored CMake target `add_dependencies` sites; a focused literal property search and the official CTest docs were used instead. All load-bearing source claims above were checked with direct `read_file`/`file_search`. No relevant project-memory hit was found or relied upon.
