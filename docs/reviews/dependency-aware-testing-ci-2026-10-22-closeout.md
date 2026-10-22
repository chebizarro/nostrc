# Dependency-aware CI: finite closeout criterion register

## Source and disposition rules

**Integration is in progress, not accepted.** Current code checkpoint:
`48965e6b5cf46b32c6c75602d1264dfb4acc7526`, based on
`dd949e4b7a2969e01ac5dea3f12bdde2cf22655b`. Preserving merges contain exactly:

- C `827e1c70ff17ff61747c7d42ff9633820fb0330d`, implementation
  `385a6cf2555756c3ccd46e13ff82bf8eb9a2c33d`, through `513999d373`.
- B `405c23b3f4bd6ab9d65818ee195e84ad18d5ad33`, preserving
  `1abee21ecbdbeabdf034444d77831e1d05289e35`, through `48965e6b5`.
- Integration test correction `dd08636cc6e6f77316c4e6f3b2ec52237f9d3f58`.

**A is not merged. B's keyring follow-through is not merged.** Neither moving
branch is an accepted source. Final gates await parent-approved exact tips.

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

All internal CI/test/docs changes here assess **no component version bump**.
No policy/default, publisher/platform, schedule, protection, authority or release
cutover is approved. Exact-33 omissions/29 required omitted-target closures stay
incomplete, and no selected-only audit fallback is authorized. Original immutable
fixtures, review identities and failed attempts remain retained.

## Delivered components, not final integration gates

| Source-bound criterion | Disposition and evidence |
|---|---|
| C workflow/event/release/setup contracts | **PASS**, 75 focused tests at merge `513999d373`, zero skips. C's failed 613-case full run is historical, not a successful combined suite. |
| PR merge and annotated-tag workflow identity | **PASS**, 42 workflow tests at `dd08636cc`, including explicit API-head/tag-object workflow-SHA rejection before outputs. Guard unchanged. |
| B supported default-generator, non-test audit, capability and lifecycle components | **PASS**, B final tip: 622 selector, 65 actual Linux-arm64 component and 13 contained cases, zero skips; 13 shell/static commands. These are component results, not current integration execution. |
| B native default build/full CTest | **PASS at `1abee21ec` only**: 434 pass, five visible skips. **NOT RUN** at final B tip or combined final source. |
| Current combined full selector/six shell/statics/lint | **NOT RUN**; await frozen A and B follow-through. |
| Current combined safe native fresh default build/full CTest | **NOT RUN**; earlier native receipts are not relabeled. |
| Current combined fresh authored Linux full audits and actual supported core/default execution | **NOT RUN**; profile authoring and backend execution must both be integrated. A's unmerged zero-preaudit reports are not credited here. |
| One bounded review of novel integrated seams | **NOT RUN**; parent C review found no immediate blocker, but is not the final integrated review. |

B component proof SHA-256:
`da9fe52856ca88b41a7ed9fe6ed23a5ae5d918c53c04b8269de07d8dd237908e`.
All **66** adjacent `SHA256SUMS` artifacts and **12** committed source-file
hashes were independently verified. C's **37** workflow records were independently
matched to actual job/workflow bytes. All **27** existing fixture/golden hashes
are pinned at the integration base, not refreshed to make assertions pass.
Receipts and comparison manifests:
`/tmp/nostrc-closeout-integration-20261022/`.

## Runnable-capability and build cells

| Cell | Implemented membership/setup | Physical runnable proof / disposition | Actual remaining action |
|---|---|---|---|
| Single-config native Ninja default scope | B implements `kind=build_contract`, `build_scope=default`, `build_targets=[]`, exact `cmake --build {build}` plus realized configuration and authored build parallelism. All eligible default checks contribute full non-test audits, even when unselected. | **PASS** component compile/link/object/phony/symlink/prerequisite positives and adversarial negatives. **NOT RUN** integrated authored production core route. | Integrate exact A profile interface; run actual configure/resolve/full preaudit/default and named prerequisite builds/postbuild audit/selected execution, retaining source and physical argv. Never substitute `signer-tests` or fabricate a File API `all` target. |
| Default custom/utility/dyndep/validation/launcher/unknown producer | Unsupported shapes refuse visibly; known native producer traversal is bound to generation/tool/manifests. | **FAIL / engineering** for unsupported authored cells; refusal is not completed parity. | Retain the exact offending producer and required source closure; no omission, selected-only fallback or fabricated generated provenance. |
| `nak` executable | C produces executable-only fingerprint; B consumes exact path/hash/version and source/profile/plan/workflow/run/attempt for command and CTest requirements. | **PASS** component reader/executable evidence. **NOT RUN** full integrated ordinary-profile lane. | Add ordinary profile declaration only after positive supported setup consumption and exact authored interface reconciliation. Tool proof does not grant bus/display/keyring custody. |
| `gnome_keyring` executable | At B `405c23b3f`, requirement refuses before launch. Active follow-through changes the contract to actual executable fingerprint only, not bus/display/service setup. Existing CTests already own private HOME/bus/supervised Secret Service, including service-initially-absent negatives. | **NOT RUN** for exact final B integration; current `405c23b3f` support is **FAIL / engineering**, not external. | Await exact B executable positives/negatives, then reconcile profile tool declarations. Do not require a new ambient/global Secret Service authority or reinterpret executable presence as resource isolation. |
| Contained keyring + private-bus command | Active B follow-through owns throwaway-service setup for commands that explicitly require both capabilities, without taking over CTest fixture resources. | **NOT RUN** until exact B follow-through and real contained positives/negatives. | Require actual secret roundtrip and owned namespace/service cleanup; keep display/bus/tool evidence separate. Do not substitute mocked service presence or global attestation for the real command fixture. |
| `display` / `private_bus` | Separate authored requirements; neither follows from `nak` or keyring executable presence. | Each selected lane requires actual supported wrapper/setup proof. **FAIL / engineering** for unsafe mixed non-CTest/native wrapper custody. | Preserve prelaunch refusal while the unsupported custody seam remains. Do not drop requirements or silently unwrap. |
| `nip5f_tcp` provisioned signer | Test consumes real TCP endpoint/token, handshake and `get_public_key`; it is not `test_nip5f_loopback`. Ordinary profiles are not declared capable from tool/loopback evidence. | **EXTERNAL BLOCKER / fixture and approval** unless a genuine isolated setup is explicitly delivered. Exit 77 is not passing execution. | Requires approved `ENABLE_TCP_IPC=ON` configuration, isolated signer identity/runtime token readiness and explicit approval for the always-prompted unidentified caller. No approval bypass, fake signer or ambient identity. |
| Native/mixed-wrapper arbitrary descendants | Direct-child reap is truthfully distinct from arbitrary-tree cleanup. Explicit diagnostic ptrace/contained routes retain their own limits. | **FAIL / engineering** for unsupported native/mixed wrapper custody. | Retain negative cleanup evidence, escaped-child/sibling survival tests and prelaunch restrictions; no native parity claim from contained tests. |
| Original MDK image-build/BuildKit lifecycle | Legacy guard remains `cleanup_complete=false`; contained prepared-peer proof is a different route. | **FAIL / engineering** for original arbitrary-tree image-build cleanup. | Preserve `cleanup_unverified` and scoped errors. Do not promote raw peer/contained success or direct-child reap to original lifecycle acceptance. |
| Hosted broad union, W11 and operator cutover | C authors the broad union and retains failed family accounting. | **EXTERNAL BLOCKER** for qualifying hosted/operator acceptance; local fixes are not hosted samples. | Collect reviewed distinct PR comparisons and successful complete broad integrations, then explicit operator approval. Current dated snapshot remains 0/20 and 0/2. |

NIP-5F source: `nips/nip5f/tests/test_nip5f_tcp.c:24-57` and
`apps/grotto/CMakeLists.txt:341,437-438`. Missing endpoint/token returns 77;
loopback success does not satisfy the provisioned-signer cell.

## Every existing bead: finite criterion accounting

The table separates recorded historical closure from unfinished final-source
execution. **NOT RUN** means no final combined-source result yet, not that the
historical implementation has disappeared. Each grouped criterion must be
satisfied in full before its bead closes; passing a subset does not close it.

| Bead | Existing criterion group | Current disposition / remaining proof |
|---|---|---|
| `nostrc-0cfsg` | Execute W1-W12 with affected/premerge, broad master/release/manual, fail-broad unknowns, exact-SHA safety/advisory policy. | **FAIL / incomplete implementation and acceptance**. Children below remain open; W11 **NOT MET**, W12 **NOT RUN**. |
| `.1` | Schema/range/golden/aggregation: rename/delete/gitlink/shallow/newline, selected skip/cancel/stale identity, release applicability, explicit unsupported roots; real module delegation. | Historical **PASS / CLOSED**. Final combined selector **NOT RUN**. |
| `.2` | Fresh/stale/failed configure; standalone/root export; fixtures/reset/model-GUI split; skip/dirty-cache/missing-edge negatives; actual focused build/test; no silently unowned supported registrations. | Component **PASS**; integrated authored full-universe ownership/audit/build/execution **NOT RUN** pending A and final engine. Native CTest alone cannot satisfy it. |
| `.3` | Shell regressions; actual shared plan/resolve/run; unknown broadening; selected capability refusal; cross-checkout locks/cancellation; sanitizer configuration parity. | Component **PASS**; final shared-profile execution/parity **NOT RUN**; unsupported custody remains **FAIL / engineering**. |
| `.4` | Workflow lint/static and PR/merge-group/tag/dry-run/result routing; no publication from partial validation; executable broad replacement; duplicate triggers explicit. | C finite contracts **PASS**. Full-source/profile broad execution and exact-tag publisher acceptance **NOT RUN**; existing failed broad attempts remain **FAIL**, not erased. |
| `.5` | Source-grounded baseline/commands; accurate plan/AGENTS/TESTING; combined gates/review; explicit external rollout prerequisites. | Register/docs integration in progress. Final gates/review/complete criterion reconciliation **NOT RUN**; dated hosted counters are separate. |
| `.6` | Full actionlint with ShellCheck, unchanged workloads/artifacts/profiles/advisory behavior. | Historical **PASS / CLOSED**; B component lint **PASS**. Final lint **NOT RUN**. |
| `.7` | Truthful libgo32 diagnostic; no stress/complete promotion; real command/build-contract absent/wrong-source/input/capability/cancel/nonzero/evidence negatives; full regressions. | Component **PASS**; actual integrated default/core route and final regressions **NOT RUN**. Complete promotion remains forbidden. |
| `.8` | Full sanitizer phase/config/environment parity; concurrent roots; cancelled/partial MDK images and sibling cleanup; real safe evidence distinguished from simulation. | Component regressions **PASS**; original MDK lifecycle and unsafe wrappers **FAIL / engineering**. Final supported sanitizer route **NOT RUN**. |
| `.9` | Every job/matrix/check mapped or explicitly tracked unsupported; exact native/Linux registrations/semantic edges; isolation goldens; no unexplained omissions or fake enforcement readiness. | A implementation not merged: integrated census/full fresh audits/goldens **NOT RUN**. Runnable capability cells remain separate even if membership passes. |
| `.10` | Actual CMake 3.22/3.25 nested/root/standalone metadata and fixture/DEPENDS parity, current suite, no fallback. | Historical **PASS / CLOSED**. Final current regressions **NOT RUN**; no new claim of old-tool reruns. |
| `.11` | Source-grounded historical heartbeat cause; deterministic verification where established; unresolved scheduling evidence explicit; original guard retained. | **FAIL / unresolved cause**, not a proved external-toolchain excuse. Later passing samples do not establish causality; no threshold/exclusion/retry waiver. |
| `.12` | Truthful MDK descendant cleanup after setsid, or stop claiming complete cleanup. | Historical finite refusal correction **PASS / CLOSED**. Legacy `cleanup_complete=false` is preserved, not arbitrary-tree success; wider `.8` remains open. |
| `.13` | Nested negative diagnostic eligibility cannot be hidden by omitted outer field. | Historical **PASS / CLOSED**; final aggregation regressions **NOT RUN**. |
| `.14` | Non-UTF8/raw output survives; decoding failure finalizes negative receipt/log. | Historical **PASS / CLOSED**; B byte/lifecycle component tests **PASS**; final suite **NOT RUN**. |
| `.15` | Actual split configure/build/test environments; drift/unknown/malformed/template/cache negatives; test-only vars absent earlier. | Historical **PASS / CLOSED**; final actual supported-profile phase proof **NOT RUN**. |
| `.16` | Actual exact-tree/dirty command outer route; source/input/capability/cancel/evidence negatives; shared sanitizer route; six shell/statics/lint; no bypass. | Historical bounded **PASS / CLOSED**; final combined route/gates **NOT RUN**. Unsafe wrappers remain refused under `.23`. |
| `.17` | Production selection command has truthful full inputs/source; missing/moved/new helper/drift/wrong-source negatives; no skip masking; normal versus strict inventory honesty. | Historical **PASS / CLOSED**. New source input/fingerprint reconciliation awaits A; final production command **NOT RUN**. |
| `.18` | Absolute fixture paths cannot collide with a real `/source`; omitted stress and unknown registrations stay audited. | Historical **PASS / CLOSED**; final fixture regressions **NOT RUN**. |
| `.19` | Diagnose actual SIGTERM lost output without invented readiness cause or weakened raw-byte assertions. | Historical **PASS / CLOSED**, with distinct failure history retained; final lifecycle regressions **NOT RUN**. |
| `.20` | Real deterministic ready/emit handshake before cancellation/timeout; retain raw bytes and final negative receipt; no sleep/retry substitute. | Historical **PASS / CLOSED**; B Linux component byte/cancel tests **PASS**; final combined regressions **NOT RUN**. |
| `.21` | Every authored profile maps to independent worker/needs/conclusion; honored native/sanitizer setup or named refusal; supported lanes still dispatch; no cutover. | Historical finite **PASS / CLOSED**. C event contracts **PASS**; final inventory/worker census **NOT RUN**; hosted execution not inferred. |
| `.22` | Before-launch refusal for selected non-CTest display/private-bus and mixed lanes; preserve legacy CTest; no dropping/receipts/unwrap; supported unwrapped/shared cases and shell/statics. | Historical bounded refusal **PASS / CLOSED**; final refusal regression **NOT RUN**. This never closed safe custody `.23`. |
| `.23` | Real macOS/Linux command+CTest custody, escaped TERM-ignoring children, unrelated sibling survival, raw evidence; no numeric/name/sleep substitute. | B truthfulness/component negatives **PASS**. Unsupported native/mixed-wrapper custody **FAIL / engineering**; do not close from contained/ptrace diagnostics. |
| `.24` | Identify or faithfully retain failing full-gate command/trace on recurrence; deterministic fix only if warranted; distinguish later diagnostics. | Historical finite retention **PASS / CLOSED**; no invented original cause or new full-gate success. |
| `.25` | Correct visible `snprintf` declaration; build all three fuzz targets and retain actual corpus guards. | Historical finite compile repair **PASS / CLOSED**. JSON corpus failure stays under `.29`; final hosted fuzz **NOT RUN**. |
| `.26` | Actual regular BP metrics/emission/parser workload with original parameters, not shortened or missing metrics. | Historical finite repair **PASS / CLOSED**; final hosted broad workload **NOT RUN**. |
| `.27` | Homed mandatory unit registrations and original filter, explicit test opt-in, no empty-match success. | Historical finite repair **PASS / CLOSED**; C preserves full homed broad call. Final hosted homed workload **NOT RUN**. |
| `.28` | Compatible default UI widget/build with behavior preserved, approved shipped fix and version assessment, no target suppression. | Historical finite shipped repair **PASS / CLOSED**; final safe native build **NOT RUN**. No new product change authorized here. |
| `.29` | Verified JSON-GLib remedy for real malformed-input leak; original corpus, leak detection and duration intact. | **EXTERNAL BLOCKER / dependency remedy** and retained real corpus **FAIL**. No dependency patch/suppression/corpus reduction in this integration. |
| `.30` | Positive and pre/postaudit/build/capability/source/cancel CLI receipts preserve valid lane identity/raw bytes; invalid plans invent none; aggregation retains failure and rejects promotion. | Historical **PASS / CLOSED**; B failure components **PASS**; final combined negatives **NOT RUN**. |
| `.31` | Real setup/build probes and worker routing; fresh Grotto registrations/target/input mapping and supported actual cases; isolation/MLS selection; every unsupported step explicit. | A membership/interface and B setup follow-through not merged: integrated proof **NOT RUN**. Tool capability is distinct from display/bus/fixture and execution acceptance. Existing keyring CTests retain resource ownership. |
| `.32` | Reproduce tracked generated-source mutation; actual build-tree outputs/install paths; source stays byte-clean; no fake platform claim. | Historical **PASS / CLOSED**; final native/default build source cleanliness **NOT RUN**. |
| `.33` | Physical CLI build2/test1 and target-only contract; fallback/legacy; type/override/resolved/receipt/argv contradictions; source/capability/failure/cancel negatives. | Historical **PASS / CLOSED**; B physical argv negatives **PASS**; final combined phase proof **NOT RUN**. |
| `.34` | Authorized bounded default-generator contract; exact fresh provenance; unknown custom/default refusal; full non-test audits and negative lifecycle proof before admission. | B supported component scope **PASS**. Actual integrated production route **NOT RUN**; unsupported producers **FAIL / engineering**. No `all` target or selected-only approximation. |

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

Official all-record export at **2026-10-22T12:00:21.434730Z** retained **1,552
records (1,515 issues + 37 memories), 505 comments and 1,145 interactions**,
including 16 root comments and two root interactions. Every base/initial record
identity, comment, memory value and prior interaction was independently verified
preserved. This snapshot predates later sibling/follow-through comments and must
be refreshed at final closeout; it is not presented as a live total.
`bd dolt pull` still refuses **Error 1105: no remote**, not successful sync.
No direct semantic JSONL edits or broad bead closure. Commits remain local;
parent owns landing and cleanup.
