# Profile/MDK parity and measurement: PR24 source receipt

## Landing and scope

[PR24](https://github.com/chebizarro/nostrc/pull/24) merged normally, without admin bypass, at **2026-10-24T00:58:13Z** as `b43ff6160b17a8ef120e171b6be211bda535d795`.

| Identity | Exact value |
|---|---|
| Reviewed/pushed final head | `bd0b3b2fd36903e3c220249faef993ffc3e4bb36` |
| GitHub-tested synthetic merge | `f5d85dec88985c5a40d4fc0ff39a9e8224a3c40f` |
| Tested/landed tree | `1b6b8a9c5070f45e51cff22d7cd201bace8f27eb` |
| Both merge parent lists | `eaa918a0ab5ffb757171922a690c629d919d4025`, `bd0b3b2fd36903e3c220249faef993ffc3e4bb36` |

Equal trees do not alias the head, tested merge, landing commit or earlier native source. This is a finite **source repair and tooling batch**, not whole-profile, W11, release or W12 acceptance. No shipped component version bump: only internal CI, test and documentation changes.

## Delivered changes

1. **Default-build prerequisites.** `dependency-worker.yml` installs `libmicrohttpd-dev` because enabled Signet's `bootstrap_server.c` includes its header unconditionally. The existing pinned Blueprint0.22.2 setup now includes `groundhog.linux`, whose enabled Grotto dependency requires that exact pin. No disabled targets or lowered pins. Source-derived positive, historical and drift/masking/duplication negatives are in `test_worker_prerequisites.py`.
2. **Real registration ownership.** Nine existing `supported.apps.*` checks gain exact MDK011 projections for **46 observed registrations / 316 observed missing edges**, and core Linux ASan projections for **45 registrations / 291 edges**. Hosted sibling descriptors match after path normalization; exact fixture closure and original owners remain audited. The three legacy ASan exclusions remain unclaimed and blocking. No new nonmember/omission authority.
3. **Grotto private test isolation.** Room-flow and approval-integration clear inherited `NOSTR_SIGNER_ENDPOINT`, `NOSTR_SIGNER_SOCK` and `NOSTR_SIGNER_TOKEN` before starting their D-Bus-only private daemons. Actual local macOS headless reproduction: overlay before repair fails on the occupied fixture listener, no-overlay control passes, repaired overlay runs both tests successfully. The owned listener retains zero connections and remains bound; test daemons are cleaned up. This is not Linux GUI proof or a general engine credential-scoping fix.
4. **Finite measurement capture.** `scripts/w11-evidence.py matrix` validates an explicit serial same-source schedule and holds repository locks across the cohort. Seven workload classes × clean/incremental × at least five paired samples require **70 pairs / 140 route executions**. It retains failures/cancellation, rejects replacement sample identities, stops after the first failure and never promotes its descriptive results to acceptance. The checked-in outstanding matrix has no executable recipe entries; product samples and measured savings remain **zero**. Genuine data, not fixture timings, must supply median/range/IQR, runner-union and critical-path evidence.
5. **Strict integration.** Test-only witnessed inverses preserve historical policy/source guards. New matrix test inputs are enrolled. Exactly two new JSON documents receive existing-style single-file global rules; the executable matrix also owns `selection.contract`. Near-name/unrelated paths still broaden. No `docs/**` exemption. The combined review's cancellation-accounting finding was corrected and tested with actual interrupted-process evidence.

## Verification: keep each source and scope distinct

### Final hosted Static — PASS, not a profile pass

[Run40044808206 / job120723979931](https://github.com/chebizarro/nostrc/actions/runs/40044808206/job/120723979931), attempt1, tests the synthetic merge above. Job duration **1,133s**, within its unchanged1,200s limit.

- Full DEFAULT: **1,261 run, zero failures/errors, 80 explicit skips**, 883.680s (1,181 derived passes).
- Separate actual Linux kernel matrix: **81 passed, zero skips**, 55.236s. It overlaps DEFAULT discovery; do not add it as unique coverage.
- All14 testhost commands exit0. Owned container cleanup and exact AppArmor teardown succeed.
- Original artifact **12541671571**, **31,735,136 bytes**, SHA256 `dceeb8288f37025febdc8f7766916586daded04c9e28dbd2d99b3a9b8486f7bc`; parent independently checks all **5,978 member CRCs**, source receipt and both original logs.
- Source receipt SHA256 `674c0ba281f7d70108c3d4823183711386e24475cc5d98040ad9839d46d6487f` retains `complete=false`, `complete_gate_eligible=false`, `native_profile_acceptance=false`.
- The original unittest driver produces no top-level suite JUnit. Its22 nested fixture XMLs are not a replacement.

### Native — source937, not relabelled final head

At **`937e58e3d400c6f90bb8fb8d1e9692e1070cf506`**, one fresh configure/default Ninja build and serial full CTest finish successfully: **439 registrations =434 passed +5 explicit skips, zero failed**. Two CTest-passing UI harnesses also emit internal SKIP lines; no executed UI coverage is inferred. Both Node web-extension registrations pass.

Three read-only keychain guards pass. The owned direct private bus is reaped and its socket disappears; arbitrary-descendant/native-profile custody is not inferred. All four pins are exact. Parent independently verifies **9,172 manifest entries /540,275,640 file bytes** and **6,531 tracked source/pinned Git blobs**.

Summary SHA256 `4524b8e2d7fe6d6dd62bb207abf6309810d914e28f2aa1c2658b9a6d88880bad`; manifest SHA256 `125dec49207ef20f92a1b0fb28ec1d1f88d427d85fd620cf13af44c051284caa`; named-accounting SHA256 `b75de73479b77bfa96d48d5784c5707aeb3b79844f903681fd294b04498983c9`.

The combined local driver exits **1/incomplete** because this Darwin/Docker Desktop host is not admitted to the disposable GitHub-hosted Linux controller. Local DEFAULT/kernel execution is NOT RUN; the hosted result above supplies its distinct proof. The later937→bd change affects five selection-test/policy files only; it is not native execution at bd or the landing commit.

Final composed local focused verification at bd: **375 run, zero failures/errors, seven explicit Linux-only skips**. Static checkers and ordinary pre-push hook pass; no bypass.

### Retained failures and review

- Earlier native `beb1aa114` configured437 registrations because its private PATH omitted Node. Preserve that incomplete attempt separately; never append two cases to manufacture439. The corrected independent source937 run fingerprints Node before configure.
- Source937 hosted Static [40041712788/job120714080188](https://github.com/chebizarro/nostrc/actions/runs/40041712788/job/120714080188): **1,259 run, one failure,80 skips**, plus81 kernel passes. Failure is the two unclassified JSON documents, not timeout (18m02s job; DEFAULT889.502s). Original artifact12539269705 remains retained. The causal classification commit, not an unchanged rerun, produces the later green Static gate.
- Sourcebeb focused integration255 has two enrollment/witness errors; source937 fixes them. P1's uninitialized/mid-edit discovery and P2's cancelled baseline are diagnostic only, not baseline or completed-gate evidence.
- One bounded combined Oracle/source review (`untitled-chat-59C124`) reviews eaa→beb. Its only new finding is cancellation classification; source937 corrects it with preflight, interrupted-command, running-phase and ordinary-failure regressions. Later witness/classification corrections are directly checked; no second review is claimed.
- Legacy [Mac ASan job120723979958](https://github.com/chebizarro/nostrc/actions/runs/40044808239/job/120723979958) again fails the existing heartbeat assertion at line458 (4misses>2; one failure of268). Test and workflow bytes are unchanged; `.11` stays open, with no new root-cause claim or guard relaxation. Retained log SHA256 `dd03db91bf9626a50745e8fa4c494b1a1eb5eb503a7bf4f7a6c244ce43a001b9`.

## Dated acceptance and actual next blockers

The **2026-10-24T00:50:15.368138Z–00:50:52.324667Z** snapshot links exact bd/f5 checks, jobs and artifacts:51success,8failure,4skipped,8in-progress,2queued,3pending. This is a bounded final-source snapshot, **not a live all-history counter** or a green PR/profile claim. Core ordinary and MDK011 are still running in that snapshot. Subsequent merge/metadata-triggered roots are not included. Original `.5` carries the dated refresh; older failures/snapshots remain intact.

| Bead | Evidence / remaining acceptance |
|---|---|
| `.36.12` CLOSED | Finite missing-microhttpd source repair landed. MDK08 now progresses to the separate freshness refusal, not the original header error. |
| `.36.17` CLOSED | Two private-test daemon isolation repairs landed with real local before/after proof. Linux GUI and general overlay scope are not claimed. |
| `.36.15` in progress | Blueprint source repair/configure progression demonstrated; retain same-profile generated-UI drift/execution proof requirement after ownership is repaired. |
| `.36.13` in progress / original `.8` open | MDK enrollment is not original producer/build/use/cleanup/sanitizer parity. Cold011, full08/09 compatibility, supported selector/native custody and full audits remain required. |
| `.36.14` in progress | Runnable capture tooling and candidate census exist; no product pairs or measured savings. |
| `.36.16` in progress | Actual core Linux/Mac ASan preaudits now each retain exactly three unclaimed registrations: `MockRelayUnit`, `concurrency_subscription_shutdown_asan`, `test_json_envelope`. Fix real bugs; no omission waiver. |
| `.36.18` open | Scope endpoint/token delivery to admitted consumer registrations while preserving physical execution/receipt/custody accounting. |
| `.36.19` open | Groundhog ordinary job120724058820/artifact12541576378:46 unclaimed registrations +638 missing edges, after configure progresses. Exact ownership/fixture proof, not a broad whitelist. |
| `.36.20` open | MDK08 job120724058804/artifact12541521591 and Grotto ASan120724058721/artifact12541088596: preaudits pass with0gaps, then selected-target no-work freshness refuses. Preserve full graph/input/build-lock proof; no graph filtering or `-d norebuild`. |
| `.36.21` open | Grotto ordinary120724058749/artifact12541083817: Xvfb cannot write display number to fd7, then service readiness expires; finalizer error is secondary. Prove FD lifecycle without bypassing custody. |
| `.36.22` open | Groundhog sanitizer120724058644/artifact12541511059: fresh nonmember registration partition differs. Preserve exact reviewed193 authority; no automatic renewal. |

The original requirements still apply: **20 reviewed distinct representative PR comparisons**, **two successful complete broad integrations**, seven classes with at least five clean/incremental samples, median/range/IQR, Grotto≥30% and non-MLS Groundhog≥25% runner-minute reductions (zero unrelated MDK), and critical-path/broad≤10% regression limits or the original measured broad justification. Historical same-source candidates are assessed under the existing protocol; there is no newly imposed prospective-only quota and no implicit dual-source overlay authority.

Reviewed acceptance remains **0/20,0/2**, with zero product samples. PR24 is CI-control work, not a representative product comparison. W11 remains NOT MET / W12 NOT RUN; publisher/defaults/protections/legacy triggers/schedules and release authority are unchanged.

## Durable evidence and references

Protected complete raw copies (not public credentials or immutable remote escrow):

- `/Users/bizarro/Library/Application Support/RepoPrompt CE/evidence/nostrc/pr24-bd0b3b2-hosted-20261024` (summary SHA256 `311d12e0bd69b4e15ec55a8a4885f10a410ff7c9cd9a9d33a5fa1fe66971f203`).
- Sibling `pr24-hosted-final-20261024` and `pr24-hosted-final-20261024-timing` retain the failed937 gate.
- Sibling `final-937e58e3-native-20261024` and `native-beb1aa114-incomplete-437-20261024` retain distinct native attempts.
- Parent independent checks and bounded review: `/tmp/nostrc-w11-priority-20261024/`; P1 causal reproduction: `/tmp/p1w11/repro/`; P2 source/fixture failures: `/tmp/nostrc-p2-mdk-20261024/`. These temporary originals supplement, not replace, protected copies and Git witnesses.

See the [plan/index](../plans/dependency-aware-testing-ci-2026-10-15.md#execution-index), [acceptance audit](dependency-aware-testing-ci-2026-10-18-acceptance-audit.md), [ledger](dependency-aware-testing-ci-2026-10-18-run-ledger.md), [measurement protocol](dependency-aware-testing-w11-e-protocol-2026-10-23.md), [curated P3 baseline](dependency-aware-testing-w11-p3-evidence-2026-10-24.json) and [outstanding executable matrix](dependency-aware-testing-w11-p3-matrix-2026-10-24.json).
