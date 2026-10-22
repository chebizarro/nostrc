# Dependency-aware CI: finite closeout criterion register

## Source and disposition rules

**Finite implementation closeout is verified; rollout is not accepted.** Tested code freeze:
`3ef5cbf9cc903ce5f91043d7040461bd1fef5e9a`, based on
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

- C additive original Docker lifecycle `0c79eff778fa733066df9aef369baa86a0e85356`,
  through preserving merge `e6134fb1b9315388d29620cd9069ba8d04aaa4af`.

**All implementation lane tips are merged.** No moving branch is accepted.
Final combined gates and three supported production routes passed at the tested
freeze below. Component evidence is still not whole-profile or hosted acceptance.

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

## Final integrated evidence — 2026-10-23 local date

All results below bind tested code `3ef5cbf9cc903ce5f91043d7040461bd1fef5e9a`;
logs use UTC on October 22. Subsequent closeout documentation/tracker changes are
not retroactively included in that source identity. No code changed during the
parallel runs. Source, generation/control, phase, prerequisite and execution
receipts are fresh. Production builds reused only physical artifacts in owned
roots with unchanged guest source/build paths; these are **incremental**, not
cold builds or reuse of old receipts.

| Gate / route | Result and boundary |
|---|---|
| Full selector | **PASS:725/725, zero failures/errors/skips**, including six real private Docker lifecycle cases. |
| Six shell suites, four statics, inventory validation, actionlint, ShellCheck, whitespace | **PASS: all15 combined gates** including the selector. |
| Safe native configure/default build/canonical vectors/full CTest | **PASS:18 canonical cases;434 pass/zero fail/five visible skips of439**. Private HOME, supervised Secret Service/bus cleanup and read-only keychain guards pass. Groundhog OFF, non-instrumented: not authored root ASan/native Groundhog acceptance. |
| Public standalone libgo macOS/Linux ASan routes | **PASS:32/33 CTests plus actual default contracts**, full postaudits553/467 input hashes. macOS stress remains not executed; both receipts are complete-gate ineligible. |
| Public `core.linux` | **PASS**, full pre/postaudits zero gaps, **6,834** postbuild input hashes, actual engine-owned default/prerequisite and selected `test_nip14`. |
| Public `groundhog.linux` | **PASS**, full pre/postaudits zero gaps, **5,818** postbuild input hashes, actual default/all mapped and omitted producers plus selected NIP14. Exact33 omissions remain not executed/incomplete. |
| Public `grotto.linux` | **PASS**, full pre/postaudits zero gaps, **6,704** postbuild input hashes, actual default/all mapped and omitted producers plus selected NIP14. Exact33 omissions remain not executed/incomplete. |
| Groundhog causal NIP29 verification | **PASS:five registrations, zero skips**: remote-feature matrix, NIP29, NIP29 service and their HOME/GStreamer fixtures. Raw targeted diagnostic, not another whole346 run. |
| All three production normalized receipts | **PASS** partial aggregation; complete-gate requests **refuse**. These are local explicit hypothetical NIP14-impact diagnostics under x86 emulation, not observed PR comparisons, native Linux x86, whole profiles or W11 samples. |

These three selected default/NIP14 routes have `required_capabilities=[]` and no
runtime capability-consumer evidence. Ordinary executable setup producers passed
separately; no Nak/keyring-consuming CTest, private-bus/service or whole-profile
success is inferred from these routes.

Evidence root: `/tmp/nostrc-closeout-integration-20261022/`.
`final-handoff/checkpoint.json` SHA-256
`02c708d150cb947f18cbfe62aa23990eeadd3b8bc2155a6e70101574abd298e8`;
`final-handoff/SHA256SUMS.gates` verifies381 artifacts, SHA-256
`bb650ec717923ea9f0ff67d6da536e78a9d226898d21a34d6a1de23fdd32b9ec`.
`final-handoff/production-receipt-verification.json` retains exact receipt,
postaudit, phase/profile and NIP29 JUnit hashes. Actual launch/configure/build/run
argv and timestamps are retained beside every route; no inferred process credit.

The finite repairs after the first integrated freeze are:

- `3233857b7`: owned anonymous Docker volumes are removed/verified without harming
  existing named siblings; new named/bind allocations refuse.
- `e8dbb80e6` / `84e731046`: nine root-generated headers and actual MDK-vector,
  async-storage and MLS consumers gain exact provenance. GNostr's existing-header
  reconfigure failure was reproduced, then its constant COPYONLY producer made
  unconditional; two fresh configure tokens preserve bytes/mtime and witnesses.
  The nostrdb private helper preserves old two-argument byte/compile compatibility;
  no vendor edit or duplicate configure producer. Gitlink remains
  `2d459e8fa9ba0af5af091c37920e5d2078d881ca`.
- `96ad818cc`: bind canonical generation-control inputs and configured CMake/Ninja
  identities at configure. Run the bound VerifyGlobs verifier with exit0 and empty
  output, then configured Ninja from the original cwd using an owned temporary
  entry containing only `include <original build.ninja>`. Exact no-work response,
  pre/post binding checks and exclusive engine build lock are required. Query
  evidence is **not physical coverage**. Missing executable, stale source, new
  glob member, changed verifier/manifest/generation and unknown control negatives
  pass; no graph filtering or unsupported `-d norebuild` flag.
- `1795a6166`: one combined Groundhog/Grotto postbuild repair: exact ten additional
  GResource bundle inputs,26 actual GNostr header consumers, async-storage/three
  MDK011 vector consumers, and the existing Marmot version-header producer witness.
  Old A zero audits covered CTest closures; new default non-test closures added
  these inputs. This does not relabel A's old proof as a regression. Witnesses and
  ownership preserve artifact bytes/configuration; no additional version bump.
- `3ef5cbf9c`: normalization now distinguishes exact selected inventory command/
  build-contract IDs from CTest registrations, verifies per-kind physical evidence
  and fresh resolved argv, and refuses forged kinds/IDs/membership/processes.
  Real ordinary-subset command/default positives cover both profiles. The33
  omissions,29 required omitted producer targets and193 source-control keys are
  unchanged. Only two newly proven default consumers update the current local-vector
  expectation; historical fixtures remain immutable.

Preserved failed checkpoints: `84e731046` Ninja freshness refusal;
`96ad818cc` full postbuild ownership/provenance failures; `1795a6166` public
normalization refusals and three stale historical assertions. The final external
verification script initially asserted that core's aggregate *requested-mode*
`complete` field must be false; its complete request already correctly failed.
Only that external assertion was corrected, with both logs retained and no code
change or physical rerun. No failure is retrospectively green.

The one Oracle review was at `fa884617b`, not an independent review of every later
line. Its Docker-volume finding was fixed and tested. Later exact source seams
received parent/B spot-checks and source-bound positive/negative regressions;
there was no second Oracle loop or whole MDK matrix rerun.

GNostr's unconditional generation is **PATCH-class**, folded into intended
unreleased final0.1.0 (only0.1.0-preview is tagged). Groundhog's ten GResource
DEPENDS correction is **PATCH-class**, folded into unreleased0.12.0. Other
byte-equivalent witness/template/ownership work and the final Python/test-only
validator fix require no additional bump; authoritative sources/release columns
and the user-dirty submodule remain unchanged. See `VERSION_MANIFEST.md`.

## Delivered components and preserved integration checkpoints

These rows preserve **pre-final, source-specific observations**, including their
then-pending integration gates. The final results above supersede those pending
labels; component scope and failures are not rewritten.

| Source-bound criterion | Disposition and evidence |
|---|---|
| C workflow/event/release/setup contracts | **PASS**, 75 focused tests at merge `513999d373`, zero skips. C's failed 613-case full run is historical, not a successful combined suite. |
| PR merge and annotated-tag workflow identity | **PASS**, 42 workflow tests at `dd08636cc`, including explicit API-head/tag-object workflow-SHA rejection before outputs. Guard unchanged. |
| B supported default-generator, non-test audit, capability and lifecycle components | **PASS at `405c23b3f`**: 622 selector, 65 actual Linux-arm64 component and 13 contained cases, zero skips; 13 shell/static commands. Keyring follow-through **PASS at `82f79ae75`**: 623 selector, 66 Linux, 27 contained (14 new keyring) and 13 shell/static commands, zero skips. These are component results, not current integration execution. |
| B native default build/full CTest | **PASS at `1abee21ec` only**: 434 pass, five visible skips. **NOT RUN** at final B tip or combined final source. |
| A full supported membership/default and mapped-input census | **PASS at `c419d2ac6`**: 610 selector tests, full ordinary pre/postbuild audits zero findings after actual default and mapped target builds, including omitted producers. Native 434 pass/five visible skips. These are A component results, not combined acceptance. |
| A causal runtime follow-through | At `c419d2ac6`, actual 346-case run retained **343 pass / three fail / zero skips**. At `dfb15c893`, five focused Linux cases **PASS**, including both independent NIP29 registrations; no whole-346 rerun or retroactive green. Native ARM64 NIP55L three-case **PASS** is diagnostic only. |
| Pre-final combined full selector/six shell/statics/lint | Frozen `98dac8acb` selector run retained **14 failures / 12 errors**. Exact projection/source-enrollment and harness-environment seams have 111 focused cases **PASS**, zero skips, in `126e29b65`; final full suite/shells/statics/lint **NOT RUN** at the stable combined source. |
| B known production producers | **PASS at `0b9feea6d`**: actual production libgo macOS/Linux defaults, full postbuild and command execution, respectively 553/455 input hashes. Full selector checkpoint 643/644 retained one parent-owned explicit-input enrollment error; that input is now enrolled. Unknown custom/utility producers still refuse. |
| Root source producers and exact ordinary renewal | **PASS** 43 focused cases, zero skips, at `eb799dbd7`; full fresh core preaudit and all three root default producer bindings have zero findings/gaps at `e47b52946`. Ordinary source refusals at that source are retained; narrowly reviewed renewal is detailed below. Final full ordinary audits/build/execution remain **NOT RUN**. |
| Pre-final combined safe native fresh default build/full CTest | **NOT RUN**; earlier native receipts are not relabeled. |
| Actual production libgo backend default route | **PASS at `38c942c64`**: public plan/run `--configure --phase all --parallel 2`, actual prerequisites/default 42-target closure, full registration/input audits and all 33 selected CTests. Physical default argv includes `--config Debug --parallel 2`. `diagnostic_eligible=true`, `complete_gate_eligible=false`; not native x86/hosted or root/core parity. |
| Pre-final combined fresh authored Linux full audits and actual supported core/default execution | **NOT RUN at final source**. A's source-bound full audits and the actual libgo route above do not substitute for fresh integrated root/core producer/default/postbuild execution. |
| C scoped original Docker-resource lifecycle | **PASS at `0c79eff77`**:30 focused tests including five real Docker cases, zero skips. Exact original matrix cancelled after1000s during MDK011 dependency fetch; cleanup took2.8s, owned builder/volume/registration absent and locks free. Final-tip original image acceptance **NOT MET**,08/09 **NOT RUN**; earlier three-version positives retain the pre-review helper identity. Legacy run/selector/native arbitrary-tree refusal unchanged; `.8` remains open. |
| One bounded review of novel integrated seams | **PASS / completed once**, chat `untitled-chat-C7641C`, actual frozen current source/diffs/new witnesses. One P1 anonymous peer-volume cleanup regression identified and corrected: journal/verify mounts and volume identity, `rm -f -v` plus anonymous absence, preserve existing named volumes and refuse new named/host allocation. Actual private Docker regression and36 focused cases **PASS**, zero skips. No second review or whole MDK matrix run. |

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
At frozen `fa884617b`, all701 selector cases executed with zero skips, including
five real Docker cases; **four historical assertions failed** (one old inline
header-generation literal, one unprojected CMake source hash, two ordinary
source-map comparisons). The exact approved template/whole-contract projection
repairs retain the immutable fixtures and unrelated-mutation refusals. All14
other combined gates passed. This failed full checkpoint remains retained under
`final-combined-fa884617b/`; it is not final-source acceptance. The review-volume
fix/historical focused confirmation is36/36 PASS, zero skips. Its initial syntax
and fixture-directory failures are retained separately, not erased.

The integrated review is text-only, scoped to selected current diffs/source;
omitted workflow callers/full inventory limit independent end-to-end claims.
Runtime/full-input/production proofs are still independent required gates. Review
export: `prompt-exports/oracle-review-2026-10-23-021100-untitled-chat-c7641c-a39c.md`
in the main checkout. C's own earlier component review is not counted as this
integrated review.

Receipts and comparison manifests:
`/tmp/nostrc-closeout-integration-20261022/`.

C lifecycle handoff SHA-256:
`36efd3b6766b85ee6a6dbe48b4467e33fb268cd3e71b9b865f231c429843656a`;
all18 referenced source/evidence files were independently hashed. Verification:
`/tmp/nostrc-closeout-integration-20261022/C-lifecycle-handoff-verification.json`,
SHA-256 `b1516f6862d2d603468b0acfb067a888050ca092499b8d377460567f2e5a3c2d`.
The existing three C-owned files are already explicit selection-contract inputs;
no inventory broadening or new workflow fingerprint renewal was needed.

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
| Single-config native Ninja default scope | B implements `kind=build_contract`, `build_scope=default`, `build_targets=[]`, exact `cmake --build {build} --config Debug` and authored build parallelism. All eligible defaults contribute full non-test audits even when unselected; new default checks own actually consumed source families, not invented graph dependencies. | **PASS** component positives/negatives and actual public-CLI production libgo prerequisite/default/audit/33-test route at `38c942c64`. Final **PASS** all three supported public default/NIP14 routes at `3ef5cbf9c`; still diagnostic/ineligible, not whole-profile acceptance. | Actual fresh configure/full preaudit/default and named prerequisite/postbuild/selected execution is now proved for the finite routes above. Whole-job/capability cells below still require their own proof. Never substitute `signer-tests`, selected-only audits or a fabricated File API `all` target. |
| Known production versus unknown default producer | B `0b9feea6d` binds actual known resource/schema/Blueprint/gdbus/copy/custom-utility producer shapes to source/tool/argv/inputs/outputs and configured trace. Unknown shell/launcher/dyndep/utility shapes still refuse. | **PASS** actual libgo default/postbuild/command at B's proof source; root core/Groundhog/Grotto producer bindings have zero gaps at `e47b52946`. Final supported production core/Groundhog/Grotto routes **PASS** at `3ef5cbf9c` with full audits. Unknown shapes are bounded unsupported cells, not implied parity. | Retain final full production proofs and unknown-producer refusals. New producer shapes require genuine source/tool/input/output evidence, not omissions or fabricated provenance. |
| `nak` executable | C produces executable-only fingerprints; B consumes path/hash/version and exact source/profile/plan/workflow/run/attempt for commands and CTest. Exact pinned-module version-link repair and source-bound local producer positives are retained above. | **PASS** component reader and real producer proof for both ordinary profiles. Final fresh executable **producer PASS** for both profiles. The supported default/NIP14 routes require no tool capability and do **not** prove consumer execution; B's source-specific consumer regressions remain separate. Full ordinary-profile lane **NOT RUN**. | Ordinary declarations are integrated; retain actual fingerprint consumption and separate bus/display/keyring custody. No full-profile or hosted acceptance from tool installation. |
| `gnome_keyring` executable | B `82f79ae75` provides actual executable fingerprint only, not bus/display/service setup. Existing CTests own private HOME/bus/supervised Secret Service, including service-initially-absent negatives. Only the ordinary Groundhog/Grotto profiles add `nak` and `gnome_keyring`, with their exact current profile hashes updated; historical review fields remain unchanged. | **PASS** component executable/fixture-ownership positives and negatives. Final fresh executable **producer PASS** for both profiles. Default/NIP14 requires no keyring capability and is not consumer/service proof; B's actual fixture/consumer evidence retains its own source. Full tool-consuming/profile execution is not inferred. | Retain executable proof separately from bus/display/service lifecycle. No ambient/global Secret Service authority or executable-as-isolation inference. |
| Contained keyring + private-bus command | B `82f79ae75` owns throwaway-service setup for commands explicitly requiring both capabilities, without taking over CTest fixture resources. | **PASS** 14 actual new contained cases: secret create/read/delete, cancellation, exit 19, missing/nonserving/preexisting service, environment redirection, HOME/control symlinks and namespace cleanup. **NOT RUN** final production-profile lane. | Preserve exact PreparedContext/control/source/profile/plan/CID/run binding and cleanup evidence; no native arbitrary-tree or whole-profile promotion. |
| `display` / `private_bus` | Separate authored requirements; neither follows from `nak` or keyring executable presence. | Each selected lane requires actual supported wrapper/setup proof. **FAIL / engineering** for unsafe mixed non-CTest/native wrapper custody. | Preserve prelaunch refusal while the unsupported custody seam remains. Do not drop requirements or silently unwrap. |
| NIP55L caller identity under Rosetta | Fixture authorizes the actual test executable; emulated daemon observes `exe:/run/rosetta/rosetta`. Approval wait is correct. | **FAIL** retained emulated execution; **EXTERNAL BLOCKER** for native Linux x86 proof. A's native ARM64 three-case **PASS** is diagnostic, not the authored x86 cell. | Run exact source on native Linux x86/hosted infrastructure. Never authorize the emulator shim, weaken attestation or relabel the timeouts as missing keyring capability. |
| NIP29 remote-feature connectivity fixture | A `dfb15c893` owns the in-process network monitor, proves offline queued/zero-sign/zero-publish then online acceptance, and retains the original deadline and signed-event assertions. | **PASS** five focused Linux registrations, including both independent NIP29 cases, at A's exact tip. Historical whole-346 failure is retained; final combined targeted verification **PASS:five/zero skips** at `3ef5cbf9c`. | Final exact registrations are verified. Keep the whole-346 failures and require native x86 caller-identity proof separately; no whole346 rerun or product deadline change. |
| `nip5f_tcp` provisioned signer | Test consumes real TCP endpoint/token, handshake and `get_public_key`; it is not `test_nip5f_loopback`. Ordinary profiles are not declared capable from tool/loopback evidence. | **EXTERNAL BLOCKER / fixture and approval** unless a genuine isolated setup is explicitly delivered. Exit 77 is not passing execution. | Requires approved `ENABLE_TCP_IPC=ON` configuration, isolated signer identity/runtime token readiness and explicit approval for the always-prompted unidentified caller. No approval bypass, fake signer or ambient identity. |
| Native/mixed-wrapper arbitrary descendants | Direct-child reap is truthfully distinct from arbitrary-tree cleanup. Explicit diagnostic ptrace/contained routes retain their own limits. | **EXTERNAL BLOCKER / platform restriction** for native arbitrary-tree custody without a real kernel primitive; mixed-wrapper implementation remains unsupported and refuses. | Retain negative cleanup evidence, escaped-child/sibling survival tests and prelaunch restrictions. No unsafe PID/name workaround or native parity from contained tests. |
| Original MDK image-build/BuildKit lifecycle | C `0c79eff77` implements additive `images [--probe]` scoped Docker-resource cleanup with unique explicit builder ownership; legacy run API and prepared-peer route remain distinct. | **PASS** component private-image positive/negative, partial/cancel/drain, owned CID and sibling-survival proofs. Exact original matrix **FAIL / incomplete** after bounded MDK011 dependency-fetch cancellation;08/09 **NOT RUN**. Scope cleanup passed, `complete=false`; `.8` open. | Final integration regressions/review only, **no second whole MDK matrix run**. Retain final-tip original-image NOT MET, old helper smokes and scoped errors; no default builder or global prune. |
| Hosted broad union, W11 and operator cutover | C authors the broad union and retains failed family accounting. | **EXTERNAL BLOCKER** for qualifying hosted/operator acceptance; local fixes are not hosted samples. | Collect reviewed distinct PR comparisons and successful complete broad integrations, then explicit operator approval. Current dated snapshot remains 0/20 and 0/2. |

NIP-5F source: `nips/nip5f/tests/test_nip5f_tcp.c:24-57` and
`apps/grotto/CMakeLists.txt:341,437-438`. Missing endpoint/token returns 77;
loopback success does not satisfy the provisioned-signer cell.

## Every authored profile: membership is not runnable acceptance

This census names all13 authored profiles at final tested source `3ef5cbf9c`.
Fresh full preaudits are distinguished from physical defaults, selected execution
and whole-job acceptance. Local amd64 containers on the ARM host are emulated;
they are not native Linux x86 acceptance.

| Profile | Membership / full input audit evidence | Default, runtime or capability cell | Finite remaining proof or blocker |
|---|---|---|---|
| `core.linux` | **PASS**, final full pre/postaudits zero gaps;6,834 input hashes. | Public engine-owned default/prerequisite + NIP14 **PASS**, partial aggregate passes, complete request refuses. | Broad display/private-bus/provisioned-signer setup remains unavailable; finite diagnostic is not whole-profile acceptance. |
| `core.linux.asan` | Fresh full preaudit **FAIL**: three legacy unclaimed registrations, `MockRelayUnit`, `concurrency_subscription_shutdown_asan`, `test_json_envelope`. | No generic omission authority; private bus is not signer provisioning. | Resolve those exact audit/execution contracts under separate approval; full instrumented/fuzz execution **NOT RUN**. |
| `core.macos.asan` | Final authored instrumented profile **NOT RUN**; safe native439 is non-instrumented and not its receipt. | Three legacy exclusions, signer approval and native arbitrary-tree custody remain separate. | Native instrumented setup/audit/runtime proof required; no substitution from standalone libgo or safe native diagnostics. |
| `grotto.linux` | **PASS**, final full pre/postaudits zero gaps;6,704 input hashes, exact33 omitted/29 required omitted producer targets retained. | Public default/all mapped + NIP14 **PASS**, incomplete/ineligible. Fresh executable setup producers pass separately; these selected checks require no tool capabilities and are not consumer/service proof. | Meson/deb/Shell/TSan variants, wrapped whole-profile execution and approved TCP signer remain unproved; no full-profile promotion. |
| `grotto.linux.asan_ubsan` | Final fresh full preaudit **PASS**,338 registrations/446 targets, zero gaps. | Instrumented wrapped/default-all execution **NOT RUN**; no ordinary33 authority. | Prove authored runtime and setup; exact13-test legacy selection and ancillary memberships do not establish whole-job extras or signer approval. |
| `groundhog.linux` | **PASS**, final full pre/postaudits zero gaps;5,818 input hashes; exact193-key/33/29 contracts retained. | Public default/all mapped + NIP14 **PASS**; five focused NIP29 registrations **PASS**, zero skips. | Whole346 history remains343 pass/three fail. Two NIP55L Rosetta identity failures require native Linux x86 proof; GUI/media/perf/whole-profile acceptance not inferred. |
| `groundhog.linux.sanitizers` | Final fresh configure/audit **REFUSES**: diagnostic nonmember fresh registration partition differs. | Original immutable authority remains bound to its exact reviewed universe; no automatic refresh. | Obtain separate exact source/partition approval before admission; full sanitized execution remains **NOT RUN**, not silently omitted or complete. |
| `groundhog.mdk08` | Final fresh full preaudit **PASS**,571 registrations/753 targets, zero gaps in the available diagnostic toolchain. | Not an original MDK image receipt. Scoped lifecycle regressions pass separately. | Original08 image build **NOT RUN** after C matrix stopped in011; `.8` remains open. No keyring/Nak/signer or prepared-route inference. |
| `groundhog.mdk011` | Final fresh full preaudit **PASS**,441 registrations/553 targets, zero gaps in the available diagnostic toolchain. | Original image acceptance **NOT MET**: C exact-tip dependency fetch cancelled at1000s; scoped cleanup passed. | Resolve actual fetch/build prerequisites and collect original-image proof separately under `.8`; no full matrix rerun here or prepared011 alias. |
| `groundhog.linux.arm64.prepared011` | Final x86 audit attempt **REFUSES** actual architecture mismatch. | Historical local ARM64 prepared peer/CID evidence retains its own source/context; not original-image or native custody proof. | Run on actual supported ARM64 with full audits; no architecture alias or selected-only fallback. |
| `libgo.linux.asan` | Final public fresh prerequisite/default/full postbuild **PASS**,467 input hashes. | All33 CTests **PASS**, complete-gate ineligible. | Remaining TSan/MPMC/fiber/fuzz variants and hosted parity need their own contracts/proof; no root or complete promotion. |
| `libgo.macos.asan` | Final public fresh prerequisite/default/full postbuild **PASS**,553 input hashes. |32 selected CTests **PASS**; stress remains not executed, complete-gate ineligible. | Historical intermittent stress exclusion is not causally fixed; no native-root or release acceptance. |
| `static.policy` | Final fresh full command-input preaudit **PASS**,zero gaps. |725 selector cases/six shells/four statics/inventory/actionlint/ShellCheck/whitespace **PASS**. | No skipped-tool success or invented public static-lane receipt; actual commands/logs are retained in final combined gates. |

## Every existing bead: finite criterion accounting

This table evaluates each bead against **its own stated criteria**, not against
whole-project acceptance. **Propose CLOSE** means the finite criterion is now
proved and parent may approve closure; it is not a tracker mutation or silent
scope deferral. `.7` is already officially closed. Historical closed scope stays
closed; current regression success does not manufacture reruns of historical
platform/compiler/corpus workloads. Root `nostrc-0cfsg` explicitly retains the
remaining runtime/whole-job/W11/W12 work in its official comment. No criterion
is erased and no partial bead is proposed closed.

| Bead | Existing criterion group | Current disposition / remaining proof |
|---|---|---|
| `nostrc-0cfsg` | Execute W1-W12 with affected/premerge, broad master/release/manual, fail-broad unknowns, exact-SHA safety/advisory policy. | **OPEN / acceptance incomplete**. Finite implementation criteria below are individually proved; original-image `.8`, heartbeat `.11`, native custody `.23`, dependency leak `.29`, explicit runtime/whole-job cells and W11/W12 remain tracked. W11 **NOT MET**, W12 **NOT RUN**. |
| `.1` | Schema/range/golden/aggregation: rename/delete/gitlink/shallow/newline, selected skip/cancel/stale identity, release applicability, explicit unsupported roots; real module delegation. | **PASS / CLOSED** finite logical scope; final725-case regressions pass. No hosted acceptance inferred. |
| `.2` | Fresh/stale/failed configure; standalone/root export; fixtures/reset/model-GUI split; skip/dirty-cache/missing-edge negatives; actual focused build/test; no silently unowned supported registrations. | **PASS — propose CLOSE**. Final real root and standalone configure/build/full input/header/registration audit/selected execution now pass;725 regressions cover fresh/stale/failed generation, fixture/reset/skip/dirty/missing-edge refusal. No silently unowned supported registration; unsupported cells are explicit below/root. Version decisions recorded, no target refactor or invented platform. |
| `.3` | Shell regressions; actual shared plan/resolve/run; unknown broadening; selected capability refusal; cross-checkout locks/cancellation; sanitizer configuration parity. | **PASS — propose CLOSE** finite gate contract. All six shell suites, real shared plan/resolve/run, split/shared parallelism, locks/cancel/capability-refusal and sanitizer phase/configuration regressions pass. Native wrapped custody remains a visible prelaunch refusal owned by `.23`, not a waived positive criterion here. |
| `.4` | Workflow lint/static and PR/merge-group/tag/dry-run/result routing; no publication from partial validation; executable broad replacement; duplicate triggers explicit. | **PASS — propose CLOSE** finite workflow contract. Full lint and event/result/merge-group/tag/dry-run/partial-publisher negatives pass; all17 broad families are authored/executable, temporary duplicate triggers explicit. Successful hosted broad/release publication is not claimed and remains root W11/W12 work. |
| `.5` | Source-grounded baseline/commands; accurate plan/AGENTS/TESTING; combined gates/review; explicit external rollout prerequisites. | **PASS — propose CLOSE after this documentation/export commit**. Exact commands/source/receipts and all15 gates, native/production results, review disposition, version policy and this35-bead register are recorded. Updated19:03 UTC hosted evidence remains0/20 and0/2, explicitly open under root; no rollout acceptance requirement is silently dropped. |
| `.6` | Full actionlint with ShellCheck, unchanged workloads/artifacts/profiles/advisory behavior. | **PASS / CLOSED**; final actionlint including ShellCheck and all statics pass; workloads/artifact ownership/policy unchanged. |
| `.7` | Truthful libgo32 diagnostic; no stress/complete promotion; real command/build-contract absent/wrong-source/input/capability/cancel/nonzero/evidence negatives; full regressions. | **PASS / officially CLOSED**. Final actual macOS32/Linux33 plus default/full postbuild routes and725 regressions pass, stress not executed/ineligible; three root routes now also prove public normalization. No complete promotion. |
| `.8` | Full sanitizer phase/config/environment parity; concurrent roots; cancelled/partial MDK images and sibling cleanup; real safe evidence distinguished from simulation. | **OPEN / FAIL-incomplete**. Final sanitizer diagnostic routes and real scoped lifecycle positives/negatives pass. Original exact-tip matrix stopped in011 dependency fetch;08/09 NOT RUN, full original-image acceptance NOT MET. Scoped cleanup2.8s passed, not native arbitrary-tree proof. Next: resolve actual image fetch/build prerequisites and obtain source-bound original-route proof, separately; no second matrix here. |
| `.9` | Every job/matrix/check mapped or explicitly tracked unsupported; exact native/Linux registrations/semantic edges; isolation goldens; no unexplained omissions or fake enforcement readiness. | **PASS — propose CLOSE** inventory criteria, not runtime acceptance. Every job/matrix/check is mapped or a tracked unsupported gap; native/available Linux exports, semantic edges, final three full pre/postaudits and isolation/savings goldens pass. Root explicitly owns remaining execution cells, all coverage gaps stay visible/enforcement-not-ready; hosted quotas remain separate. |
| `.10` | Actual CMake 3.22/3.25 nested/root/standalone metadata and fixture/DEPENDS parity, current suite, no fallback. | **PASS / CLOSED**; final metadata regressions pass. Historical actual3.22/3.25 runs retain their own sources; no invented new old-tool rerun. |
| `.11` | Source-grounded historical heartbeat cause; deterministic verification where established; unresolved scheduling evidence explicit; original guard retained. | **FAIL / unresolved cause**, not a proved external-toolchain excuse. Later passing samples do not establish causality; no threshold/exclusion/retry waiver. |
| `.12` | Truthful MDK descendant cleanup after setsid, or stop claiming complete cleanup. | Historical finite refusal correction **PASS / CLOSED**. Legacy `cleanup_complete=false` is preserved, not arbitrary-tree success; wider `.8` remains open. |
| `.13` | Nested negative diagnostic eligibility cannot be hidden by omitted outer field. | **PASS / CLOSED**; final nested eligibility/aggregation regressions pass. |
| `.14` | Non-UTF8/raw output survives; decoding failure finalizes negative receipt/log. | **PASS / CLOSED**; final raw-byte/failure finalization regressions pass, with prior failed traces retained. |
| `.15` | Actual split configure/build/test environments; drift/unknown/malformed/template/cache negatives; test-only vars absent earlier. | **PASS / CLOSED**; final fresh supported root/standalone routes and phase/environment drift negatives pass. |
| `.16` | Actual exact-tree/dirty command outer route; source/input/capability/cancel/evidence negatives; shared sanitizer route; six shell/statics/lint; no bypass. | **PASS / CLOSED** bounded route; final shared/unwrapped routes and all combined gates pass. Unsafe wrappers remain refused under `.23`. |
| `.17` | Production selection command has truthful full inputs/source; missing/moved/new helper/drift/wrong-source negatives; no skip masking; normal versus strict inventory honesty. | **PASS / CLOSED**. Final full725-case selector workload and complete input preaudit pass (actual argv retained, not relabelled as a public static-lane receipt); missing/moved/new-helper/identity negatives remain. Actual input enrollment includes A/B/C, keyring and custody helpers; no skip or false strict-enforcement claim. |
| `.18` | Absolute fixture paths cannot collide with a real `/source`; omitted stress and unknown registrations stay audited. | **PASS / CLOSED**; final absolute-path collision/unknown registration/omitted-source fixture regressions pass. |
| `.19` | Diagnose actual SIGTERM lost output without invented readiness cause or weakened raw-byte assertions. | **PASS / CLOSED**, distinct original failure retained; final lifecycle/byte regressions pass without inventing readiness causality. |
| `.20` | Real deterministic ready/emit handshake before cancellation/timeout; retain raw bytes and final negative receipt; no sleep/retry substitute. | **PASS / CLOSED**; final deterministic handshake/cancel/raw-byte regressions pass; no sleep/retry substitute. |
| `.21` | Every authored profile maps to independent worker/needs/conclusion; honored native/sanitizer setup or named refusal; supported lanes still dispatch; no cutover. | **PASS / CLOSED** finite worker census; final workflow/inventory/refusal tests pass. Independent conclusions/setup gaps remain explicit; no hosted execution or cutover inferred. |
| `.22` | Before-launch refusal for selected non-CTest display/private-bus and mixed lanes; preserve legacy CTest; no dropping/receipts/unwrap; supported unwrapped/shared cases and shell/statics. | **PASS / CLOSED** bounded prelaunch refusal; final mixed/display/private-bus negatives and supported cases pass. This does not close safe custody `.23`. |
| `.23` | Real macOS/Linux command+CTest custody, escaped TERM-ignoring children, unrelated sibling survival, raw evidence; no numeric/name/sleep substitute. | B truthfulness/component negatives **PASS**. Native arbitrary-tree custody without a real kernel primitive is **EXTERNAL BLOCKER / platform restriction**; mixed wrappers remain unsupported and refuse. Do not close from contained/ptrace diagnostics or substitute unsafe numeric PID cleanup. |
| `.24` | Identify or faithfully retain failing full-gate command/trace on recurrence; deterministic fix only if warranted; distinguish later diagnostics. | Historical finite retention **PASS / CLOSED**; no invented original cause or new full-gate success. |
| `.25` | Correct visible `snprintf` declaration; build all three fuzz targets and retain actual corpus guards. | **PASS / CLOSED** historical finite compile repair; final source builds/regressions pass, not a new hosted fuzz/corpus run. JSON corpus failure remains `.29`. |
| `.26` | Actual regular BP metrics/emission/parser workload with original parameters, not shortened or missing metrics. | **PASS / CLOSED** finite workload contract; final regressions pass. Actual final hosted broad BP workload is NOT RUN/success not established. |
| `.27` | Homed mandatory unit registrations and original filter, explicit test opt-in, no empty-match success. | **PASS / CLOSED** opt-in/exact-list/no-empty-match repair; final workflow guards preserve full homed call. No fresh hosted homed execution claim. |
| `.28` | Compatible default UI widget/build with behavior preserved, approved shipped fix and version assessment, no target suppression. | **PASS / CLOSED** finite shipped repair; final safe native configure/default build and current regressions pass. No new product change here. |
| `.29` | Verified JSON-GLib remedy for real malformed-input leak; original corpus, leak detection and duration intact. | **OPEN / FAIL**, verified real JSON-GLib leak retained; dependency remedy is outside this integration's authorized scope, not a successful skip. Next: compatible upstream/source remedy and exact original corpus/duration/leak-detection proof. No suppression or corpus reduction. |
| `.30` | Positive and pre/postaudit/build/capability/source/cancel CLI receipts preserve valid lane identity/raw bytes; invalid plans invent none; aggregation retains failure and rejects promotion. | **PASS / CLOSED**; final CLI negative receipts/identity/raw-byte/partial aggregation/promotion regressions pass. |
| `.31` | Real setup/build probes and worker routing; fresh Grotto registrations/target/input mapping and supported actual cases; isolation/MLS selection; every unsupported step explicit. | **PASS — propose CLOSE** finite setup/routing/membership criteria. Actual final Grotto fresh export/full input/default/NIP14 route, real Nak/keyring producers, existing fixture isolation and MLS/non-MLS savings tests pass. Unsupported variants/whole jobs remain explicit root gaps; native x86 NIP55L and provisioned signer approval are not silently counted as passing cases. |
| `.32` | Reproduce tracked generated-source mutation; actual build-tree outputs/install paths; source stays byte-clean; no fake platform claim. | **PASS / CLOSED**; final native and production defaults leave source clean. Root-owned templates/witnesses preserve artifacts/install paths; pinned dirty vendor/submodule untouched. |
| `.33` | Physical CLI build2/test1 and target-only contract; fallback/legacy; type/override/resolved/receipt/argv contradictions; source/capability/failure/cancel negatives. | **PASS / CLOSED**; final Grotto build2/test1 physical evidence, fallback/split/override/forged-argv negatives and shared routes pass. |
| `.34` | Authorized bounded default-generator contract; exact fresh provenance; unknown custom/default refusal; full non-test audits and negative lifecycle proof before admission. | **PASS — propose CLOSE** separately authorized finite default-production wave. Parent explicitly approved B/root implementation after the earlier criterion's no-implementation wave; this is not retroactive authorization. Final actual three root and two standalone default/full audits/postbuild/selected executions pass, along with known-producer and unknown/refusal/lifecycle negatives. Exact provenance/source/template repairs are proved; no invented `all` target, source-refusal laundering or selected-only audit. |

## Finite remaining execution queue

1. **Root / native Linux x86:** run the two exact NIP55L RPC cases on a native
   x86 kernel/executable identity. Retain Rosetta failures and ARM64 diagnostics;
   never authorize the shim or weaken caller approval.
2. **Root / provisioned signer:** approved `ENABLE_TCP_IPC=ON`, private identity,
   runtime token and explicit unidentified-caller approval before the real TCP
   test. Exit77/loopback cannot satisfy it.
3. **`.8` original MDK:** resolve actual011 dependency fetch/build prerequisites,
   then obtain exact-source original08/09/011 acceptance and scoped cleanup.
   Current cancellation/08-09 NOT RUN remain; no second whole matrix in this session.
4. **`.23` native/mixed custody:** identify a genuine kernel-supported ownership
   primitive before escaped-child/sibling-survival positive proof. Current
   platform restriction and prelaunch refusals remain; no unsafe PID workaround.
5. **`.11` / `.29`:** reproduce the original heartbeat failure with a causal
   trace; separately deliver a compatible JSON-GLib remedy and unchanged corpus
   proof. Later passes, timeouts or dependency suppressions are not remedies.
6. **Root / explicit whole-job cells:** core ASan's three legacy registrations;
   fresh separately approved Groundhog sanitizer partition; actual native macOS
   root ASan/GUI/media/perf; Grotto Meson/deb/Shell/TSan; libgo TSan/MPMC/fiber/fuzz;
   remaining17-family broad setup/install/Windows/homed contracts. Each remains a
   named inventory gap/worker cell, not an unexplained omitted test or proof from
   the supported NIP14 routes. Prepared ARM64 requires its actual architecture.
7. **Root W11/W12 / operator:** parent lands the exact local changes, collect20
   qualifying reviewed comparisons and two complete broad integrations, then
   obtain explicit operator approval before any default/protection/schedule or
   publication-mode cutover. No local proof advances those counters.

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

The latest reviewed hosted snapshot is **2026-10-22T19:03:24.712764Z**, not a live
counter: **0/20** distinct qualifying reviewed PR comparisons, **0/2** successful
complete broad integrations; **45 failed / 1 cancelled / zero pending of46 roots**.
See the [dated ledger](dependency-aware-testing-ci-2026-10-18-run-ledger.md#integration-refresh--2026-10-22t190324712764z).
All earlier43/1/2 and44/1/1 observations remain historical, not erased.
No local component tests, nested jobs, retries, parent colors or omitted/partial
receipts advance those counters. **W11 NOT MET / W12 NOT RUN.**

## Tracker and ownership

Final official `bd export --all`, captured **2026-10-22T20:24:53.734648Z**:
**1,552 records (1,515 issues +37 full memories),575 comments,1,149 generated
interactions**, including18 root comments and five root interactions. All prior
root HEAD/index/worktree, integration HEAD/worktree and initial preservation
snapshots were compared semantically: zero lost identities/comments, zero changed
memory records, zero missing/changed interactions, and every old event stream is
a byte-exact prefix. The whole legitimate root event file was copied byte-for-byte,
not merged/edited semantically. It includes the earlier3lofk events, `.7` closure
`int-450b1585` and final root tool checkpoint `int-8401dac9`.

Official issue export SHA-256
`f1045f442386277f8aa4c95d9f45cc391511853bd7dd0cacb259963077275fe2`;
generated interactions
`ae3b0e2b35cc9a0eafb6bf123c861b5fb194a4723a58e7c7247755776f137ded`.
`/tmp/nostrc-closeout-integration-20261022/final-tracker/preservation.json`
SHA-256 `7f805df0f7fed0200fb0566e15333645eed0f3215ad47be189714b6e7b30d850`;
adjacent `SHA256SUMS`
`28c2ebbe31a4b0fda4058e8db440c21d1a3f26506443e74aba126455e003be55`.
The earlier14:43 checkpoint (547 comments/1,147 interactions) and15:52 root-before-FF
snapshot remain retained, not restored over the new generated events.

Seven exact closure proposals are official bd comments, not status mutations;
`.7` remains officially closed. Root/`.8`/`.11`/`.23`/`.29` stay open. No direct
semantic database/JSONL edit or broad closure. `bd dolt pull` still refuses
**Error1105: no remote**, not successful sync. Commits remain **local only**;
parent owns landing/push/cleanup. User image/investigation/submodule fingerprints
are separate and untouched; no `.DS_Store` or vendor cleanup.
