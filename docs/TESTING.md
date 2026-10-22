# Test Strategy & Suite Documentation

> **Staged dependency-aware tooling (updated 2026-10-19): implemented, not cut over.**
> Explicit commands and additive shadow workflows exist; existing local defaults,
> push requirements and schedules remain authoritative through W11. See the
> [staged acceptance receipt](reviews/dependency-aware-testing-ci-2026-10-15-staged-acceptance.md)
> and [plan/progress](plans/dependency-aware-testing-ci-2026-10-15.md). Current bounded
> source-specific results: [6299f003 checkpoint](reviews/dependency-aware-testing-ci-2026-10-15-staged-acceptance.md#2026-10-18-migration-checkpoint--6299f003).
> Acceptance decisions and run counts: [criterion audit](reviews/dependency-aware-testing-ci-2026-10-18-acceptance-audit.md)
> and [timestamped run ledger](reviews/dependency-aware-testing-ci-2026-10-18-run-ledger.md).
> The ledger snapshot at **2026-10-19T04:10:01.512997Z** verifies **0/20 PR
> comparisons / 0/2 successful broad integrations**: nine roots/attempts, seven
> failed nested broad accounting jobs, one unlaunched and one incomplete call.
> These are dated reviewed counts, not live counters or unit-test totals. Refresh
> before reporting later progress; subsequent pushes/PRs are outside that snapshot.
> `.1` is closed for the proven logical contract; `.2`–`.5` remain open for the
> audit's specific root, custody, CI/release and operational gaps.
> The older GNostr CI/nightly table and universal environment wording below remain
> historical guidance, not a current repository-wide execution guarantee. Extended
> BP is manual-only; MDK daily and Grotto fuzz weekly schedules still exist.

## Opt-in Linux custody diagnostic — 2026-10-22

The outer adapter keeps **`--linux-custody legacy` as the default**. On a matching
Linux host, an existing non-instrumented, unwrapped profile may explicitly request:

```sh
scripts/test-gate.sh --base "$BASE" --head "$HEAD_SHA" \
  --profile "$PROFILE" --linux-custody ptrace --evidence-dir "$EVIDENCE"
# Omitting --linux-custody, or using --linux-custody legacy, retains legacy behavior.
```

`PROFILE` must have `instrumentation=none` and no selected display/private-bus
wrappers. Instrumented/sanitizer profiles and wrapped CTest refuse **before lane
target launch**; ordinary Groundhog/Grotto profiles still require wrappers and
therefore cannot use this diagnostic. Host/architecture, capabilities, compiler,
source and full input-audit admission remain enforced; this flag is not a bypass.
Prepared routes still require `contained-gate.py`. No unavailable-pidfd fallback,
untraced-profile parity, hostile-code sandbox or remote/service-work custody.
Ptrace receipts retain `complete=false`, `complete_gate_eligible=false` and
`untraced_profile_parity=false`. macOS/legacy cleanup and MDK policy are unchanged;
`.23` remains OPEN/in progress for full safe cross-platform descendant custody.

[Combined integration receipt](reviews/dependency-aware-testing-ci-2026-10-18-acceptance-audit.md#combined-transportcustody-integration--2026-10-22):
code **`a4cfd1420`** passes **523/523 selector tests, zero skips**, six shell suites,
four statics, inventory validation, actionlint/ShellCheck and fresh safe native
configure/default-all build. Read-only keychain checks pass before/after; native
Groundhog OFF and libsecret/secret-service flags remain required. **Full native
CTest NOT RUN**, with no production apps/lib/nips changes. The first failed
combined run is retained; two authorized test-harness fixes, pinned `nostrdb`
initialization and source-proven `XPC_SERVICE_NAME=0` normalization precede the
single fresh full run. Full environment fingerprint/refusal guards are unchanged.
Custody's given **18 macOS / 32 Linux-arm64** results remain at **`5b20780d3`**, not
a fresh Linux run at the integration SHA. Later docs/export do not renew those
source identities. The membership checkpoint below retains its dated receipt;
only its pending custody/integration status is superseded here. No source-authority
renewal, hosted/W11/release/default credit, closure or shipped version bump.

## Bounded transport membership checkpoint — 2026-10-22

At `1ed8f087e6571207c132ae885dbe7ede43f18dfd` (base `53f2e822d`), six
headless relay/network registrations extend ordinary Groundhog coverage to
**27 primaries / 29 expected executions**, with only the exact selected/fixture
authority arrays renewed. All 33 libgo omissions and 193 source-review hashes per
profile remain unchanged. Fresh full audits still **FAIL**: Groundhog
**8,905 → 7,900** (exactly six unclaimed + 999 source findings removed, zero
added); Grotto **9,146 unchanged**. See the [source-bound transport receipt](reviews/dependency-aware-testing-ci-2026-10-18-acceptance-audit.md#headless-relaynetwork-transport-membership--2026-10-22).

Source-specific checks: **521/521 selector PASS, zero skips**; **37/37 focused
headless/subset PASS** (eight new membership cases); safe native default-all
configure/build and static checks PASS. Separate raw Linux/amd64 CTest at the
base passes **8/8, zero skips**, including two setups; **not authored gated
execution**. Full native CTest, Linux all-target/all-29-omitted-target builds,
production postbuild/header audits and selected-profile backend execution are
**NOT RUN**. `complete=false` and `complete_gate_eligible=false` remain; no hosted/W11/release credit,
source-authority renewal, default change or shipped version bump. Custody sibling
and combined gates remain pending a verified integration commit. This checkpoint
does not refresh the dated hosted ledger above or close `.2`/`.9`.

## Approved ordinary libgo subset — diagnostic accounting only

The exact 33 reviewed libgo registrations in each of `groundhog.linux` and
`grotto.linux` now have a separate source-bound diagnostic subset authority. This
accounts for tests built but intentionally outside those ordinary workflow filters;
it does not execute them or expand the workflow filters. Fresh x86 configure,
real target-closure builds and full input audits underpin the records. The original
Groundhog sanitizer full-partition policy is unchanged.

Only those matching unclaimed-registration findings can be reclassified. All other
registration/input/runtime/ownership gaps remain blocking; omitted targets remain
built/audited as required. Omission receipts are `not_executed`, `complete=false`
and `complete_gate_eligible=false`, including through adapters and nested receipt
validation. Independent genuinely complete broad validation retains its own route;
these incomplete ordinary receipts cannot be pasted into it as complete proof.

Use the existing explicit `scripts/test-gate.sh --profile groundhog.linux ...` or
`--profile grotto.linux ...` route with committed endpoints. It still refuses on
remaining full-audit findings. See the [source-bound admission and limits](reviews/dependency-aware-testing-ci-2026-10-18-acceptance-audit.md#approved-ordinary-libgo-subset-admission).
This is not local/CI default activation, W11 acceptance or a release-policy cutover.
For these ordinary subsets, backend `--phase test` refuses before CTest because no
split-phase build-custody extension is implemented; use `--phase all` to retain the
physical prerequisite-build proof. This restriction does not change legacy or
non-subset phases.

## Dependency-aware commands — explicit diagnostics

Set `BASE` and `HEAD_SHA` to the exact committed endpoints of the intended range.
These commands are implemented; successful planning is **not** executed coverage:

```sh
python3 scripts/test-plan.py --help
scripts/test-gate.sh --help
python3 scripts/test-plan.py explain --base "$BASE" --head "$HEAD_SHA" \
  --context local --profile libgo.macos.asan
scripts/test-gate.sh --base "$BASE" --head "$HEAD_SHA" \
  --profile libgo.macos.asan --explain
scripts/test-gate.sh --broad --explain
```

Explicit execution is available as `scripts/test-gate.sh --base "$BASE" --head
"$HEAD_SHA" --profile libgo.macos.asan`; `--broad` requests broad selection but a
`--profile` restriction is still a partial projection, never complete CI coverage.
Without `--head`, the adapter handles a private working snapshot; with `--head`,
it executes that committed tree, not dirty caller files. `--evidence-dir <dir>`
retains a unique run's plans/adapter/lane evidence; its default parent is the Git
common directory's `nostrc-test-gate`. `--explain` does not configure/build/test.
Missing capabilities/unsupported checks fail visibly. Do not bypass them to
manufacture acceptance or point a raw realization at another checkout.

The planner also implements `plan`, `validate`, `resolve`, `check-inputs`, `run`,
`aggregate`, and `release-plan`. Use each subcommand's `--help` for the exact flags:
backend `resolve`/`run` require `--plan <plan.json>` and `--build-dir <dir>`;
`resolve --configure --profile <id>` creates a fresh realization. `run --phase
{configure,build,test,all}` exposes actual phase execution; each phase applies its
own declared environment, source-path substitution and fingerprint. Input auditing
uses `check-inputs --build-dir <dir> --profile <id> --plan <plan.json>` (add
`--after-build` for discovered headers). These raw commands require the matching
admitted execution checkout/source identity and are not substitutes for legacy
push gates. Policy-only `python3 scripts/test-plan.py validate --inventory
tests/selection/inventory.json` currently exits 0 while reporting
`enforcement_ready=false`; adding `--strict` exits 1 for retained coverage gaps.

### Current native profile and evidence limits

`libgo.macos.asan` is **standalone libgo**, macOS **arm64**, clang, Debug,
ASan/UBSan, Ninja, `GOF_PORTABLE_CONTEXT=OFF`, warnings-as-errors; its effective
`ASAN_OPTIONS=detect_leaks=0` matches the legacy workflow (not LSan acceptance).
It maps 32 registrations across `libgo.channel` and `libgo.native_standalone`.
`GoChannelStressTest` is an explicit `.4` coverage gap: its executable is built
for header-input auditing only and remains **`not_executed`**.

The newer actual native receipt at backend
`a21d25f7fdba6b5cb13c44cc837f3763773eb211` records **32/32 passed**, no skips,
one CTest `-I` invocation, zero audit gaps, about **8.79 seconds**, **CLI exit 0**,
`job_conclusion=success`, `diagnostic_eligible=true`,
**`complete_gate_eligible=false`**. Diagnostic `aggregate --partial` exits 0 /
`passed=true`; complete aggregation still exits 1 / `passed=false`. This is a
successful **DIAGNOSTIC**, never complete CI/release acceptance or a stress pass.
The older failed-lane `b0a31b0ab` execution and aggregates remain historical facts
in the staged receipt; they are not relabeled as successful attempts.

The fresh native root [QA at `84ae0896b`](reviews/dependency-aware-testing-ci-2026-10-16-native-root-qa.md)
configured/built all default targets and ran full first-attempt serial CTest:
**437 registrations, 432 passed / 5 skipped / 0 failed**. Receipt/JUnit/outcome
hashes were independently verified. Groundhog was OFF; four Apple signer UI cases
and the unconfigured TCP case skipped. This is root compiled-source evidence, not
selector/profile-union, Groundhog, GUI/live-peer or installed-desktop acceptance;
it does not re-date the [older `d9e8c6294` run](reviews/dependency-aware-testing-ci-2026-10-15-native-root-qa.md).
Keep macOS application test backends/private HOME/XDG/Unix bus/throwaway Secret
Service isolation and read-only keychain guards as required by `AGENTS.md`; never
change developer keychain defaults/search lists. `.11`'s hosted sanitizer heartbeat
failure remains unresolved: the native root pass does not authorize a threshold
change, exclusion, retry waiver or runtime correction.

### Implemented phase/command contracts and remaining limits

- The core backend now executes declared real `command` and `build_contract`
  checks, including prerequisite builds, bound argv/cwd/source/input identity,
  capability checks, cancellation/nonzero status and required evidence. Fix
  `732346a87` preserves raw output bytes with lossless UTF-8/surrogateescape text
  decoding, finalizes failed receipts on unexpected execution errors, and retains
  the original failure separately from cleanup errors. Fix `83a38d879` rejects a
  negative diagnostic decision in either outer or nested receipt even when the
  outer duplicate is absent. Native miniature executions establish those
  contracts, not the complete production profile union. Signal-capture fix
  `0b1e8705e` additionally retains bytes across actual SIGTERM; parent reports
  **24 Linux / 278 native tests passed**, separately from pending combined gates.
  The outer `scripts/test-gate.sh` adapter admits ordinary unwrapped `command` /
  `build_contract` checks and legacy CTest lanes, with exact-tree/snapshot evidence.
  Guard `8179275b2` **refuses before launch** any selected non-CTest lane requiring
  `display`/`private_bus`, including mixed CTest/non-CTest selection and capabilities
  from either profile or check. `.22` closes **by refusal only**: no capability
  dropping, silent unwrapping or fabricated execution receipts. Other unsupported
  capabilities stay refused; legacy CTest cancellation is unchanged. Safe wrapped
  command custody is **not implemented** (`.23` OPEN), not a drain fix.
- Phase engine `7bd6dc4b4` actually applies separate configure/build/test
  environments, `{source_root}` paths, fingerprints/cache validation and input
  audits, including compatibility application prerequisites/targets. Earlier root
  active-language compiler evidence was **configure + resolve only**; the newer
  real miniature phase executions do not establish the production profile union.
- `core.linux.asan` / `core.macos.asan` model the actual root CI configure/command/
  environment and configured registration membership. `groundhog.linux.sanitizers`
  retains separate configure/build/test environments (not a flattened map).
  Adapter `c115cc711` adds an explicit actual phase route:

  ```sh
  NOSTRC_SANITIZER_SHARED_PROFILE=groundhog.linux.sanitizers \
    scripts/linux-gate.sh --sanitizers "$PWD"
  ```

  It checks exact workflow/profile parity and capabilities, then configures,
  builds all declared compatibility/application targets, audits discovered inputs,
  tests once and partially aggregates. Unsupported quarantine/retry overrides fail;
  no legacy fallback is shared proof, and the **legacy default is unchanged**.
  Real native Linux/arm64 Docker sanitized MINI **1/1** establishes bounded adapter
  execution, **not production x86_64 Groundhog/full ASan/LSan acceptance**.
- The prior MDK 0.11 Docker/private-bus/throwaway-Secret-Service smoke's **5/5
  CTests** and passing read-only keychain guards remain **historical command/CTest
  evidence only**. Review F1 proved the old successful-cleanup assertion false for
  detached descendants. Fix `85a2e2ab` **always** records `cleanup_complete=false`
  and `complete=false`; otherwise successful command phases return nonzero status
  `cleanup_unverified` (or `cleanup_failed` for explicit scoped errors). Original
  command status/exit and original-session/recorded-container results remain
  separate. Arbitrary detached/reparented-tree cleanup is **unverified**; there is
  **no current successful full-lifecycle proof**, and `.8` remains open. The old
  smoke does not establish selector custody/finalization, full 0.8/0.9 matrix,
  shared sanitizer or complete-gate acceptance. The inherited-FD custody proposal
  also failed actual CMake/Ninja/CTest traversal (`EBADF`); its unaccepted
  prototype/probe is archived outside the repository, **not shipped**. No
  environment/config fallback or new transport is authorized.

### Shadow CI and release staging

- `dependency-tests.yml` is the additive **Dependency tests (shadow)** entrypoint:
  unfiltered PR/merge-group/integration/tag/manual routing, reusable same-SHA
  workers, and the always-evaluated `Dependency gate`. It is **not an accepted
  required check**. `dependency-broad.yml` calls existing broad families, including
  MDK/fuzz/extended BP; `release-validation.yml` requests exact-SHA broad validation.
  Distinct worker routing `853603b00` now maps **all ten authored profiles** to
  independent jobs/conclusions, including both root ASan profiles and shared
  Groundhog sanitizers. Inventory accounts for **87 workflow jobs**. This is source
  routing/local-contract evidence, **not hosted acceptance**.
- `NOSTRC_SELECTIVE_GATE=1` adds explicit diagnostic execution to the existing
  pre-push/full-gate path. Static default checks and existing broad verification
  requirements remain unchanged; this is not a selective-default switch.
- Publisher source fallback for `NOSTRC_DEPENDENCY_RELEASE_MODE` is `disabled`.
  `disabled` does not request the added validation job; `shadow` requests it but
  does not make its result a publication requirement; both retain existing
  publisher identity/package safeguards. Their `staged_disabled`/`staged_shadow`
  publication decisions are **not validation attestations**. `enforce` requires
  successful exact-SHA broad/complete validation and every applicable package
  result/artifact. Do not enable it before W11/operator cutover. This variable
  controls publisher validation, not whether general shadow PR workflows exist.

### Current bounded handoff — 2d64a7ac2, 2026-10-16

> **HISTORICAL snapshot.** Current status is superseded by the
> [final focused checkpoint](#2026-10-16-final-focused-checkpoint--df2b23612).
> The source-specific results and negatives below are preserved, not relabeled.

Metadata `.17` records **98 exact `selection.contract` inputs**, detects new
tracked/discoverable helpers and accounts for the independent shadow jobs. Actual
production Ubuntu 24.04 Linux/x86_64 `static.policy` command execution at
`7e3745463`: **306 passed / 0 skips, 405.309s**, same-SHA post-input audit
**99 hashes / `gaps=[]`**, clean checkout, `diagnostic_eligible=true`,
**`complete_gate_eligible=false`**. Parent native combined suite at `90a1b675f`:
**306/306 passed / 0 skips, 146.958s**. Parent's six shell suites, four static
checkers, full actionlint including ShellCheck and range whitespace passed at
`83dfea3bb`; these are distinct-source receipts, not reruns by this docs task.

Integration `2d64a7ac22bcc08018d4ffdec91fee800f54df6f` includes the guard-only
resolution of [review R1](reviews/dependency-aware-testing-ci-2026-10-16-phase-review.md#resolution-status--8179275b2-guard-only):
`.22` CLOSED at `8179275b2`, with focused **15/15 native and actual Linux passes**
and **30 refusal combinations**. The original finding at `83dfea3bb` stays intact;
no rereview or positive wrapped acceptance is claimed. `.16` is CLOSED only for
bounded unwrapped commands and actual shared Docker phases; `.15`/`.17`/`.18`/
`.19`/`.21` are closed for their bounded scopes. `.20` closed as a duplicate,
**not an implemented fix**. The review's metadata errors and the 306-test results
above retain their original source identities.

**Still open:** `.23` owns full safe wrapped custody. An abandoned reserved-group
draft left a stubborn wrapped CTest child alive; it is archived outside the repo,
**not shipped**, not a drain/custody success. `.24` tracks the original full-gate
fixture invocation's failure/lost trace: a later diagnostic pass is not a causal
fix; the original cause remains unknown. Forensics `967172f77` preserves failed
private fixtures/logs plus command/line/phase/call stack; success cleanup is
unchanged. Four injected failures retained negative exits **1/1/37/1**; B's fresh
full-gate fixture passed, not a causal resolution of the original failure.

Parent final combined suite at `2d64a7ac22bcc08018d4ffdec91fee800f54df6f`:
**307/307 passed, 0 skips, 125.303s**, `/tmp/nostrc-phase-final-selection.log`.
Subsequent integration `97e393cba6c8aa3fb3bf076b3862a39482f0e656` adds only
documentation/failure forensics. Parent confirms at that source: **all six shell
suites, four static checkers, full actionlint including ShellCheck and range
whitespace passed**; logs `/tmp/nostrc-phase-final-test-*.log`. Earlier 306-test
native and production Linux proofs above remain source-specific; no full wrapped,
MDK, enforcement or W11 acceptance follows.

Fresh native root configure/all-default-target **build only** at `7fdadd251`
passed (CMake 4.3.2/Ninja 1.13.2/AppleClang 21, arm64). Logs/hashes and safety
checks are in `/tmp/nostrc-phase-native-build-evidence.json`, independently
parent-verified. No CTest rerun: no compiled sources/CMake changed since the prior
437-case QA. Groundhog stayed OFF; both required backend flags were supplied,
but its backend flag was unused. Read-only keychain guards passed unchanged;
external private build/cache paths preserved default `_build` and developer HOME.
This does not re-date the prior CTest receipt or establish GUI/live/W11 acceptance.

The dated hosted `33a101276` observations remain separate: root ASan **Linux
266 passed / 1 skipped**, **macOS 267 passed / 1 skipped**; Debian built but found
no tests. Heartbeat passed in those samples but `.11` cause remains unresolved.
Static cancellation and shadow routing failed, leading to `.19`/`.21`; these
historical jobs do not prove current workers or a clean current integration.
Full provenance/limits are in the [bounded phase acceptance section](reviews/dependency-aware-testing-ci-2026-10-15-staged-acceptance.md#2026-10-16-bounded-phase-acceptance--2d64a7ac2).
MDK custody/cleanup, complete ownership/runtime/header/semantic/profile union,
production sanitizer/hosted proof and W11/W12 remain unmet. No default/enforcement,
threshold, schedule, requirement or protection cutover before W11/operator approval.
**No component bump:** internal testing/documentation only.

### 2026-10-16 bounded custody checkpoint — final QA/metadata pending

> **HISTORICAL / superseded pending snapshot**, including the 49,158-findings
> audit below. See the [final focused checkpoint](#2026-10-16-final-focused-checkpoint--df2b23612)
> for current completed gates and final A/B/native evidence. Pending statements
> describe only this earlier checkpoint; its failed audits remain failed.

This dated update supersedes earlier *current-status* wording only; all earlier
counts, failures and source-specific receipts remain historical. The [detailed
checkpoint](reviews/dependency-aware-testing-ci-2026-10-15-staged-acceptance.md#2026-10-16-contained-custody-and-retention-checkpoint--final-evidence-pending)
records sources and hash anchors. Documentation base is `7616080a3`; parent
QA513CA's fresh full native QA at that source is **PENDING**, not a result.
B is actively repairing nonmember metadata; await the parent's final checkpoint
before quoting any replacement full-audit count.

**Explicit contained route, local only.** `scripts/contained-gate.py prepare`
builds locked native peers (08/09/011), binds binary/context/toolchain/image
receipts and verifies owned CID lifecycle. `run` admits the distinct
`groundhog.linux.arm64.prepared011` projection in a contained Linux namespace,
not legacy native wrapped custody or the original Docker image-build fixtures.
It is Linux/aarch64, GCC Debug, **not sanitizer coverage**. Its `contexts=[local]`
means CI/release omit it as `not_executed`; explicit incompatible requests refuse.
The original ten CI-eligible profiles retain their independent shadow routes.
Inspect exact interfaces without executing builds:

```sh
python3 scripts/contained-gate.py --help
python3 scripts/contained-gate.py prepare --help
python3 scripts/contained-gate.py run --help
```

For an intentional, separately provisioned local attempt, the implemented shape
is below. Variables denote exact committed source, fresh evidence destinations,
architecture-compatible images and actual preparation receipt/plan paths; these
are **not runnable acceptance receipts**. Initialize/verify required gitlinks;
`run --gitlink-source PATH=OBJECT_REPO` optionally supplies exact local objects.

```sh
python3 scripts/contained-gate.py prepare --source-root "$SOURCE" \
  --evidence-dir "$PREP_EVIDENCE" --version 011 \
  --builder-image "$BUILDER_IMAGE" --rust-image "$RUST_IMAGE"
python3 scripts/test-plan.py plan --repo "$SOURCE" --base "$BASE" \
  --head "$HEAD_SHA" --context local --mode broad \
  --profile groundhog.linux.arm64.prepared011 --output "$PLAN"
python3 scripts/contained-gate.py run --source-root "$SOURCE" \
  --evidence-dir "$RUN_EVIDENCE" --image "$LINUX_IMAGE" --plan "$PLAN" \
  --profile groundhog.linux.arm64.prepared011 --peer-receipt "$PEER011_RECEIPT"
```

The full-root input audit still blocks prepared-profile acceptance. Historical
raw 011 **18/18**, 08 **3/3**, and 09 **expected incompatibility** are separate
consumer evidence, not selector/profile/original-image-build success. Earlier
locked peer builds were revalidated, not rebuilt at the latest SHA. The optional
separately named selected-closure diagnostic is **DEFERRED / not implemented**;
no partial audit exception or selection false-negative proof is claimed.

At A `a9d6c8caa`, **348/348 selection regressions** and **13 actual Docker custody
cases** passed with no skips. Finalization seals retained membership/bytes and
revalidates after namespace exit; actual escaped-writer deletion/overwrite
adversaries refuse diagnostic/partial aggregation while retaining failed bytes.
This does not fix native arbitrary-tree `.23` or legacy `mdk-gate.py`'s nonzero
`cleanup_unverified`. The original two phase-FIFO causes remain **unknown** despite
later passes; `60cd24c5c` fixes a separately traced detached Git-maintenance race.
Parent forensics **8/8** supports `.24` closed **for retention only**, not original
cause/generic native deadlines. Burst test fix `23ca21c19` restores all 50 unpaced
cycles with the original strict >50ms / <=2 missed guard; `.11` stays open.

At B `ab5cdbf05`, selected sanitizer primary/fixture closure has **zero semantic
findings**, but the full audit **fails: 49,158 = 347 unclaimed registrations +
48,811 nonmember semantic findings**. These are checkpoint-specific findings,
not test results or final metadata totals. Broad/Homed commits `1cc6af107`,
`7b77046a0`, `20810a780` add prerequisites/pinned Blueprint, correct refused-rename
expectations and register/select five Homed units; the parent reports native
**5/5**, not the four-distro/systemd/FUSE/hosted proof.

The read-only hosted audit at `8a031f443` records voice LSan **120 bytes** with an
external libsrt-gnutls stack and failed shadow execution, not accepted integration.
**W11 NOT MET: 0/20 accepted PR comparisons, 0/2 successful broad integrations;
representative costs, release/protection/operator proof unmet. W12 NOT RUN.**
No default, enforcement, schedule, threshold/exclusion, required-check or
protection changes; **no component bump** for these internal tests/docs.

### 2026-10-16 final focused checkpoint — df2b23612, bounded scopes only

> Historical source-specific checkpoint; current progress is recorded below in
> the [2026-10-18 migration checkpoint](#2026-10-18-migration-checkpoint--6299f003).

Parent integration source **`df2b236124254450ac1645146fd0e3a4958a53c7`** is not
master. This dated section supersedes the preceding pending A/B/native status,
**not historical receipts**. The [final focused evidence](reviews/dependency-aware-testing-ci-2026-10-15-staged-acceptance.md#2026-10-16-final-focused-checkpoint--df2b23612)
records exact sources, review fixes and hashes. **Final parent gates completed at
unchanged `df2b23612`: selection 382/382, zero skips, 104.637s; actual Docker
custody 13/13, zero skips, 12.771s; all six shell suites** (`test-pre-push.sh`,
`test-full-gate.sh`, `test-linux-gate-lifecycle.sh`, `test-linux-gate-smoke.sh`,
`test-sanitizer-gate.sh`, `test-test-gate.sh`), **four statics, full actionlint including
ShellCheck and range whitespace PASS**. No new failures/waivers. Logs:
`/tmp/nostrc-final-reviewed-*`; these completed integrated-source gates are
separate from A's results below and are not global readiness or hosted acceptance.

- **A `db9eb03a6`: 382/382 selection tests, zero skips; 13 actual Docker custody
  cases, zero skips; six shell suites, four statics, full actionlint/ShellCheck
  and whitespace passed.** These are final A-source contract/custody regressions,
  not production sanitizer or full-root/runtime-union acceptance.
- **Exact nonmember diagnostic admission only for `groundhog.linux.sanitizers`:**
  fresh registration partition must equal **347 reviewed nonmembers + 65 required
  fixture closure = 412**. Only `unclaimed_registration` membership findings are
  removed for this bounded accounting; omitted source/Ninja/discovered-header/
  runtime inputs and required omitted build targets stay audited/built. Every
  omitted registration stays **`not_executed`**, complete/enforcement false;
  complete/release and nested promotion refuse. Authored **parallelism 2 /
  timeout 2700s** are required, including explicit shared-route job options.
  No automatic prepared011 or arm64 admission; no selected-only audit fallback.
- **Confirmed configuration P2 fixed at `db9eb03a6`:** backend/resolved/retained
  phase configuration must equal independently declared profile build type;
  aggregate independently checks phase/profile composition. The review's original
  rehashed Release-receipt/Debug-phase input now rejects, as does a consistent
  Release-phase rewrite against the Debug profile. This is a specific resolution,
  **not broad rereview/approval**. Real miniature configure/build/full audit/CTest
  and negative promotion/configuration controls are not a production receipt.
- **B `8d052ca84`:** all **412** configured registrations now have **zero missing
  semantic/runtime/header edges**, with **357/357** selection tests passed there.
  Its original arm64/parallel-4 diagnostic **still failed on 347 unclaimed names**;
  later accounting does not retroactively pass it or attest the final source.
  **83/87 workflow-job mappings remain explicitly unsupported**. Authored native
  Linux/x86_64 production execution/audit on final source is **not yet proved**.
- **Bounded broad repairs:** BP `139aaa185` / `adf13917e` repairs runner/reporting
  layout and selection-suite discovery: **14 unchanged actual regular workloads,
  14 real report rows**, seven reporting regressions included in
  `selection.contract`; `.26` closed. Homed **five units pass on native and
  Linux**, with mandatory exact listing/negative controls; `.27` closed, not
  four-distro/systemd/FUSE proof. Group expectations now match the preexisting
  refuse-rename security contract; no runtime change. `.25` closed for the
  LibFuzzer compiler defect only. **`.28` Adw build / `.29` JSON-GLib leak OPEN**;
  no source fix, suppression or waiver is implied.

**Fresh native QA is complete at its own source `a478070509569c26917dd5fb13a548b1f68eade9`:**
configure/all-default-target build/full serial CTest all exit **0**;
**437 registrations, 432 passed / 5 existing skips / 0 failures**. Corrected
wrapper/finalization **0**, direct owned child explicitly reaped, bus daemon and
socket absent, read-only original-HOME keychain guards **0/0**. Required safe
backend flags, private HOME/XDG/bus/Secret Service and loopback-only sandbox were
used. Skips are the unconfigured TCP case and four existing signer UI cases,
not passed coverage. **Groundhog and Homed opt-ins OFF**: no sanitizer, live peer,
rendered GUI, selector/profile-union or arbitrary-tree acceptance. The earlier
`7616080a3` wrapper **143 remains a failure**, not relabeled by this fresh run.
Report: `/private/tmp/nostrc-final-native-a478070.C6w5fP/report.md`, SHA-256
`9121379d5b3ff43e27ac5219547d484d0ca55eb90abaf9e4270e2dc407d70aaf`.

At this checkpoint the feature branch is pushed, master unchanged; nonpublishing
hosted run **38873978927** at older `a47807050` remains **QUEUED**, **not accepted
execution**. Prior hosted `8a031f443` voice/shadow failures and original FIFO /
heartbeat unknown causes remain. `.24` closed **for forensics retention only**;
native arbitrary-tree `.23`, legacy MDK `cleanup_unverified` unchanged; optional
prepared partial diagnostic **DEFERRED**. **W11 NOT MET: 0/20 PR comparisons,
0/2 broad integrations, cost/release/protection/operator proof unmet. W12 NOT RUN.**
No general default/enforce/schedule/protection changes. **No bump:** internal
tooling/test/doc changes only; this pass edits no source, tracker or settings.

### 2026-10-18 migration checkpoint — 6299f003

Reviewed compiled source **`6299f003399d0b99f269289baf88242e3340246d`** adds
admitted negative CLI receipts, independent authored build/test jobs, worker
libpeas-2/routing repairs, bounded Grotto native/ASan projections and clean
Gnostr/nostr-gtk CMake resources. Full details, original failures and remaining
acceptance: [current staged receipt](reviews/dependency-aware-testing-ci-2026-10-15-staged-acceptance.md#2026-10-18-migration-checkpoint--6299f003).

- `python3 scripts/test-plan.py run --help` documents `--build-parallel` and
  `--test-parallel`. These must match profile `build_parallelism` /
  `test_parallelism`; each falls back to legacy `parallelism`, then 1. They do
  not enable unsupported default-all builds or expand capability admission.
- Ordinary admitted CLI failures now retain `lane_failure_v1`, raw diagnostics,
  actual phase/process status and ineligible coverage. A negative receipt is not
  a passing worker; invalid plan/source/build-path admission still fails closed.
- **437/437 selection tests**, **13/13 actual custody tests**, all six shell
  contract/lifecycle suites, four statics, full actionlint/ShellCheck and range
  whitespace pass at the exact source. These are not production profile-union,
  sanitizer or release acceptance.
- Fresh native default configure/all-target build pass. Awake full serial CTest
  on Oct 18: **439 total / 434 passed / 5 existing skips / 0 failed**; wrapper,
  cleanup and original-HOME read-only keychain guards **0**. Groundhog/Homed
  opt-ins OFF. Five skip names and exact commands are in the staged receipt.
- The first full run remains **422 pass / 5 skip / 12 timeouts**, wrapper **8**,
  with actual overlapping clamshell/maintenance sleep evidence. The separate
  awake attempt changes neither source, timeouts nor selection; it does not
  erase that failure or prove a distinct cause for every timeout.
- Raw Linux/aarch64 Grotto native **56/56**, ASan **15/15** and resource results
  are bounded diagnostics, not authored x86_64 or whole-job acceptance. Exact
  configured compiler/generator/make-program and actual GLib manifest inputs
  now drive resource CTests; active/retired generated lookup and installs retain
  the **6 library + 51 application** resource paths.

Only the existing exact Groundhog sanitizer nonmember policy is authorized.
Core/Grotto extensions remain unapproved; new registration/full-build coverage
and workflow variants remain gaps. **`.28` / `.30` / `.32` / `.33` CLOSED for finite
repairs; `.31` / `.9` OPEN, `.34` default-all attestation OPEN.** Exact-source
`.32` fresh full-root Groundhog-ON builds also pass native and strict-read-only
Linux/arm64, with unchanged 4,770 tracked hashes and all 57 installed resource
paths. That separate build proof runs no CTest; details are in the current receipt,
not inferred from a resource fixture. **W11 NOT MET; W12 NOT RUN.** Legacy local
requirements/jobs/schedules/protections and disabled-by-default release publisher
remain unchanged. Internal tooling is no-bump; compatible shipped UI/build fixes
fold into intended Gnostr **0.1.0** and unreleased nostr-gtk **1.2.0**, with no
published release metadata changes.

### Historical post-review verification

At integration `ae36f5a2639ff1c28d6657621b8afb0d81771b2b`, `.6` (workflow
ShellCheck), `.10` (CMake 3.22 inventory compatibility), and review-fix beads `.12`
(false cleanup attestation), `.13` (nested negative diagnostics), `.14` (raw command
output/failed-receipt finalization) are closed. `.12` closes only the false claim,
not arbitrary-tree ownership: `.7`/`.8`/`.9` remain open/in progress and `.11` remains open. Existing `.1`–`.5`
scopes are not closed by this handoff. Remaining acceptance includes complete
ownership/runtime/header/semantic/profile union, guarded MDK custody/finalization,
phase-aware shared sanitizer execution and W11/W12 evidence. No default/enforcement,
requirement/protection, trigger or schedule change is authorized before W11/operator
cutover.

Historical B slice `3a7c685e02ba` passed **244 tests, six shell suites and static
checks**. Parent confirmed the historical combined **244/244 tests, 0 skips,
58.163s** at `9c03e7712`
(`/tmp/nostrc-completion-final-selection.log`) and full actionlint including
ShellCheck passed. Parent also confirms all six shell suites passed at that SHA:
`test-sanitizer-gate.sh`, `test-full-gate.sh`, `test-linux-gate-lifecycle.sh`,
`test-linux-gate-smoke.sh`, `test-pre-push.sh`, `test-test-gate.sh`; all four static
checkers and range whitespace passed. These are final combined results, distinct
from B's earlier pass. The delivered [source review at `9c03e7712`](reviews/dependency-aware-testing-ci-2026-10-16-continuation-review.md)
(`24b7bb987`) retains confirmed F1–F3; its appended resolution status records
`85a2e2ab`, `83a38d879`, `732346a87`. Parent spot-checked the fixes; **no broad
rereview is claimed**. A-branch core regressions passed **251 tests** at
`732346a87`; parent confirms the combined suite at
`ae36f5a2639ff1c28d6657621b8afb0d81771b2b`: **255/255 passed, 0 skips, 66.169s**
(`/tmp/nostrc-completion-reviewed-selection.log`). All six shell suites, four static
checkers, full actionlint including ShellCheck, and whitespace passed. Subsequent
`4956fcf2` changes were docs only; this is not a new source review. The older 172-test result at `017039eea`
remains a dated historical receipt.
That dated bounded evidence and its source-specific hashes remain in
the [2026-10-16 continuation](reviews/dependency-aware-testing-ci-2026-10-15-staged-acceptance.md#2026-10-16-continuation--integration-9c03e7712).
Documentation/internal-testing changes require **no component bump**.

## Overview

The gnostr test suite validates memory safety, performance budgets, widget correctness, event flow integrity, and zero-copy sliding window invariants across the three library layers (`nostr-gobject`, `nostr-gtk`) and the application (`apps/gnostr`).

### Architecture

```
┌──────────────────────────────────────────┐
│  Real-Component Benchmarks (nightly)     │  ← RSS ceiling with real NDB data
├──────────────────────────────────────────┤
│  Real-Component Integration (CI per-PR)  │  ← NDB violation detection, real bind
├──────────────────────────────────────────┤
│  Mock-Based Integration (CI per-PR)      │  ← Event flow, delete authorization
├──────────────────────────────────────────┤
│  Widget Tests (Xvfb, CI per-PR)          │  ← Recycling stress, sizing, latency
├──────────────────────────────────────────┤
│  Unit Tests (no display, fast)           │  ← Store contract, cache bounds, lifecycle
└──────────────────────────────────────────┘
```

> **Key distinction**: The bottom 3 layers use mock objects and test individual
> mechanisms. The top 2 layers use the **real** `GnNostrEventItem`, `GnNostrEventModel`,
> `NoteCardFactory`, and a real temporary NDB — catching the architectural violations
> that mocks hide (main-thread NDB transactions, lazy loading during bind, etc.).

### Test Locations

| Layer | Directory | Links Against | Display Required |
|-------|-----------|---------------|-----------------|
| Shared infrastructure | `tests/testkit/` | `nostr-gobject` + glib | No |
| nostr-gobject unit | `nostr-gobject/tests/` | `nostr-gobject` + testkit | No |
| nostr-gobject integration | `nostr-gobject/tests/integ/` | Same + local NDB | No |
| nostr-gtk widget | `nostr-gtk/tests/` | `nostr-gtk` + `nostr-gobject` + testkit | Yes (Xvfb) |
| Application-level | `apps/gnostr/tests/` | app model sources + `nostr-gobject` | No |
| Application integration | `apps/gnostr/tests/integ/` | Same + NDB | No |
| Benchmarks | `benchmark/` | Release-with-debug-info | Yes (Xvfb) |

---

## Shared Test Infrastructure: The Testkit

**Location**: `tests/testkit/gnostr-testkit.{h,c}`

A static library that all test binaries link against, providing:

### `GnTestNdb` — Temporary NDB Instance

```c
GnTestNdb *gn_test_ndb_new(const char *opts_json);  // temp dir + storage_ndb_init
const char *gn_test_ndb_get_dir(GnTestNdb *ndb);
gboolean gn_test_ndb_ingest_json(GnTestNdb *ndb, const char *json);
void gn_test_ndb_free(GnTestNdb *ndb);
```

Each test gets an isolated nostrdb instance in a unique temp directory. Automatically cleaned up on free.

### Event Fixture Generation

```c
char *gn_test_make_event_json(int kind, const char *content, gint64 created_at);
char *gn_test_make_event_json_with_pubkey(int kind, const char *content,
                                           gint64 created_at, const char *pubkey_hex);
GPtrArray *gn_test_make_events_bulk(guint n, int kind, gint64 start_ts);
```

Generates minimal valid nostr event JSON strings with random (or specified) pubkeys and IDs.

### Main Loop Helpers

```c
gboolean gn_test_run_loop_until(GnTestPredicate pred, gpointer data, guint timeout_ms);
void gn_test_drain_main_loop(void);
```

### Memory Measurement

```c
gsize gn_test_get_rss_bytes(void);   // Linux: /proc/self/status VmRSS; macOS: mach_task_basic_info
double gn_test_get_rss_mb(void);
```

### Object Lifecycle Watchers

```c
GnTestPointerWatch *gn_test_watch_object(GObject *obj, const char *label);
void gn_test_assert_finalized(GnTestPointerWatch *watch);
void gn_test_assert_not_finalized(GnTestPointerWatch *watch);
```

Uses `g_object_weak_ref()` to track GObject finalization. Call `gn_test_assert_finalized()` after `g_object_unref()` to verify the object was actually freed.

---

## Test Suite by Blocker Category

### 1️⃣ Memory Footprint (target < 1 GB)

| Test File | Test Count | What It Validates |
|-----------|------------|-------------------|
| `benchmark/gnostr_bench_real_memory.c` | 1 | Memory profiling benchmark for real-world usage patterns |
| `nostr-gobject/tests/test_profile_provider_lru.c` | 5 | LRU cache capacity enforcement, eviction ordering, init/shutdown cycles, watcher cleanup |

**Key invariants enforced:**
- Profile provider cache size ≤ configured capacity after any number of insertions
- LRU evicts least-recently-used entries (recently accessed survive)
- Process RSS stays below ceiling during sustained timeline scrolling

### 2️⃣ Memory Leaks

| Test File | Test Count | What It Validates |
|-----------|------------|-------------------|
| `nostr-gobject/tests/test_lifecycle_leaks.c` | 5 | GNostrEvent create/destroy loops with weak refs, GNostrNdbStore subscribe/unsubscribe cycles, event with signal connections, rapid ref/unref churn (500x) |
| `nostr-gtk/tests/test_widget_churn_leaks.c` | 2 | MockItem with atomic live-count tracking; 50× model clear/repopulate cycles; model-to-NULL swap cycles |
| `apps/gnostr/tests/test_event_model_windowing.c` | 1 (subset) | `test_model_finalize_no_leak`: 20 create/destroy cycles with weak-ref verification |

**CI enforcement:**
All tests run with `G_DEBUG=fatal-warnings,gc-friendly` and `G_SLICE=always-malloc`. Sanitizer builds use `-fsanitize=address` with `detect_leaks=1`.

### 3️⃣ Widget Sizing

| Test File | Test Count | What It Validates |
|-----------|------------|-------------------|
| `nostr-gtk/tests/test_note_card_measure.c` | 3 | Content corpus with 9 edge cases (short text, long text, many links, many hashtags, unicode-heavy, empty, newlines-only, single long word). Tests `gtk_widget_measure()` bounds and ScrolledWindow clamping. |

### 4️⃣ Segfaults from Recycling / Signal Detachment

| Test File | Test Count | What It Validates |
|-----------|------------|-------------------|
| `nostr-gtk/tests/test_listview_recycle_stress.c` | 6 | **Most critical test.** Uses MockEventItem with `notify::profile` signal. Tests: basic bind/unbind, model replacement, **profile notification after unbind** (the exact crash path), rapid scroll churn (500 items), 20× clear/repopulate cycles, simultaneous profile updates during scroll. |

**Crash vector tested:**
1. `factory_bind_cb()` → installs handler on item's `notify::profile`
2. `factory_unbind_cb()` → attempts to disconnect the handler
3. If unbind races with async profile completion → UAF crash
4. Test verifies handler is properly disconnected by setting profile after unbind

### 5️⃣ UI Latency

| Test File | Test Count | What It Validates |
|-----------|------------|-------------------|
| `nostr-gtk/tests/test_bind_latency_budget.c` | 2 | Bind 300 items in GtkListView + scroll through entire list; heartbeat idle detects main-thread stalls > threshold. Model swap (10× clear/repopulate) stall detection. |

**Heartbeat mechanism:** A 5ms `g_timeout_add` heartbeat tracks gaps between ticks. If any gap exceeds `MAX_STALL_MS` (100ms, or 1000ms under ASan), it's counted as a "missed" heartbeat. Tests assert heartbeat actually fired (`count >= MIN_HEARTBEATS`) to prevent vacuous passing.

### 6️⃣ Event Flow Correctness

| Test File | Test Count | What It Validates |
|-----------|------------|-------------------|
| `nostr-gobject/tests/integ/test_event_flow_ingest_subscribe.c` | 5 | Ingest→subscribe→poll round-trip, multiple subscriptions, double-poll consumed, unsubscribe stops delivery, note counts |
| `nostr-gobject/tests/test_store_contract.c` | 7 | GNostrStore interface contract: implements check, subscribe, poll, lifecycle |
| `apps/gnostr/tests/integ/test_model_delete_authorization.c` | 6 | NIP-09: authorized removes from model, unauthorized preserves, non-existent harmless, multi-target, re-ingest, repeated cycles |

**NIP-09 delete tests:** Each test ingests both kind-0 profiles and kind-1 notes (so the model's readiness filter passes), creates a `GnNostrEventModel`, refreshes it, asserts the note is visible, then ingests a kind-5 delete and verifies model state.

### 7️⃣ Zero-Copy Sliding Windows

| Test File | Test Count | What It Validates |
|-----------|------------|-------------------|
| `apps/gnostr/tests/test_event_model_windowing.c` | 11 | Model lifecycle, refresh with profile ingestion (asserts n > 0 AND n ≤ MODEL_MAX_ITEMS), trim-newer/older, clear, thread-view bypass, pending count/flush, visible range, drain timer, finalize no leak |
| `apps/gnostr/tests/test_event_item_txn_budget.c` | 7 | Item creation budget, bulk create/destroy, profile set/get, metadata accessors (legacy constructor path), thread info, reaction/zap stats, animation skip |
| `nostr-gtk/tests/test_thread_view_bounded_load.c` | 5 | MAX_THREAD_EVENTS clamping, swap no accumulation, ancestor dedup, large GtkListView, empty thread |

**Windowing invariants:**

| Invariant | Type | Scope |
|-----------|------|-------|
| No duplicate keys across `note_key_set` ∩ `insertion_key_set` | Hard (always) | All views |
| `item_cache.size ≤ ITEM_CACHE_SIZE` | Hard | All views |
| `insertion_buffer.len ≤ INSERTION_BUFFER_MAX` after backpressure | Hard | All views |
| `notes->len ≤ MODEL_MAX_ITEMS` | Eventual (quiescent) | Non-thread views |

---

## Profile Service Tests

| Test File | Test Count | What It Validates |
|-----------|------------|-------------------|
| `nostr-gobject/tests/test_profile_service_batching.c` | 9 | Singleton lifecycle, request dedup (1 pending, 2 callbacks), debounce batching, cancel-for-user-data, stats accuracy, shutdown cleanup, debounce setting, invalid pubkey rejection (counter stays 0), relay provider registration |

---

## ASan / Sanitizer Relaxation

All timing-sensitive tests use a `SANITIZER_SLOWDOWN` multiplier:

```c
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) \
    || (defined(__has_feature) && (__has_feature(address_sanitizer) || __has_feature(thread_sanitizer)))
#  define SANITIZER_SLOWDOWN 10
#else
#  define SANITIZER_SLOWDOWN 1
#endif
#define TIMING_BUDGET_US(base) ((gint64)(base) * SANITIZER_SLOWDOWN)
```

This prevents flaky failures in CI sanitizer jobs where execution is 5-10× slower.

---

## Build & Run

### CMake

```bash
# Standard build with tests
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
cd build && ctest --output-on-failure

# With sanitizers
cmake -B build-san \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGNOSTR_ENABLE_ASAN=ON \
  -DGNOSTR_ENABLE_UBSAN=ON
cmake --build build-san
cd build-san && ctest --output-on-failure

# Widget tests only (requires Xvfb on Linux)
ctest -R nostr_gtk --output-on-failure

# Benchmarks (optional, for nightly)
cmake -B build-bench \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_BENCHMARKS=ON
cmake --build build-bench
cd build-bench && ctest -R bench --output-on-failure
```

### Meson

```bash
meson setup build-meson
meson compile -C build-meson
meson test -C build-meson
```

### By Label (CMake)

```bash
ctest -L unit --output-on-failure       # Unit tests (no display)
ctest -L widget --output-on-failure     # Widget tests (Xvfb)
ctest -L integ --output-on-failure      # Integration tests
ctest -L "memory|nightly" --output-on-failure  # Benchmarks
```

### Environment Variables

All tests automatically set:

| Variable | Value | Purpose |
|----------|-------|---------|
| `G_DEBUG` | `fatal-warnings,gc-friendly` | Turn GLib warnings into fatal errors; GC-friendly mode for leak detection |
| `G_SLICE` | `always-malloc` | Disable GSlice so ASan/Valgrind can track allocations |
| `GDK_BACKEND` | `x11` | Force X11 backend for Xvfb compatibility (widget tests) |

---

## CI Configuration

| Job | What | When | Sanitizers |
|-----|------|------|------------|
| **Linux Debug** | All unit tests (no X) | Every PR | None |
| **Linux ASan/UBSan** | All tests including widgets under Xvfb | Every PR | `address`, `undefined`, `detect_leaks=1` |
| **Nightly Perf/Memory** | Benchmarks with RSS ceiling enforcement | Nightly | None (RelWithDebInfo) |

---

## `cmake/GnTest.cmake` Helper Macros

### `gn_add_gtest(name SOURCES ... LIBS ... [ENV ...] [TIMEOUT n])`

Registers a standard GTest binary. Automatically sets `G_DEBUG`, `G_SLICE`, applies sanitizer flags, registers with CTest.

### `gn_add_gtest_xvfb(name SOURCES ... LIBS ... [ENV ...] [TIMEOUT n])`

Same as above, but wraps execution in `xvfb-run -a` on Linux. On macOS, runs natively.

---

## Test Inventory

| # | File | Tests | Category | Display |
|---|------|-------|----------|---------|
| 1 | `nostr-gobject/tests/test_store_contract.c` | 7 | Store interface | No |
| 2 | `nostr-gobject/tests/test_lifecycle_leaks.c` | 5 | Memory leaks | No |
| 3 | `nostr-gobject/tests/test_profile_provider_lru.c` | 5 | Memory bounds | No |
| 4 | `nostr-gobject/tests/test_profile_service_batching.c` | 9 | Service correctness | No |
| 5 | `nostr-gobject/tests/integ/test_event_flow_ingest_subscribe.c` | 5 | Event flow | No |
| 6 | `nostr-gtk/tests/test_listview_recycle_stress.c` | 6 | Segfault prevention | Yes |
| 7 | `nostr-gtk/tests/test_note_card_measure.c` | 3 | Widget sizing | Yes |
| 8 | `nostr-gtk/tests/test_widget_churn_leaks.c` | 2 | Memory leaks | Yes |
| 9 | `nostr-gtk/tests/test_bind_latency_budget.c` | 2 | UI latency (mock) | Yes |
| 10 | `nostr-gtk/tests/test_thread_view_bounded_load.c` | 5 | Sliding windows | Yes |
| 11 | `apps/gnostr/tests/test_event_model_windowing.c` | 11 | Sliding windows | No |
| 12 | `apps/gnostr/tests/test_event_item_txn_budget.c` | 7 | Transaction budgets | No |
| 13 | `apps/gnostr/tests/integ/test_model_delete_authorization.c` | 6 | NIP-09 delete | No |
| 14 | `benchmark/gnostr_bench_real_memory.c` | 1 | Memory profiling | Yes |
| 15 | `apps/gnostr/tests/test_ndb_main_thread_violations.c` | 5 | **NDB violation detection** | No |
| 16 | `apps/gnostr/tests/test_real_bind_latency.c` | 2 | **Real-component latency** | Yes |
| 17 | `benchmark/gnostr_bench_real_memory.c` | 1 | **Real-component memory** | Yes |
| — | **Total** | **~82** | | |

---

## Adding New Tests

### Pattern for nostr-gobject tests

```c
#include "gnostr-testkit.h"
#include <nostr-gobject-1.0/your_header.h>

static void test_your_feature(void) {
  GnTestNdb *ndb = gn_test_ndb_new(NULL);
  // ... test code ...
  gn_test_ndb_free(ndb);
}

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nostr-gobject/your-feature/test-name", test_your_feature);
  return g_test_run();
}
```

```cmake
gn_add_gtest(nostr-gobject-test-your-feature
  SOURCES tests/test_your_feature.c
  LIBS gnostr-testkit nostr_gobject PkgConfig::GLIB
)
```

### Pattern for nostr-gtk widget tests

```c
#include <gtk/gtk.h>

int main(int argc, char *argv[]) {
  gtk_test_init(&argc, &argv, NULL);  // NOT g_test_init
  g_test_add_func("/nostr-gtk/your-widget/test-name", test_your_widget);
  return g_test_run();
}
```

```cmake
gn_add_gtest_xvfb(nostr-gtk-test-your-widget
  SOURCES tests/test_your_widget.c
  LIBS gnostr-testkit nostr_gtk nostr_gobject PkgConfig::GTK4
)
```

### Pattern for timing-sensitive tests

```c
#if defined(__SANITIZE_ADDRESS__) || ...
#  define SANITIZER_SLOWDOWN 10
#else
#  define SANITIZER_SLOWDOWN 1
#endif
#define TIMING_BUDGET_US(base) ((gint64)(base) * SANITIZER_SLOWDOWN)

static void test_fast_operation(void) {
  gint64 start = g_get_monotonic_time();
  // ... operation ...
  g_assert_cmpint(g_get_monotonic_time() - start, <, TIMING_BUDGET_US(1000));
}
```

---

## Real-Component Integration Tests

### The Problem with Mocks

The original test suite used `MockEventItem` objects with static string properties.
These mocks exercise the GTK recycling mechanism correctly but **skip the actual
code paths that cause bugs**: NDB transactions, Pango content rendering, async HTTP
fetches, and widget lifecycle complexity.

| Real Component | What Mocks Skip |
|---|---|
| `GnNostrEventItem` + lazy NDB load | NDB transactions on main thread, content copying |
| `NostrGtkNoteCardRow` (6300+ lines) | Pango layout, async HTTP, GStreamer video |
| `storage_ndb_begin_query_retry` | `usleep()` blocking the main loop |
| `gnostr_render_content()` | Synchronous content parsing |

### Main-Thread NDB Violation Detection

The core architectural test uses **instrumentation** in `storage_ndb.c` (enabled via
`-DGNOSTR_TESTING`) to detect when NDB read transactions are opened on the main thread.

```c
// In your test setUp:
gn_test_mark_main_thread();
gn_test_reset_ndb_violations();

// Exercise the code path...
const char *content = gn_nostr_event_item_get_content(item);

// Assert zero violations:
gn_test_assert_no_ndb_violations("during item property access");
```

On failure, the assertion dumps diagnostic output:

```
╔════════════════════════════════════════════════════╗
║ MAIN-THREAD NDB VIOLATIONS: 3 during bind
╠════════════════════════════════════════════════════╣
║  [0] storage_ndb_begin_query
║  [1] storage_ndb_begin_query_retry
║  [2] storage_ndb_begin_query
╚════════════════════════════════════════════════════╝

FIX: Move NDB transactions to a GTask worker thread.
```

### Key Real-Component Tests

| Test | File | What It Catches |
|------|------|-----------------|
| NDB violations during item access | `test_ndb_main_thread_violations.c` | Lazy NDB loads on main thread |
| NDB violations during model refresh | Same | Model queries on main thread |
| NDB violations during batch metadata | Same | Batch reaction/zap queries on main thread |
| Real bind with NDB violation counter | `test_real_bind_latency.c` | NDB txns during GtkListView bind |
| Real bind with heartbeat stall detection | Same | Actual UI stalls during scroll |
| Real NDB memory benchmark | `gnostr_bench_real_memory.c` | Memory with realistic content corpus |

### Testkit Enhancements

The testkit now includes:

- **`gn_test_mark_main_thread()`** — Mark current thread for NDB violation detection
- **`gn_test_assert_no_ndb_violations(ctx)`** — Assert zero violations with diagnostics
- **`gn_test_ingest_realistic_corpus(ndb, n_events, n_profiles)`** — Ingest varied content
- **`gn_test_make_realistic_event_json(kind, style, ts)`** — Content styles: short, medium, long, unicode, URLs, mentions
- **`gn_test_make_profile_event_json(pk, name, about, pic, ts)`** — Kind-0 profiles
- **`gn_test_heartbeat_start/stop(hb, interval_ms, max_stall_ms)`** — Stall detection

### How to Use This for Iterative Bug Fixing

The test harness is designed for LLM-assisted iterative development:

1. **Run `gnostr-test-ndb-main-thread-violations`** — It will FAIL with a list of functions calling NDB on the main thread
2. **Fix one violation at a time** — Move the NDB call to a `g_task_run_in_thread()` worker
3. **Re-run the test** — The violation count decreases
4. **Repeat until zero violations** — The test passes

The violation counter is **deterministic** — it doesn't depend on timing, system load, or randomness. Each violation is a concrete function call that must be moved off the main thread.

---

## LLM Closed-Loop Debugging

This section describes how an LLM agent can perform complete debug cycles — identify
bugs, reproduce them, diagnose root causes, apply fixes, and verify corrections —
without human intervention between iterations.

Detailed skill documents are in [`skills/`](../skills/):

| Skill | File | Purpose |
|-------|------|---------|
| **Closed-Loop Debug** | [`skills/closed-loop-debug/SKILL.md`](../skills/closed-loop-debug/SKILL.md) | Full IDENTIFY → REPRO → DIAGNOSE → FIX → VERIFY workflow |
| **Broadway + Playwright** | [`skills/broadway-debug/SKILL.md`](../skills/broadway-debug/SKILL.md) | UI debugging via Broadway HTML5 backend |
| **GDB / LLDB** | [`skills/gdb-debug/SKILL.md`](../skills/gdb-debug/SKILL.md) | Memory errors, segfaults, reference counting |

### The Debug Loop

```
    ┌──────────────────────────────────────────────────────────┐
    │                                                          │
    ▼                                                          │
┌─────────┐    ┌─────────┐    ┌─────────┐    ┌─────────┐    ┌─────────┐
│ IDENTIFY │───►│ REPRO   │───►│ DIAGNOSE│───►│  FIX    │───►│ VERIFY  │
│ (test/   │    │ (test + │    │ (GDB/   │    │ (edit   │    │ (test + │
│  report) │    │  ASan)  │    │  LLDB/  │    │  code)  │    │  Bway)  │──┐
└─────────┘    └─────────┘    │  ASan)  │    └─────────┘    └─────────┘  │
                               └─────────┘                        │       │
                                                                  │ PASS  │ FAIL
                                                                  ▼       │
                                                               DONE ◄─────┘
```

### Broadway Persistent Sessions

The Broadway daemon (`gtk4-broadwayd`) persists independently of the app process.
This means the browser tab and Playwright MCP connection survive across app rebuilds:

```bash
# 1. Start daemon once (stays running)
./skills/broadway-debug/scripts/run-broadway.sh

# 2. Connect Playwright
# browser_navigate(url="http://127.0.0.1:8080")

# 3. Debug loop — daemon stays, browser stays:
#    Edit code → cmake --build build → relaunch app → browser_snapshot()
```

Any GTK4 app in the nostrc stack works with Broadway:

| App | Launch Command |
|-----|---------------|
| gnostr | `GDK_BACKEND=broadway BROADWAY_DISPLAY=:5 GSETTINGS_SCHEMA_DIR=build/apps/gnostr build/apps/gnostr/gnostr` |
| gnostr-signer | `GDK_BACKEND=broadway BROADWAY_DISPLAY=:5 GSETTINGS_SCHEMA_DIR=build/apps/gnostr-signer build/apps/gnostr-signer/gnostr-signer` |

### GTK Test Utilities vs Playwright

For automated tests, prefer GTK's built-in test utilities over Playwright DOM interaction:

| GTK Test Utility | Playwright Equivalent | When to Use GTK |
|------------------|-----------------------|-----------------|
| `gtk_widget_measure()` | Screenshot + pixel comparison | Widget sizing validation |
| `gtk_test_widget_wait_for_draw()` | `browser_wait_for()` | Waiting for layout completion |
| `gtk_test_accessible_assert_role()` | `browser_snapshot()` + parse | Accessibility contract tests |
| `g_signal_emit()` | `browser_click()` | Deterministic signal testing |

GTK test utilities are deterministic, run headless (via Xvfb), and don't need a
Broadway daemon. Use Playwright for **visual verification** and **interaction flow
debugging**, GTK utilities for **automated regression tests**.

### Debugger Integration

#### GDB (Linux) — batch mode for LLM agents

```bash
gdb -batch \
  -ex "set pagination off" \
  -ex "set print pretty on" \
  -ex "run" \
  -ex "bt full" \
  -ex "thread apply all bt 10" \
  --args build-debug/apps/gnostr/tests/FAILING_TEST 2>&1
```

#### LLDB (macOS) — batch mode for LLM agents

```bash
lldb -b \
  -o "run" \
  -o "bt all" \
  -k "bt all" \
  -k "quit" \
  -- build-debug/apps/gnostr/tests/FAILING_TEST 2>&1
```

#### ASan (cross-platform) — self-diagnosing memory errors

```bash
ASAN_OPTIONS=detect_leaks=1 \
  build-asan/apps/gnostr/tests/FAILING_TEST 2>&1
```

ASan reports are **self-contained diagnoses**: they show WHERE the crash happened,
WHERE the memory was allocated, WHERE it was freed, and WHICH threads were involved.
No interactive debugger session needed for most UAF/leak issues.

### GLib Debug Environment

All tests automatically set these, but for manual debugging:

| Variable | Value | Purpose |
|----------|-------|---------|
| `G_DEBUG` | `fatal-warnings,gc-friendly` | Abort on warnings; GC-friendly for leak detection |
| `G_SLICE` | `always-malloc` | Disable slab allocator (required for ASan) |
| `GOBJECT_DEBUG` | `objects` | Track all GObject instances (heavy but thorough) |
| `GTK_DEBUG` | `interactive` | Open GTK Inspector in Broadway session |

### GTK Inspector

The GTK Inspector is a built-in interactive debugger for GTK4 — widget tree
introspection, live CSS editing, GObject property/signal monitoring, rendering
performance analysis, and layout debugging. **It runs in the same Broadway session
as the app**, so Playwright can interact with both.

```bash
# Launch any app with Inspector
GTK_DEBUG=interactive GDK_BACKEND=broadway BROADWAY_DISPLAY=:5 \
  GSETTINGS_SCHEMA_DIR=build/apps/gnostr \
  build/apps/gnostr/gnostr
```

| Inspector Panel | Debugging Use |
|-----------------|---------------|
| **Objects** | Widget tree navigation, GObject property inspection, signal handler auditing, reference count monitoring |
| **CSS** | Live CSS editing to test layout fixes without recompiling |
| **Visual** | Box model (margins/borders/padding/content), allocation bounds, baseline alignment |
| **Accessibility** | Verify accessible roles/labels match Playwright `browser_snapshot()` output |
| **Statistics** | Frame rate, frame times (stall detection), texture memory, CSS node count |
| **Recorder** | Frame-by-frame render analysis, identify redundant redraws and layout cascades |

> **📖 Full guide**: [`skills/gtk-inspector/SKILL.md`](../skills/gtk-inspector/SKILL.md) —
> LLM-actionable workflows for each panel, Playwright-based interaction with the
> inspector, programmatic usage in tests and GDB sessions, and 4 debugging scenarios
> (widget sizing, signal leaks, memory growth, layout performance).

### Strategy by Bug Class

| Bug Class | Identify | Reproduce | Diagnose | Verify | Inspector Panel |
|-----------|----------|-----------|----------|--------|-----------------|
| **Segfault** | ASan build crashes | `ctest -R recycle` | ASan trace → freed-by location | Test passes under ASan | Objects (signals) |
| **Memory leak** | `ASAN_OPTIONS=detect_leaks=1` | Lifecycle test | LSAN trace → allocation site | `gn_test_assert_finalized()` | Objects (ref count) |
| **Main-thread block** | UI stalls visibly | `ctest -R ndb-violations` | Violation dump → function name | Violation count = 0 | Statistics (frame times) |
| **Widget sizing** | Broadway screenshot | `ctest -R note_card_measure` | Inspector Visual panel + CSS | Dimensions within bounds | **Visual + CSS** |
| **Signal race** | Random crash on scroll | Recycle stress test | Inspector Objects → Signals section | No ASan errors after 50 cycles | **Objects (Signals)** |
| **Latency** | Broadway feels slow | Heartbeat test + NDB violations | Inspector Statistics + Recorder | Heartbeat missed count = 0 | **Statistics + Recorder** |

---

## Related Documentation

- [`docs/BROADWAY_TESTING.md`](BROADWAY_TESTING.md) — Broadway + Playwright browser UI testing
- [`docs/test-scenarios/`](test-scenarios/) — Broadway-specific test scenarios
- [`skills/`](../skills/) — LLM skill documents for debug workflows:
  - [`skills/broadway-debug/`](../skills/broadway-debug/SKILL.md) — Broadway + Playwright UI debugging
  - [`skills/gtk-inspector/`](../skills/gtk-inspector/SKILL.md) — GTK Inspector debugging (widget tree, CSS, signals, performance)
  - [`skills/gdb-debug/`](../skills/gdb-debug/SKILL.md) — GDB/LLDB debugging
  - [`skills/closed-loop-debug/`](../skills/closed-loop-debug/SKILL.md) — Full closed-loop debug workflow
- [`ARCHITECTURE.md`](../ARCHITECTURE.md) — Project architecture overview
