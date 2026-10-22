# Dependency-aware CI: finite closeout criterion register

## Source and disposition rules

**Integration is in progress, not accepted.** Current code checkpoint:
`eb799dbd7cd0a4f339180c643e8a4624f1265f8f`, based on
`dd949e4b7a2969e01ac5dea3f12bdde2cf22655b`. Preserving merges contain exactly:

- C `827e1c70ff17ff61747c7d42ff9633820fb0330d`, implementation
  `385a6cf2555756c3ccd46e13ff82bf8eb9a2c33d`, through `513999d373`.
- B `405c23b3f4bd6ab9d65818ee195e84ad18d5ad33`, preserving
  `1abee21ecbdbeabdf034444d77831e1d05289e35`, through `48965e6b5`.
- B keyring follow-through `82f79ae75667974591685e11343c9b4cb4053529`, through
  preserving merge `688135e9f46da39ad4aeffc398614193b84f2a9a`.
- Integration test correction `dd08636cc6e6f77316c4e6f3b2ec52237f9d3f58` and
  exact interface/default-contract reconciliation `012d0fcd8`.

- A metadata `c419d2ac6d55c64a5dde25db637384225b5d2134`, through preserving
  merge `38c942c647fef5262826bb65b77d4247bfdb3d80`.
- A test-owned connectivity follow-through `dfb15c8938dbc6467f76faa428a0aa772453494d`,
  through preserving merge `7cfabebcbefdd73180e07e3f9ddad6e1cfc26d9c`.
- Strict integration witness/default input owners `98dac8acb84cad44320f8c5d2f597fde9c7ad254`
  and exact Nak install repair `126e29b65d79b18e70ae51eea629461f6c8db7d3`.

- B production producers `0b9feea6d5b28f3a65df5c1ea930c505a687501f`, through
  preserving merge `e33602c9c468637c0350234fb79ca5c3810ede36`.
- Root producer/input-edge repair `e47b52946b0dc1ebe5ffcd637da4bcd6ba62d489`
  and exact source/derived-metadata renewal `eb799dbd7cd0a4f339180c643e8a4624f1265f8f`.

**C's additive Docker-resource lifecycle follow-through awaits its exact tip.**
No moving branch is accepted. Final combined review/gates wait for that handoff;
component and precondition evidence is not frozen-final-source acceptance.

This register maps the existing epic and all 34 children; it does not create a
replacement tracker or close any bead. The complete initial criterion text is
retained in `/tmp/nostrc-closeout-integration-20261022/criterion-register-initial.json`,
from official `bd export --all`. The original [execution-index Done when
contracts](../plans/dependency-aware-testing-ci-2026-10-15.md#execution-index)
and [criterion/source audit](dependency-aware-testing-ci-2026-10-18-acceptance-audit.md)
remain authoritative. Historical receipts below retain their original sources.

Dispositions are **PASS**, **FAIL**, **NOT RUN**, or **EXTERNAL BLOCKER** for the
specific criterion, not for an entire profile inferred from one successful cell.
Implemented membership, successful preaudit, physical prerequisite/default build,
postbuild/header audit, selected execution, lifecycle and complete eligibility are
separate cells. An implementation refusal is **FAIL / engineering**, not an
external acceptance excuse. A fixture/operator requirement is named explicitly.
Historical closed status is preserved; it is not fresh combined-source proof.

Internal CI/test/docs and metadata-only changes assess **no component version
bump**. The Groundhog resource dependency repair is **PATCH-class shipped build
correctness**, folded into the existing unreleased **0.12.0** per the manifest;
no second numeric bump or release-column change. The configure-template/witness
portion preserves generated header bytes, including escaping counterfactuals.
No policy/default, publisher/platform, schedule, protection, authority or release
cutover is approved. Exact-33 omissions/29 required omitted-target closures stay
incomplete, and no selected-only audit fallback is authorized. Original immutable
fixtures, review identities and failed attempts remain retained.

## Delivered components, not final integration gates

| Source-bound criterion | Disposition and evidence |
|---|---|
| C workflow/event/release/setup contracts | **PASS**, 75 focused tests at merge `513999d373`, zero skips. C's failed 613-case full run is historical, not a successful combined suite. |
| PR merge and annotated-tag workflow identity | **PASS**, 42 workflow tests at `dd08636cc`, including explicit API-head/tag-object workflow-SHA rejection before outputs. Guard unchanged. |
| B supported default-generator, non-test audit, capability and lifecycle components | **PASS at `405c23b3f`**: 622 selector, 65 actual Linux-arm64 component and 13 contained cases, zero skips; 13 shell/static commands. Keyring follow-through **PASS at `82f79ae75`**: 623 selector, 66 Linux, 27 contained (14 new keyring) and 13 shell/static commands, zero skips. These are component results, not current integration execution. |
| B native default build/full CTest | **PASS at `1abee21ec` only**: 434 pass, five visible skips. **NOT RUN** at final B tip or combined final source. |
| A full supported membership/default and mapped-input census | **PASS at `c419d2ac6`**: 610 selector tests, full ordinary pre/postbuild audits zero findings after actual default and mapped target builds, including omitted producers. Native 434 pass/five visible skips. These are A component results, not combined acceptance. |
| A causal runtime follow-through | At `c419d2ac6`, actual 346-case run retained **343 pass / three fail / zero skips**. At `dfb15c893`, five focused Linux cases **PASS**, including both independent NIP29 registrations; no whole-346 rerun or retroactive green. Native ARM64 NIP55L three-case **PASS** is diagnostic only. |
| Current combined full selector/six shell/statics/lint | Frozen `98dac8acb` selector run retained **14 failures / 12 errors**. Exact projection/source-enrollment and harness-environment seams have 111 focused cases **PASS**, zero skips, in `126e29b65`; final full suite/shells/statics/lint **NOT RUN**, awaiting C's stable lifecycle tip. |
| B known production producers | **PASS at `0b9feea6d`**: actual production libgo macOS/Linux defaults, full postbuild and command execution, respectively 553/455 input hashes. Full selector checkpoint 643/644 retained one parent-owned explicit-input enrollment error; that input is now enrolled. Unknown custom/utility producers still refuse. |
| Root source producers and exact ordinary renewal | **PASS** 43 focused cases, zero skips, at `eb799dbd7`; full fresh core preaudit and all three root default producer bindings have zero findings/gaps at `e47b52946`. Ordinary source refusals at that source are retained; narrowly reviewed renewal is detailed below. Final full ordinary audits/build/execution remain **NOT RUN**. |
| Current combined safe native fresh default build/full CTest | **NOT RUN**; earlier native receipts are not relabeled. |
| Actual production libgo backend default route | **PASS at `38c942c64`**: public plan/run `--configure --phase all --parallel 2`, actual prerequisites/default 42-target closure, full registration/input audits and all 33 selected CTests. Physical default argv includes `--config Debug --parallel 2`. `diagnostic_eligible=true`, `complete_gate_eligible=false`; not native x86/hosted or root/core parity. |
| Current combined fresh authored Linux full audits and actual supported core/default execution | **NOT RUN at final source**. A's source-bound full audits and the actual libgo route above do not substitute for fresh integrated root/core producer/default/postbuild execution. |
| One bounded review of novel integrated seams | **NOT RUN**; parent C review found no immediate blocker, but is not the final integrated review. |

B component proof SHA-256:
`da9fe52856ca88b41a7ed9fe6ed23a5ae5d918c53c04b8269de07d8dd237908e`.
All **66** adjacent `SHA256SUMS` artifacts and **12** committed source-file
hashes were independently verified. B keyring proof SHA-256
`3cabac2702e3708ee727e07819bd7dae76f7edadfb0676067c4f0946fb2231f9`
and its **34** adjacent artifacts were also verified. C's **37** workflow records were independently
matched to actual job/workflow bytes. All **27** existing fixture/golden hashes
are pinned at the integration base, not refreshed to make assertions pass.
`012d0fcd8` adds exactly three B/C runtime-input paths, the reviewed 37 workflow
records, two ordinary tool-capability/profile-hash changes, four explicit default
contracts, and one conservative global rule for C's non-prose hosted-evidence
JSON. Normal inventory validation passes; no historical source-review map,
omission set, named prerequisite contract or original sanitizer authority changes.
`98dac8acb` adds a pinned test-only reverse projection for those exact changes,
A's complete inputs, four explicit Debug default checks and their actually
observed Ninja source owners. Unknown profile, authority, source control, job,
command or input edits refuse; old fixtures and A's consolidated witness remain
byte-identical. The supported-capability remainder now distinguishes implemented
executable fingerprints from signer approval and unsupported custody.

A metadata checkpoint SHA-256:
`7042561a535cd2a3d01feb64456c60502c74f7b3b1be51956986056dfd701872`;
A runtime interface SHA-256:
`931bf3be5f87780de132eea7b512ecd824224b08c8d2d2b4f4489c51cf63608b`.
All **30 + 30** respective artifacts were independently verified. The changed
remote-feature C test is not among either historical 193-key source-control map;
no source-control or omission hash was refreshed. Actual libgo receipt SHA-256:
`4fa84cc41606b4ab2e884101d5ec25da1a301c80ea1f4b1934de7db121db9bf7`.

The real Nak bootstrap exposed upstream's default `nak version debug`, despite
Go module metadata proving `github.com/fiatjaf/nak v0.19.7`. C correctly refused.
The exact install now supplies `-ldflags='-X main.version=v0.19.7'` to that same
pinned module, with unchanged producer/consumer version checks. The real C
producer then passed for both ordinary profiles with binary SHA-256
`cb7c7df16bf77d565837d1f4eae41d4961df2ec1d603ab51e9f95c10b2d9b337`.
Only the affected worker job's source/envelope hashes were refreshed, bound to
the literal reversible one-line repair; the other C records remain exact.
Bootstrap Go 1.24.13 required automatic toolchain selection (actual Go 1.26.10);
local-only and no-network bootstrap refusals are retained, not profile failures.
These producer receipts use an explicit **local** run context and attest
executables only, never a hosted run, bus/display/service or profile acceptance.

The retained 658-case selector checkpoint failures include exact historical
negative-message/source-enrollment mismatches and a host XPC marker differing
from the fixture CLI's safety environment. Matching that environment makes the
phase cases pass without relaxing source/environment identity guards. All
original failed logs remain; the 111 focused fixes are not a full-suite pass.
Receipts and comparison manifests:
`/tmp/nostrc-closeout-integration-20261022/`.

## Precise producer and candidate renewal

B's production proof SHA-256 is
`c7c13c290494c6185e08cb95479dc60c039f5bd6a8230969adbf8182b543e0d6`.
All **6,072** entries in its retained manifest were independently hashed, zero
mismatches; manifest SHA-256
`ff13d25cec90d5123d9510d12a615ee9d4cedcbf7a9d0879d5ab9218ad158050`.
Its actual proof source is `4b53a627062bb2493b9f2833cb704b8500799bf5`,
not the final integration. No component result is relabeled as final acceptance.

At `e47b52946`, the actual fresh core preaudit passes; all three root default
producer bindings have zero gaps. The ordinary Groundhog/Grotto source guards
correctly refused the changed CMake control. Original configure, refusal and
first candidate-mismatch logs remain under
`/tmp/nostrc-closeout-integration-20261022/production-source-producer-preaudit-e47b52946/`.
The successful source-bound renewal precondition proof is
`control-renewal-derived-target-preconditions.json`, SHA-256
`457cb7015c0b1580641302d5366217ee98ba7a6ca166b27140958830ac689166`.
The approved renewal changes exactly:

- One value in each unchanged **193-key** source-control map:
  `gnome/groundhog/CMakeLists.txt`,
  `e9ec66371ad955063bd4cbe1e442c39fedec122040c9a18503adddb3afc667a7`
  to `0310724b36c40157385555e032a02519c13530edf5774f031176f92bb8e72d39`.
  The ten missing static resource dependencies are now declared; immediate
  configure witnesses bind the actual source templates. The generated header
  remains **501 bytes**, SHA-256
  `b80e3b7880520c1a68be3054bd378ae91992b9351941a12ae99b3620396ca541`.
- Candidate hash
  `1b388f407707962875055bb166ba8d01b295330621469e5f6527d5c218b4e402`
  to `2f4632d4d10aaa65e3236516dd0b1f567408347604bd13fdda2b5ab841daa932`.
  The only target-record addition is **80 derived `build_path="{build}/libgo"`
  entries across the same 33 registrations**. Removing only that field from
  actual fresh candidates byte-exactly recovers the old hash; reconstructing
  the untouched packed historical fixture and adding only that field independently
  yields the new hash. No production hash algorithm is weakened.

Registration universe, primary/required closure, exact **33 omissions / 29 omitted
producer targets**, toolchain and every other contract/review field are unchanged.
The original immutable exports/hashes remain. Architecture/prepared aliases are
not expanded. Test-only reverse projection checks the whole exact current
contract before reversal; wrong candidate path, unrelated source, architecture,
universe, closure and toolchain mutations refuse. The 43 focused cases pass with
zero skips. This is permission for the exact renewal, **not** a replacement for
fresh final full input/header/postbuild audits and actual execution.

## Runnable-capability and build cells

| Cell | Implemented membership/setup | Physical runnable proof / disposition | Actual remaining action |
|---|---|---|---|
| Single-config native Ninja default scope | B implements `kind=build_contract`, `build_scope=default`, `build_targets=[]`, exact `cmake --build {build} --config Debug` and authored build parallelism. All eligible defaults contribute full non-test audits even when unselected; new default checks own actually consumed source families, not invented graph dependencies. | **PASS** component positives/negatives and actual public-CLI production libgo prerequisite/default/audit/33-test route at `38c942c64`. **NOT RUN** final integrated root/core route. | B's known-production implementation and root source/input repairs are merged. After C freezes, run actual configure/resolve/full preaudit/default and named prerequisite builds/postbuild audit/selected execution. Never substitute `signer-tests`, selected-only audits or a fabricated File API `all` target. |
| Known production versus unknown default producer | B `0b9feea6d` binds actual known resource/schema/Blueprint/gdbus/copy/custom-utility producer shapes to source/tool/argv/inputs/outputs and configured trace. Unknown shell/launcher/dyndep/utility shapes still refuse. | **PASS** actual libgo default/postbuild/command at B's proof source; root core/Groundhog/Grotto producer bindings have zero gaps at `e47b52946`. Final production root execution **NOT RUN**. Unknown shapes are bounded unsupported cells, not implied parity. | Run full input audits and physical production defaults/postbuild at frozen source. Preserve unknown refusals; no omission or fabricated provenance. |
| `nak` executable | C produces executable-only fingerprints; B consumes path/hash/version and exact source/profile/plan/workflow/run/attempt for commands and CTest. Exact pinned-module version-link repair and source-bound local producer positives are retained above. | **PASS** component reader and real producer proof for both ordinary profiles. **NOT RUN** final full ordinary-profile lane. | Ordinary declarations are integrated; retain actual fingerprint consumption and separate bus/display/keyring custody. No full-profile or hosted acceptance from tool installation. |
| `gnome_keyring` executable | B `82f79ae75` provides actual executable fingerprint only, not bus/display/service setup. Existing CTests own private HOME/bus/supervised Secret Service, including service-initially-absent negatives. Only the ordinary Groundhog/Grotto profiles add `nak` and `gnome_keyring`, with their exact current profile hashes updated; historical review fields remain unchanged. | **PASS** component executable/fixture-ownership positives and negatives. **NOT RUN** final ordinary-profile execution. | Retain executable proof separately from bus/display/service lifecycle. No ambient/global Secret Service authority or executable-as-isolation inference. |
| Contained keyring + private-bus command | B `82f79ae75` owns throwaway-service setup for commands explicitly requiring both capabilities, without taking over CTest fixture resources. | **PASS** 14 actual new contained cases: secret create/read/delete, cancellation, exit 19, missing/nonserving/preexisting service, environment redirection, HOME/control symlinks and namespace cleanup. **NOT RUN** final production-profile lane. | Preserve exact PreparedContext/control/source/profile/plan/CID/run binding and cleanup evidence; no native arbitrary-tree or whole-profile promotion. |
| `display` / `private_bus` | Separate authored requirements; neither follows from `nak` or keyring executable presence. | Each selected lane requires actual supported wrapper/setup proof. **FAIL / engineering** for unsafe mixed non-CTest/native wrapper custody. | Preserve prelaunch refusal while the unsupported custody seam remains. Do not drop requirements or silently unwrap. |
| NIP55L caller identity under Rosetta | Fixture authorizes the actual test executable; emulated daemon observes `exe:/run/rosetta/rosetta`. Approval wait is correct. | **FAIL** retained emulated execution; **EXTERNAL BLOCKER** for native Linux x86 proof. A's native ARM64 three-case **PASS** is diagnostic, not the authored x86 cell. | Run exact source on native Linux x86/hosted infrastructure. Never authorize the emulator shim, weaken attestation or relabel the timeouts as missing keyring capability. |
| NIP29 remote-feature connectivity fixture | A `dfb15c893` owns the in-process network monitor, proves offline queued/zero-sign/zero-publish then online acceptance, and retains the original deadline and signed-event assertions. | **PASS** five focused Linux registrations, including both independent NIP29 cases, at A's exact tip. Historical whole-346 failure is retained; final combined targeted verification **NOT RUN**. | Verify the affected exact registrations at frozen final source; do not rerun the whole 346 or change product connectivity/deadlines. |
| `nip5f_tcp` provisioned signer | Test consumes real TCP endpoint/token, handshake and `get_public_key`; it is not `test_nip5f_loopback`. Ordinary profiles are not declared capable from tool/loopback evidence. | **EXTERNAL BLOCKER / fixture and approval** unless a genuine isolated setup is explicitly delivered. Exit 77 is not passing execution. | Requires approved `ENABLE_TCP_IPC=ON` configuration, isolated signer identity/runtime token readiness and explicit approval for the always-prompted unidentified caller. No approval bypass, fake signer or ambient identity. |
| Native/mixed-wrapper arbitrary descendants | Direct-child reap is truthfully distinct from arbitrary-tree cleanup. Explicit diagnostic ptrace/contained routes retain their own limits. | **EXTERNAL BLOCKER / platform restriction** for native arbitrary-tree custody without a real kernel primitive; mixed-wrapper implementation remains unsupported and refuses. | Retain negative cleanup evidence, escaped-child/sibling survival tests and prelaunch restrictions. No unsafe PID/name workaround or native parity from contained tests. |
| Original MDK image-build/BuildKit lifecycle | C is implementing additive `images [--probe]` scoped Docker-resource cleanup with unique explicit builder ownership; legacy run API and prepared-peer route remain distinct. | **NOT RUN at integrated source**, awaiting C's exact tip, five real Docker lifecycle proofs and bounded review. The existing legacy run API still cannot claim complete arbitrary-tree cleanup. | Verify actual private-image positive/negative, partial/cancel/drain and owned CID proofs after exact merge. Preserve incomplete broad cleanup and scoped errors; no default builder or global prune. |
| Hosted broad union, W11 and operator cutover | C authors the broad union and retains failed family accounting. | **EXTERNAL BLOCKER** for qualifying hosted/operator acceptance; local fixes are not hosted samples. | Collect reviewed distinct PR comparisons and successful complete broad integrations, then explicit operator approval. Current dated snapshot remains 0/20 and 0/2. |

NIP-5F source: `nips/nip5f/tests/test_nip5f_tcp.c:24-57` and
`apps/grotto/CMakeLists.txt:341,437-438`. Missing endpoint/token returns 77;
loopback success does not satisfy the provisioned-signer cell.

## Every authored profile: membership is not runnable acceptance

This census names all 13 authored profiles. **Final** refers to source frozen
after C's pending exact handoff, so every final execution/audit cell below is
currently **NOT RUN**, even where an earlier component proof passed. Local amd64
containers on the ARM host are emulated; they are not native Linux x86 acceptance.

| Profile | Membership / full input audit evidence | Default, runtime or capability cell | Finite remaining proof or blocker |
|---|---|---|---|
| `core.linux` | A complete primary mapping; fresh full preaudit **PASS**, zero findings at `e47b52946`. | Actual production default producer binding has zero gaps. Broad selected execution correctly requires unavailable `display`, `private_bus`, `signer_endpoint`. | Final physical default/prerequisite/postbuild plus actual supported NIP14 local explicit-impact route **NOT RUN**; retain broad-capability refusal separately. |
| `core.linux.asan` | Authored membership includes three explicitly excluded legacy registrations; no generic omission authority. | Private bus is declared; provisioned signer is not. No ordinary subset or prepared alias. | Final authored audit **NOT RUN**; whole-job fuzz/setup/default parity and exact exclusions must remain visible. |
| `core.macos.asan` | Native ARM64 profile, three explicit legacy exclusions; not root-native equivalence. | Private bus does not imply signer or arbitrary-tree custody. | Final profile execution **NOT RUN**; native arbitrary-tree platform restriction and signer approval remain separate. |
| `grotto.linux` | A full mapping/default+mapped pre/postaudits **PASS**; precise ordinary source/candidate renewal above, exact33 incomplete. | Real Nak/keyring executable components **PASS**; bus/display/service and provisioned TCP signer are separate. | Final full audit/default+mapped/postbuild **NOT RUN**. Whole-job Meson/deb/shell-extension/TSan variants are not established by CTest membership. |
| `grotto.linux.asan_ubsan` | A fresh full authored preaudit **PASS**; not granted ordinary33 omission authority. | Display/private bus declared, signer approval still required for that cell. | Final audit/execution **NOT RUN**; no new ordinary default contract or whole-job extras inferred. |
| `groundhog.linux` | A full mapping/default+mapped pre/postaudits **PASS**; exact193-key/33/29 renewal above. | Actual 346-case history is 343 pass/three failures; five causal NIP29 cases pass at A's fix. Rosetta caller-identity failures retained. | Final default+all mapped/omitted producer builds and full postbuild **NOT RUN**; targeted NIP29 verification, not whole346 rerun. Native x86 NIP55L remains **EXTERNAL BLOCKER**. |
| `groundhog.linux.sanitizers` | Original immutable full-partition authority remains bound to its exact old universe; fresh extra resource registration is not silently admitted. | No automatic new source/partition/architecture authority. | Final audit **NOT RUN**; expected source/universe refusal must remain explicit, not broad refresh or complete promotion. |
| `groundhog.mdk08` | Authored membership does not establish actual original-image execution. | C's new scoped image cleanup awaits exact merge; legacy run API and prepared peers are distinct. | Final original-image lifecycle **NOT RUN**; full profile keyring/Nak/signer setup and actual legacy-image dependency proof are not inferred. |
| `groundhog.mdk011` | Same original-route distinction, no prepared011 alias. | Additive scoped image cleanup is not complete arbitrary-tree custody. | Final image lifecycle **NOT RUN**; original dependency/build/runtime and explicit signer approval remain separate cells. |
| `groundhog.linux.arm64.prepared011` | Local-only contained aarch64 admission, not authored x86 or original Docker route. | Actual prepared-peer/CID evidence is bounded to that context. | Final fresh full audit/execution **NOT RUN**; no selected-only audit fallback or platform alias expansion. |
| `libgo.linux.asan` | Actual public-CLI prerequisite/default/full audits/all33 tests **PASS** at `38c942c64`; B production postbuild covers455 input hashes at its proof source. | Supported no-capability single-config route; incomplete diagnostic, not root parity. | Final source default/full postbuild/selected execution **NOT RUN**. |
| `libgo.macos.asan` | B production default/full postbuild/command **PASS**,553 input hashes at its proof source. |32 selected diagnostics, visible excluded stress; standalone ARM64 ASan/UBSan, never native root. | Final selected sanitizer execution **NOT RUN**; omission and incomplete flags remain required. |
| `static.policy` | Selector/command inputs explicitly enrolled, including producer helpers and witnesses. | Supported tool-backed static lane; missing tools cannot pass via skips. | Final selectors/six shells/four statics/full actionlint/ShellCheck/whitespace **NOT RUN**. |

## Every existing bead: finite criterion accounting

The table separates recorded historical closure from unfinished final-source
execution. **NOT RUN** means no final combined-source result yet, not that the
historical implementation has disappeared. Each grouped criterion must be
satisfied in full before its bead closes; passing a subset does not close it.

| Bead | Existing criterion group | Current disposition / remaining proof |
|---|---|---|
| `nostrc-0cfsg` | Execute W1-W12 with affected/premerge, broad master/release/manual, fail-broad unknowns, exact-SHA safety/advisory policy. | **FAIL / incomplete implementation and acceptance**. Children below remain open; W11 **NOT MET**, W12 **NOT RUN**. |
| `.1` | Schema/range/golden/aggregation: rename/delete/gitlink/shallow/newline, selected skip/cancel/stale identity, release applicability, explicit unsupported roots; real module delegation. | Historical **PASS / CLOSED**. Final combined selector **NOT RUN**. |
| `.2` | Fresh/stale/failed configure; standalone/root export; fixtures/reset/model-GUI split; skip/dirty-cache/missing-edge negatives; actual focused build/test; no silently unowned supported registrations. | Component **PASS**; integrated authored full-universe ownership/audit/build/execution **NOT RUN** at frozen final source; A/B metadata and engine are merged. Native CTest alone cannot satisfy it. |
| `.3` | Shell regressions; actual shared plan/resolve/run; unknown broadening; selected capability refusal; cross-checkout locks/cancellation; sanitizer configuration parity. | Component **PASS**; final shared-profile execution/parity **NOT RUN**; unsupported custody remains **FAIL / engineering**. |
| `.4` | Workflow lint/static and PR/merge-group/tag/dry-run/result routing; no publication from partial validation; executable broad replacement; duplicate triggers explicit. | C finite contracts **PASS**. Full-source/profile broad execution and exact-tag publisher acceptance **NOT RUN**; existing failed broad attempts remain **FAIL**, not erased. |
| `.5` | Source-grounded baseline/commands; accurate plan/AGENTS/TESTING; combined gates/review; explicit external rollout prerequisites. | Register/docs integration in progress. Final gates/review/complete criterion reconciliation **NOT RUN**; dated hosted counters are separate. |
| `.6` | Full actionlint with ShellCheck, unchanged workloads/artifacts/profiles/advisory behavior. | Historical **PASS / CLOSED**; B component lint **PASS**. Final lint **NOT RUN**. |
| `.7` | Truthful libgo32 diagnostic; no stress/complete promotion; real command/build-contract absent/wrong-source/input/capability/cancel/nonzero/evidence negatives; full regressions. | Component **PASS**; actual integrated default/core route and final regressions **NOT RUN**. Complete promotion remains forbidden. |
| `.8` | Full sanitizer phase/config/environment parity; concurrent roots; cancelled/partial MDK images and sibling cleanup; real safe evidence distinguished from simulation. | Component regressions **PASS**; original MDK lifecycle and unsafe wrappers **FAIL / engineering**. Final supported sanitizer route **NOT RUN**. |
| `.9` | Every job/matrix/check mapped or explicitly tracked unsupported; exact native/Linux registrations/semantic edges; isolation goldens; no unexplained omissions or fake enforcement readiness. | A metadata/runtime tips are merged. Source-bound full census/default+mapped pre/postaudits **PASS** at A; exact historical integration projection focused regressions **PASS**. Final combined census/audits/goldens **NOT RUN**. Runnable capability cells remain separate. |
| `.10` | Actual CMake 3.22/3.25 nested/root/standalone metadata and fixture/DEPENDS parity, current suite, no fallback. | Historical **PASS / CLOSED**. Final current regressions **NOT RUN**; no new claim of old-tool reruns. |
| `.11` | Source-grounded historical heartbeat cause; deterministic verification where established; unresolved scheduling evidence explicit; original guard retained. | **FAIL / unresolved cause**, not a proved external-toolchain excuse. Later passing samples do not establish causality; no threshold/exclusion/retry waiver. |
| `.12` | Truthful MDK descendant cleanup after setsid, or stop claiming complete cleanup. | Historical finite refusal correction **PASS / CLOSED**. Legacy `cleanup_complete=false` is preserved, not arbitrary-tree success; wider `.8` remains open. |
| `.13` | Nested negative diagnostic eligibility cannot be hidden by omitted outer field. | Historical **PASS / CLOSED**; final aggregation regressions **NOT RUN**. |
| `.14` | Non-UTF8/raw output survives; decoding failure finalizes negative receipt/log. | Historical **PASS / CLOSED**; B byte/lifecycle component tests **PASS**; final suite **NOT RUN**. |
| `.15` | Actual split configure/build/test environments; drift/unknown/malformed/template/cache negatives; test-only vars absent earlier. | Historical **PASS / CLOSED**; final actual supported-profile phase proof **NOT RUN**. |
| `.16` | Actual exact-tree/dirty command outer route; source/input/capability/cancel/evidence negatives; shared sanitizer route; six shell/statics/lint; no bypass. | Historical bounded **PASS / CLOSED**; final combined route/gates **NOT RUN**. Unsafe wrappers remain refused under `.23`. |
| `.17` | Production selection command has truthful full inputs/source; missing/moved/new helper/drift/wrong-source negatives; no skip masking; normal versus strict inventory honesty. | Historical **PASS / CLOSED**. A/B/C and integration source inputs are reconciled, including explicit keyring helper/custody test enrollment; focused positives and refusal negatives **PASS**. Final full production command **NOT RUN**. |
| `.18` | Absolute fixture paths cannot collide with a real `/source`; omitted stress and unknown registrations stay audited. | Historical **PASS / CLOSED**; final fixture regressions **NOT RUN**. |
| `.19` | Diagnose actual SIGTERM lost output without invented readiness cause or weakened raw-byte assertions. | Historical **PASS / CLOSED**, with distinct failure history retained; final lifecycle regressions **NOT RUN**. |
| `.20` | Real deterministic ready/emit handshake before cancellation/timeout; retain raw bytes and final negative receipt; no sleep/retry substitute. | Historical **PASS / CLOSED**; B Linux component byte/cancel tests **PASS**; final combined regressions **NOT RUN**. |
| `.21` | Every authored profile maps to independent worker/needs/conclusion; honored native/sanitizer setup or named refusal; supported lanes still dispatch; no cutover. | Historical finite **PASS / CLOSED**. C event contracts **PASS**; final inventory/worker census **NOT RUN**; hosted execution not inferred. |
| `.22` | Before-launch refusal for selected non-CTest display/private-bus and mixed lanes; preserve legacy CTest; no dropping/receipts/unwrap; supported unwrapped/shared cases and shell/statics. | Historical bounded refusal **PASS / CLOSED**; final refusal regression **NOT RUN**. This never closed safe custody `.23`. |
| `.23` | Real macOS/Linux command+CTest custody, escaped TERM-ignoring children, unrelated sibling survival, raw evidence; no numeric/name/sleep substitute. | B truthfulness/component negatives **PASS**. Native arbitrary-tree custody without a real kernel primitive is **EXTERNAL BLOCKER / platform restriction**; mixed wrappers remain unsupported and refuse. Do not close from contained/ptrace diagnostics or substitute unsafe numeric PID cleanup. |
| `.24` | Identify or faithfully retain failing full-gate command/trace on recurrence; deterministic fix only if warranted; distinguish later diagnostics. | Historical finite retention **PASS / CLOSED**; no invented original cause or new full-gate success. |
| `.25` | Correct visible `snprintf` declaration; build all three fuzz targets and retain actual corpus guards. | Historical finite compile repair **PASS / CLOSED**. JSON corpus failure stays under `.29`; final hosted fuzz **NOT RUN**. |
| `.26` | Actual regular BP metrics/emission/parser workload with original parameters, not shortened or missing metrics. | Historical finite repair **PASS / CLOSED**; final hosted broad workload **NOT RUN**. |
| `.27` | Homed mandatory unit registrations and original filter, explicit test opt-in, no empty-match success. | Historical finite repair **PASS / CLOSED**; C preserves full homed broad call. Final hosted homed workload **NOT RUN**. |
| `.28` | Compatible default UI widget/build with behavior preserved, approved shipped fix and version assessment, no target suppression. | Historical finite shipped repair **PASS / CLOSED**; final safe native build **NOT RUN**. No new product change authorized here. |
| `.29` | Verified JSON-GLib remedy for real malformed-input leak; original corpus, leak detection and duration intact. | **EXTERNAL BLOCKER / dependency remedy** and retained real corpus **FAIL**. No dependency patch/suppression/corpus reduction in this integration. |
| `.30` | Positive and pre/postaudit/build/capability/source/cancel CLI receipts preserve valid lane identity/raw bytes; invalid plans invent none; aggregation retains failure and rejects promotion. | Historical **PASS / CLOSED**; B failure components **PASS**; final combined negatives **NOT RUN**. |
| `.31` | Real setup/build probes and worker routing; fresh Grotto registrations/target/input mapping and supported actual cases; isolation/MLS selection; every unsupported step explicit. | A membership/interface and test-owned connectivity are merged; B executable/contained keyring components and real C executable producer **PASS**. Final integrated profile proof **NOT RUN**. NIP55L Rosetta identity failures require native x86 proof, not weaker approval. Existing keyring CTests retain resource ownership. |
| `.32` | Reproduce tracked generated-source mutation; actual build-tree outputs/install paths; source stays byte-clean; no fake platform claim. | Historical **PASS / CLOSED**; final native/default build source cleanliness **NOT RUN**. |
| `.33` | Physical CLI build2/test1 and target-only contract; fallback/legacy; type/override/resolved/receipt/argv contradictions; source/capability/failure/cancel negatives. | Historical **PASS / CLOSED**; B physical argv negatives **PASS**; final combined phase proof **NOT RUN**. |
| `.34` | Authorized bounded default-generator contract; exact fresh provenance; unknown custom/default refusal; full non-test audits and negative lifecycle proof before admission. | B supported component scope and actual integrated production libgo route **PASS**. B's known-production support and root source/input repairs are merged. Actual B libgo default/postbuild/command and root producer preconditions **PASS**; final fresh core/default/postbuild/selected execution **NOT RUN**, awaiting C freeze. Unknown shapes remain explicit refusals. No `all` target or selected-only approximation. |

## Workflow-SHA semantics and hosted acceptance

Current [GitHub variables](https://docs.github.com/en/actions/reference/workflows-and-actions/variables)
define `GITHUB_WORKFLOW_SHA` as the workflow commit. [Local reusable calls](https://docs.github.com/en/actions/how-tos/reuse-automations/reuse-workflows)
use the caller's commit, while the [reusable context](https://docs.github.com/en/actions/reference/workflows-and-actions/reusing-workflow-configurations)
retains the caller and [PR events](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#pull_request)
identify the tested merge, not API head. **Inference scoped to these relative
same-repository calls:** comparing the executing workflow commit with the peeled
tested commit is appropriate; a manual caller at a different revision must
refuse before dispatch. Annotated source tag objects peel; workflow identity
must be a commit, not the tag object. This is documented-contract/local regression
proof, not a hosted run or arbitrary external reusable-workflow attestation.

The latest reviewed hosted snapshot is **2026-10-22T11:16:15.818726Z**, not a live
counter: **0/20** distinct qualifying reviewed PR comparisons, **0/2** successful
complete broad integrations; **43 failed / 1 cancelled / 2 queued of 46 roots**.
See the [dated ledger](dependency-aware-testing-ci-2026-10-18-run-ledger.md#batch-c-frozen-snapshot--2026-10-22t111615818726z).
No local component tests, nested jobs, retries, parent colors or omitted/partial
receipts advance those counters. **W11 NOT MET / W12 NOT RUN.**

## Tracker and ownership

Official all-record export at **2026-10-22T14:43:45.766462Z** retained **1,552
records (1,515 issues + 37 memories), 547 comments and 1,147 interactions**,
including 16 root comments and four root interactions (`int-993d1344` is this
integration checkpoint). Every base/initial/previous record identity, comment,
complete memory record and prior interaction was independently verified preserved.
The full official interaction stream was copied without semantic editing. Proof:
`/tmp/nostrc-closeout-integration-20261022/tracker-preservation-after-a.json`.
This is a checkpoint snapshot, not a live total; final B/gate comments still
require another official export at final closeout.
`bd dolt pull` still refuses **Error 1105: no remote**, not successful sync.
No direct semantic JSONL edits or broad bead closure. Commits remain local;
parent owns landing and cleanup.
