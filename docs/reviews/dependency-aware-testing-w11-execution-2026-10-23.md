# W11 executable census and current-head closure gaps — 2026-10-23

## E follow-through at44044fefe — physical runner and actual collection

[Implemented command/contract protocol](dependency-aware-testing-w11-e-protocol-2026-10-23.md)
now supplies `census`, attempt-scoped `collect`, original-PR-source `execute`,
failure-preserving paired `measurements` and **unapplied** `prepare-cutover`.
These replace neither native custody nor complete selector/legacy receipts.
The runner rejects transplanted candidates, uses committed-source bytes/modes
rather than a fixed historical product-body whitelist, checks within-run drift,
and keeps actual phase/opaque-adapter times and original failures. Its physical
byte/mode checks verify pre/post endpoints, **not** within-phase modify/restore
or immutable execution; accepted inner custody must supply that separate proof. All14
clean/incremental cells are accounted; no automatic W11 credit is granted.
No production workload was run locally under the host's5GiB/Docker restriction.

[Actual new dated ledger](dependency-aware-testing-ci-2026-10-18-run-ledger.md#e-bounded-execution-collection--2026-10-23t102247933138z)
records58 roots at10:22:47Z (53 failed,one cancelled,four nonterminal), original
PR22 failure ZIPs, failed manual broad accounting and terminal strict-fuzz
success. **0/20 and0/2;W11 NOT MET/W12 NOT RUN.** Future qualifying original-source
cases need parent review and B/A/native prerequisites, not planner-case promotion.
The prior master663 replay/ownership narrative below remains source-specific
history; it is not a current implementation inventory or acceptance claim.


## Scope and result

Base/master: `663edcd70c0d313c98f87610aa41f1261bce4ee6`; finite measurement
child `nostrc-0cfsg.36.3`. This lane changes only its diagnostic tool, regressions
and evidence documentation. Resource admission `.36.1`, hosted policy `.36.2`,
shared engine/inventory/workflows, JSON-GLib and heartbeat remediation are not
changed. **Internal tooling/tests/docs: no component version bump. Parent lands
local commits.** `.5` stays CLOSED (finite baseline); W11 remains open.

**Bounded API window 2026-10-23T02:12:04.578186Z–02:12:28.940355Z:** 54 shadow
roots, **49 failed / one cancelled / two queued / one in progress / one pending**;
zero successful roots. **0/20 qualifying reviewed distinct PR comparisons and
0/2 complete broad integrations. W11 NOT MET; W12 NOT RUN.** This is a dated,
sequential snapshot, not a live counter. Older failures remain in the ledger.

Unlike earlier numeric-only observations, all **21 actual merged PRs** were
censused from the API, their immutable base/head/merge commits checked against
Git, and their **own committed planner and policy executed twice per PR** in an
isolated clone. Repeated output bytes agree. This is **logical diagnostic proof
only**, never physical coverage, a successful comparison, or cost savings.

## Current hosted evidence, not parent colours

| Source | Actual evidence and disposition |
|---|---|
| [PR20 root 39884384021](https://github.com/chebizarro/nostrc/actions/runs/39884384021) | Plan artifact12471967279 binds tested merge `7ff720d9709672cb60a7b7b50b3294de337ee186`, not merged master. 58 jobs:38 success,9 failure,1 cancellation,10 nonterminal at capture. No terminal root gate. |
| [PR20 Linux libgo 120189097071](https://github.com/chebizarro/nostrc/actions/runs/39884384021/job/120189097071), [Windows 120189097982](https://github.com/chebizarro/nostrc/actions/runs/39884384021/job/120189097982) | Both terminal success on corrected source. Libgo receipt12472661715 passes its three selected check IDs, still `complete_gate_eligible=false`. macOS standalone receipt12471834674 likewise passes its three IDs, not full root/profile acceptance. |
| PR20 selected failures | Receipt12473283533:core Linux lacks `display/private_bus/signer_endpoint`;12473348634:core ASan lacks signer;12473040102/12472582975/12471703786:ordinary GH/Grotto/core macOS lack signer;12472064347:sanitizer fresh partition differs;12473585107/12473750591:MDK08/011 lack keyring/nak/signer. All failure receipts retain incomplete flags. Resources do not fix membership/authority gaps below. |
| [PR20 broad macOS 120189097756](https://github.com/chebizarro/nostrc/actions/runs/39884384021/job/120189097756) | Actual retained log fails `test_nostr_gobject_main_thread_latency`; `run_heartbeat` asserts `(hb.missed_count <= 2): (3 <= 2)` at source line272. This is another observed failure, not a causal diagnosis or permission to relax the guard. Separate owner handles the additional probe. |
| [PR21 root 39885673421](https://github.com/chebizarro/nostrc/actions/runs/39885673421), [gate 120200327073](https://github.com/chebizarro/nostrc/actions/runs/39885673421/job/120200327073) | Terminal failure. Plan12472608978 binds tested merge `a86aae499c3770a7e84b3d6479ad78977346d683`:zero selected checks/lanes. Validation12473820942 retains `passed=false`, `complete=true`, `complete_gate_eligible=false`, errors `inventory coverage/enforcement readiness cannot satisfy complete gate` and `inventory/profile coverage is incomplete; W4/W11 acceptance not established`. Thirteen unselected job skips do not make this a complete broad run. |
| PR21 independent source check | Commit API verifies parents `d9c9c8808b2611fc179ed4ed3640e63920f4683f` / `5641f8b0bade465fee093925aa29443e791c4ae9` and tree `a2fca595d744434038d67e8628392b6700bcac2d`, exactly master663's tree. Canonical merged SHA663 is not substituted for tested SHAa86. Commit API observed02:15:56Z, after the bounded job window above. |
| [master20 39884406426](https://github.com/chebizarro/nostrc/actions/runs/39884406426) | Plan12472495513 binds `d9c9c8808b2611fc179ed4ed3640e63920f4683f`; selected core/Grotto-ASan/native-core and broad macOS/JSON-fuzz fail. 59 jobs include one unselected skip and17 nonterminal; no terminal gate. No new JSON/heartbeat cause or fix claimed. |
| [current master 39885713147](https://github.com/chebizarro/nostrc/actions/runs/39885713147) | Plan12471884958 binds exact663 source.58 jobs:17 success,1 cancellation,40 nonterminal. No terminal gate or complete integration; earlier corrected-source Windows success remains distinct from W11 credit. |

API digest/size, ZIP CRC and every downloaded member hash were checked for all
15 archives retained by this lane (ten initial, five final). Raw sources, original
failed logs, archive proofs and per-query timestamps are under
`/tmp/nostrc-w11-execution-20261023/`; temporary/local bytes can expire. The
GitHub artifact/job links and immutable identities above are durable references,
not a claim of permanent hosted retention.

## Finite current-head closure-gap register

Inventory byte SHA256:
`8b428b820152fd2d78b487153e4ce20623c0c5f8d4518dec6ba993c94fe2def6`.
The `inventory` diagnostic retains that exact input, all ten gap rows, all89
workflow job rows and profile-specific ordinary33 ownership. This register is
about **current code's refusal**, not an instruction to delete stale metadata.

`aggregate()` unconditionally rejects nonempty `plan.coverage_gaps`
(`scripts/test_selection.py:646-651`). Each following row independently keeps
that global refusal active, even with a zero-lane plan as PR21 demonstrates.
Physical readiness must be proven before resolving a row; textual removal is
not an implementation or acceptance fix.

| Exact gap ID | Finite remaining interface / implementation / evidence |
|---|---|
| `workflow_profile_union` | 89 recorded workflow-job rows; only5 have profile mappings,84 do not. This is metadata accounting, not89 executed families. Provide actual per-kind family/variant contracts and source-bound result import for the existing union. Grotto explicitly retains unsupported ubuntu-latest/native and advisory TSan variants, native smoke/quarantine steps, Meson/deb/shell-extension and optional wallet/activation/fuzz variants. Preserve advisory versus blocking semantics. |
| `root_asan_execution_contract` | Full fresh source/header/runtime audits and actual phase receipts; three legacy exclusions remain unexecuted, with generic exclusion and separate audit/fuzz semantics unaccepted. Neither new latency exclusions nor weakening sanitizer contracts is authorized. |
| `groundhog_sanitizer_execution_contract` | Reconcile actual fresh registration/fixture partition against its independently reviewed exact source authority; full nonmember producer audits and real sanitizer execution remain required. Current PR20 partition refusal is physical evidence. No automatic authority renewal or selected-only audit. |
| `libgo_macos_asan_stress_exclusion` | `GoChannelStressTest` remains `not_executed` in standalone macOS ASan; build/audit alone is insufficient. A real causal runtime fix/proof or an explicitly approved contract decision is needed, not a skip-as-pass or importing Linux's33. |
| `supported_profile_union` | Prove complete native/toolchain/sanitizer/optional registration and execution union. Ordinary33 per-profile ownership/admission below is a concrete remaining implementation, not just resource setup. |
| `shadow_cutover` | W11 measurement/cohort/broad/protection evidence is still absent. This row currently participates in the same unconditional execution-completeness refusal, although it names rollout acceptance. Parent must explicitly resolve the technical-completeness versus rollout-state dependency; do not delete the row, mark W11 met, or change criteria without user approval. |
| `prepared_peer_linux_contract` | Prepared011 is local-only Linux/arm64 diagnostic. Original08/09/011 Docker build/peer/cleanup and native/profile acceptance need their actual distinct receipts; no prepared alias or complete promotion. |
| `homed_unit_profile_contract` | Five legacy opt-in unit registrations (`homed_manifest/cache/cache_config/uid_map/groups`) need a real selective profile or typed imported original execution contract. Their legacy success is not selective ownership. Experimental runtime options stay off. |
| `worker_setup_and_grotto_execution` | `.36.1` supplies resource interfaces; actual complete Grotto/default/non-CTest/wrapped execution and variant contracts remain separately necessary. Tool fingerprints do not prove bus/display/service/signer consent. |
| `supported_profile_execution_remainder` | Named profiles:core Linux + Linux/macOS ASan; GH ordinary/sanitizer/MDK08/011/prepared011; Grotto ordinary/ASan. Complete source/header/runtime/default-producer custody, independent tools, approved signer and exact phase/runtime evidence remain required. Generic root `core.linux` is not an authored hosted matrix. Hosted policy `.36.2` is a prerequisite, not completion of these rows. |

### Exact33: missing ordinary ownership, not missing all Go descriptors

Confirmed by enumerating eligible `kind=ctest` checks and exact per-profile names:

| Profile | Claimed reviewed names | Current authority |
|---|---:|---|
| `groundhog.linux` | **0/33** | Universe422; selected-primary386 + fixture closure3 = required389; exact33 omitted. |
| `grotto.linux` | **0/33** | Universe461; selected-primary422 + fixture closure6 = required428; exact33 omitted. |
| `core.linux`, `core.linux.asan`, `core.macos.asan` |33/33 each| Existing `supported.libgo` / `root.asan.libgo` ownership; not authority for another profile. |
| standalone `libgo.linux.asan` / `libgo.macos.asan` |33/33 /32/33| Different source root, configure/toolchain/environment; macOS stress omission above. |

Descriptors already exist in `libgo/CMakeLists.txt:725-752`, explicitly mapping
33 registrations onto27 direct executable targets; the reviewed ordinary
omission's prerequisite audit includes29 target closures. Existing `supported.libgo`
has ordinary nodes/source ownership but does **not** list either ordinary
profile. There is no need to invent another name-only whitelist.

The current narrow authority is intentionally incompatible with complete
execution: `test_selection_subset.py:55-61` requires exact33 outside required
closure;`:258-269` prohibits executing them and requires incomplete resolved
flags;`:274-305` requires the same on physical receipts/postbuild audits.
`test_selection.py:769-777` selects that authority by profile, and`:862-866`
forces `complete=false` for those subset profiles. Simply adding ownership names
without a distinct reviewed full-admission path collides with these checks.

**Parent interface / tracked implementation `nostrc-0cfsg.9.1`:**

1. Author genuine unique ordinary-profile ownership for the actual33, with
   fresh root-profile exports and all actual source/header/runtime inputs,
   mapped targets/default prerequisites, fixture closure and impact edges.
2. Preserve CTest run semantics, not names alone: six W4 registrations reuse
   executables but require `GOF_NWORKERS=4` (`libgo/CMakeLists.txt:655-680`), with
   their actual timeouts; retain authored profile and phase environments/argv.
3. Explicitly distinguish full ownership/execution admission from the frozen
   diagnostic-subset route. Preserve exact33 diagnostic refusals, omission
   accounting and full omitted-target audits; **do not expand that authority**,
   strip incomplete flags, silently drop contracts or copy standalone results.
4. Prove the full route with fresh native Linux/x86 GCC configure/export,
   prerequisite/default build, pre/post audits, exact actual execution and
   negative mutations. Full ownership is not a waiver of reviewed omissions.
   Other global gaps and W11/operator acceptance remain independently open.

The interface was sent to parent tracker `.36`; no shared code/inventory was
edited. New diagnostic tool/test control inputs also need the parent's normal
source-binding assessment at integration; no executable/profile admission is
inferred here.

## Real-PR candidate census and replay disposition

All158 existing PR workflow roots were fetched in two pages; all map to these21
actual heads. At that capture PR1–18 dependency roots had failed; PR19–21 were
nonterminal. The later02:12 snapshot records PR21's terminal failure above.
The exact original raw API response, two-parent bindings, diff bytes/hash,
original policy/planner/workflow hashes, event metadata, physical process argv,
stdout/stderr and repeated plans are retained per PR by the runner.

Replay sources below are **canonical merged results**, not claimed to be the
original Actions-tested PR merge objects. That distinction is retained in every
result. Short SHA labels link full immutable merge identities.

| PR | Immutable replay source | Original-policy result / selected check IDs |
|---:|---|---|
| [1](https://github.com/chebizarro/nostrc/pull/1) | [1fad10a23](https://github.com/chebizarro/nostrc/commit/1fad10a239f9cef72b7b056b8ab99679337d53c4) | broad /151 |
| [2](https://github.com/chebizarro/nostrc/pull/2) | [096091aaa](https://github.com/chebizarro/nostrc/commit/096091aaa758468a8338e9cf0646acee432cfba1) | broad /151 |
| [3](https://github.com/chebizarro/nostrc/pull/3) | [f39401e12](https://github.com/chebizarro/nostrc/commit/f39401e12eaac24cbf86e903cdc37198fb332479) | broad /151 |
| [4](https://github.com/chebizarro/nostrc/pull/4) | [f9d1e8537](https://github.com/chebizarro/nostrc/commit/f9d1e853794d81037e8dcf177e40b9bc83178b9c) | broad /151 |
| [5](https://github.com/chebizarro/nostrc/pull/5) | [7219eb4b5](https://github.com/chebizarro/nostrc/commit/7219eb4b5dd7e3bd046dd96f846baa26ed08ddbf) | broad /151 |
| [6](https://github.com/chebizarro/nostrc/pull/6) | [a14b8b38c](https://github.com/chebizarro/nostrc/commit/a14b8b38cbd1b350324c2fbfed227592ede3ab2b) | broad /151 |
| [7](https://github.com/chebizarro/nostrc/pull/7) | [705d7b051](https://github.com/chebizarro/nostrc/commit/705d7b051b41a54fdd62e1823a1d074c8e9d7197) | broad /151 |
| [8](https://github.com/chebizarro/nostrc/pull/8) | [493aaa20e](https://github.com/chebizarro/nostrc/commit/493aaa20ee9a8c9cd58ac6831327685bb679eacb) | broad /151 |
| [9](https://github.com/chebizarro/nostrc/pull/9) | [37e144cab](https://github.com/chebizarro/nostrc/commit/37e144cab898e942d2875d903670ccaf4335158b) | affected /72; shared Signer + resource change,8 lanes |
| [10](https://github.com/chebizarro/nostrc/pull/10) | [d953c37fb](https://github.com/chebizarro/nostrc/commit/d953c37fb7705eae76342c78222308f1290995ec) | broad /151 |
| [11](https://github.com/chebizarro/nostrc/pull/11) | [3c7c42c08](https://github.com/chebizarro/nostrc/commit/3c7c42c08cf1fade80da1355e4392847060abdb2) | broad /154 |
| [12](https://github.com/chebizarro/nostrc/pull/12) | [93970ed48](https://github.com/chebizarro/nostrc/commit/93970ed48fb5711193bc73bd0b304b9e81e6f25f) | broad /159 |
| [13](https://github.com/chebizarro/nostrc/pull/13) | [53f2e822d](https://github.com/chebizarro/nostrc/commit/53f2e822d04bd32e243a9cf969396f2f241af83e) | broad /159 |
| [14](https://github.com/chebizarro/nostrc/pull/14) | [ed7063170](https://github.com/chebizarro/nostrc/commit/ed70631704778a7fbe31d45b24161570a91abb53) | broad /165 |
| [15](https://github.com/chebizarro/nostrc/pull/15) | [b7aede816](https://github.com/chebizarro/nostrc/commit/b7aede8166cc52a6ec55faafd755a1fa4a9121d1) | broad /170 |
| [16](https://github.com/chebizarro/nostrc/pull/16) | [f646db033](https://github.com/chebizarro/nostrc/commit/f646db033031a32ad0f6283f881d8506eafbcc4b) | broad /179 |
| [17](https://github.com/chebizarro/nostrc/pull/17) | [dd949e4b7](https://github.com/chebizarro/nostrc/commit/dd949e4b7a2969e01ac5dea3f12bdde2cf22655b) | broad /184 |
| [18](https://github.com/chebizarro/nostrc/pull/18) | [0ce430299](https://github.com/chebizarro/nostrc/commit/0ce430299060a09e4a2db250be1a2cac4df2c212) | broad /391 |
| [19](https://github.com/chebizarro/nostrc/pull/19) | [587ebea25](https://github.com/chebizarro/nostrc/commit/587ebea254fb3b7c19c8f91a42b23a8a4d950db3) | broad /391 |
| [20](https://github.com/chebizarro/nostrc/pull/20) | [d9c9c8808](https://github.com/chebizarro/nostrc/commit/d9c9c8808b2611fc179ed4ed3640e63920f4683f) | broad /391 |
| [21](https://github.com/chebizarro/nostrc/pull/21) | [663edcd70](https://github.com/chebizarro/nostrc/commit/663edcd70c0d313c98f87610aa41f1261bce4ee6) | affected /0; docs/tracker-only |

The plan does not impose a future-PR-only rule. Historical real PR replay could
in principle count **if original criteria were actually met**, with independently
bound original diff/tested tree, genuine legacy/new execution policies and
physical results, every difference reviewed and cost comparability established.
Here, unchanged historical policies retain gaps/incomplete authorities, no
successful complete shadow root exists, and these21 changes do not supply the
required Grotto/UI/MLS performance matrix. Applying today's policy to yesterday's
filenames cannot repair that. **No dual-source framework or filename-only
projection is built or proposed as acceptance by this lane; no synthetic PRs,
head/retry/nested-job counting or retrospective failed-coverage promotion.**

## Run sequence and measurement contract (unchanged)

This is a finite gate register, not a claim all steps are runnable now:

1. Parent integrates concrete resource/policy fixes and separately reviewed
   full-membership/admission work; resolve actual remaining guard/provider and
   family failures with their owners. Verify exact new control-input binding.
   The ten gap rows above must have source-grounded dispositions, not be cleared
   to manufacture a complete flag. W11 versus technical-gate state needs the
   explicit parent/user decision described above.
2. First execute focused actual affected lanes at that exact integrated source,
   preserving failures. Do not dispatch another unchanged cold broad run while
   existing roots are running or known blockers are unchanged.
3. Once prerequisites truly pass, run one actual complete broad integration
   including every existing MDK/fuzz/matrix/family and required result; inspect
   exact-source original receipts and independent gate/accounting. Only then
   sequence a second distinct complete integration after review. Repeated/nested
   jobs or skipped negatives are not integrations.
4. Review20 representative real PR comparisons (existing candidates only if
   genuinely qualified); classify differences as prerequisite-only build,
   justified unaffected omission, duplicate removed, missing coverage or
   deliberate profile/advisory distinction. Every legacy-only detected failure
   requires investigation and a regression. Cohort scarcity stays explicit.
5. Collect **at least five paired samples per representative condition** for
   Grotto-only, isolated GH UI, MLS, shared-core, shared-store/media, graph-change
   and unknown-path changes, **clean and incremental** (seven×two×five =70 paired
   samples across14 conditions, not70 invented PRs). Bind SHA/runner/architecture/compiler/profile/
   tools, targets/fixtures/expensive checks, phase timings, cache/retry state,
   aggregate runner-minutes and actual critical path; report median/range/IQR.

| Performance gate | Required | Current qualified samples |
|---|---|---:|
| Grotto-only aggregate runner-minutes | median reduction≥30% vs legacy triggered workload |0|
| Isolated non-MLS GH | reduction≥25%, zero unrelated live MDK executions |0|
| Representative critical paths | regression≤10% |0|
| Broad median | regression≤10%, or explicit measured coverage justification |0|

Partial nested-job timestamps are now reproducibly measured, but are explicitly
**not** billed runner-minutes, phase measurements, a dependency-DAG critical
path, a complete workload or paired savings. Five arbitrary nested jobs cannot
fill a matrix cell. Preserve correctness if savings miss targets. Protection,
merge-group, selected-skip/cancellation and operator/no-gap cutover still need
the original acceptance; W12/defaults/publishers/triggers/schedules stay unchanged.

## Tool, verification and retained receipts

```sh
python3 scripts/w11-evidence.py replay --repo "$PWD" --prs /path/to/real-pr-api-array.json --output /new/replay-dir
python3 scripts/w11-evidence.py inventory --inventory tests/selection/inventory.json --output /new/gap-dir
python3 scripts/w11-evidence.py timings --jobs /path/to/exact-attempt-jobs.json --output /new/timing-dir
python3 scripts/w11-evidence.py statistics --samples /path/to/observations.json --output /new/statistics-dir
```

No operation grants acceptance. New output paths prevent overwrite; raw input,
runner/Python fingerprints, commands, failed outputs and hashes are retained.
Ambiguous/rebased/squashed PR-parent binding refuses rather than guessing. The
replay disables inherited hooks/CRLF conversion and Python bytecode caches,
checks original source input bytes before/after and never applies today's policy
to old paths. `statistics` requires five finite nonnegative observations but is
labelled unattested descriptive statistics, not a five-sample acceptance cell.

Verification: seven focused evidence tests (including actual local Git branching,
merging and subprocess replay, whitespace-only instability, failed-output
retention, inherited caller Git-routing/index protection, owned process-group timeout
termination, reused-output refusal and malformed/missing job timestamps/pages);
completed skipped jobs with null timestamps are accounted without duration;
20 existing Git-range tests;42 existing workflow tests; four repository static
checks; fresh native Debug/Ninja configure and all-default-target build pass.
Native root Groundhog default OFF; safe `libsecret` / `secret-service` backend
flags; no keychain defaults/search lists changed. Pinned submodules initialized.
Root CTest NOT RUN; native build is a quality gate, not selective/profile/hosted
acceptance. Post-build source manifest binds4867 files including the two new
implementation files; it is not retroactively called a pre-build input audit.
One bounded review (`untitled-chat-948846`) first encountered missing transported
files; its same-chat inline continuation found three actionable issues. Fixed:
strip inherited Git routing/config from every subprocess (external staged-index
bytes regression); signal the owned POSIX process group and reap its primary on
timeout, stop the
cohort and retain its checkout (child-retained-pipes/delayed-write regression);
account skipped jobs before timestamp checks (null-timestamp regression).
Process-group diagnostics are not kernel/native custody or general escaped-child
proof; unsupported non-POSIX execution refuses. No repeat review was requested.

Raw proof root: `/tmp/nostrc-w11-execution-20261023/`. Key frozen anchors:

- `final-api/queries.json`: SHA256 `86a6c0a59e09e4a3c7af8d75dba933777bca6b1025fefed2910c41d894869582`.
- The twice-per-PR replay before the additive inventory command:
  `replay-final/result.json`, SHA256 `7d1e2e168329bd33a20d0721f582f95412271f6b3ede5b011f75d9d6bcdd287c`;
  its own runner fingerprint is retained, not falsely rebound to later bytes.
- Post-build source manifest before documentation/inventory-command additions:
  `source-manifest.json`, SHA256 `4da468d14fb3b333a61a9d277fd887cd28819894e14f403ba8a26ce92513059a`.

Final reviewed/fixed implementation and actual diagnostics:

- `scripts/w11-evidence.py`: SHA256 `c6b32c2932a6afd23a86860984194db8d03278fd323b76d45088d1adaaf65f9f`.
- `tests/selection/test_w11_evidence.py`: SHA256 `8a6f546476d478d8a1b05f55e2fce5ee7b17538a7c52d5b12fcad5649a97f8fa`.
- `verified-replay/runner.json` binds the final runner above; actual42 original
  planner subprocesses again yield21 logical-only candidates,19 broad/72-check
  shared Signer/zero-check docs. `verified-replay/result.json` SHA256 is also
  `7d1e2e168329bd33a20d0721f582f95412271f6b3ede5b011f75d9d6bcdd287c`.
  Identical logical outputs across runner revisions do not count as more PRs.
- `verified-inventory/result.json`: SHA256 `1fd9577a16d2852adbb18e3f2e0754fae4e61ddce09fcf80f74581a221a900a1`;
  actual ten gaps/89 workflow rows/ordinary0-of33 ownership retained.
- `verified-timing-pr21/result.json`: SHA256 `383316a86879bba76d76deb27e01f37460a3d316b9b025fa6b7320e00aa43eb8`;
  all15 actual job records accounted,13 unselected skips,zero pending. Neither
  successful selector duration nor skipped workers repair the failed gate.

Final seven new,20 Git-range and42 workflow tests, four static checks and final
native incremental default build pass; source manifest comparison proves no
compiled-source inputs changed since the earlier fresh native build. Only the
two owned implementation files and two audit/ledger prefixes changed from that
post-build manifest; this new report was added separately.

The local handoff supplies the exact committed tip and complete proof manifest.
Earlier runs/failed outputs are preserved, not overwritten or counted again.
No all-source selector suite, live Linux/x86, broad/profile, release or operator
acceptance is claimed from these bounded gates.
