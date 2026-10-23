# W11: hosted measurement and first blockers — 2026-10-23 UTC

Base/master `587ebea254fb3b7c19c8f91a42b23a8a4d950db3`; local setup repair
`fc5333d58447110fff59f3da1741f193e098227d`. Finite work item
`nostrc-0cfsg.36`; completed finite baseline `.5` remains **CLOSED**.
This report owns hosted measurement and two setup corrections, not another
acceptance rewrite. Heartbeat `.11`, W12 hooks/AGENTS/TESTING and provider-blocked
JSON-GLib `.29` remain outside this change. **Internal CI/test/docs: no bump.**

## Frozen evidence and accounting

Full API capture **2026-10-23T00:32:59.251872Z–00:35:51.902641Z**:
154 queries, all four workflow histories, all 50 root runs with exact-attempt jobs
and artifact metadata. **47 failed / one cancelled / one queued / one in progress**;
no successful terminal `Dependency gate`. Standalone broad/worker/release histories
have zero roots. These are sequential observations, not an atomic/live counter.
Final **bounded** refresh **2026-10-23T00:48:24.506164Z–00:48:54.695786Z**
rechecks master/PR19, all 50 listed root identities, and exact-attempt jobs/artifacts
for PR19/master: unchanged **47 failed / one cancelled / one queued / one in
progress**, neither latest root has terminal gate/broad accounting. Older full
bodies are the 00:35 capture, not falsely claimed re-fetched. The
[evidence index](dependency-aware-testing-ci-2026-10-23-w11-evidence.json)
preserves the failed intervening full refresh/TLS timeout and links every root and
all **25** newly verified archives.

**0/20 qualifying reviewed distinct PR comparisons; 0/2 successful complete broad
integrations. W11 NOT MET / W12 NOT RUN.** There are only **19 PR root/API-head
identities** in this history, and none qualifies. Nineteen observed heads are not
nineteen accepted comparisons. Nested job success, macOS standalone libgo success,
local regression tests, retries and pending root colors add no credit.

| Source | Root and exact tested source | Disposition at full snapshot |
|---|---|---|
| PR19 | [39872346136](https://github.com/chebizarro/nostrc/actions/runs/39872346136), API head `cc56887a7b433f6391d13be5ef1688035de7de88`, tested merge `b5c94b6b61befca32a3291e73b9863bf1570c03a` | Queued root with already failed selected jobs; no terminal gate/broad accounting yet |
| Latest master | [39872363501](https://github.com/chebizarro/nostrc/actions/runs/39872363501), tested `587ebea254fb3b7c19c8f91a42b23a8a4d950db3` | In progress with already failed selected/broad jobs; no terminal gate/broad accounting yet |
| Previous PR18 | [39851463581](https://github.com/chebizarro/nostrc/actions/runs/39851463581), tested merge `dede487fcdbcecd88ad7abac2aa9983c3384201c` | Terminal failure: [gate 120133302311](https://github.com/chebizarro/nostrc/actions/runs/39851463581/job/120133302311), [broad 120120486819](https://github.com/chebizarro/nostrc/actions/runs/39851463581/job/120120486819) |
| Previous master | [39851489448](https://github.com/chebizarro/nostrc/actions/runs/39851489448), tested `0ce430299060a09e4a2db250be1a2cac4df2c212` | Terminal failure: [gate 120138388326](https://github.com/chebizarro/nostrc/actions/runs/39851489448/job/120138388326), [broad 120138270049](https://github.com/chebizarro/nostrc/actions/runs/39851489448/job/120138270049) |

PR19's tested merge parents are independently checked through the commit API:
`0ce430299060a09e4a2db250be1a2cac4df2c212` and its API head above. Do not substitute
the subsequently merged master SHA for the PR-tested merge. The two new plan ZIPs
bind these exact tested trees and distinct plan IDs; normalized worker receipts
match their source/plan/inventory identities. Previous PR18/master validation
artifacts `12464408185`/`12464563701` retain `passed=false`, `complete=false`,
`complete_gate_eligible=false`, with **2,339/2,340 errors** respectively; their
retained broad-family receipts also fail. Archive API digest/size, ZIP CRC and
member hashes are verified. The evidence index distinguishes fresh archive reads
from historical evidence; it does not imply all artifacts were downloaded.

## First concrete blockers, not inferred remedies

| Boundary | Exact observation | Disposition / next action |
|---|---|---|
| Selected ordinary/root execution | PR19 receipt artifacts `12466734246`, `12466812885`, `12467121281`, `12467453522`, `12468342374`: unavailable `signer_endpoint`; core Linux also `display`/`private_bus`. Latest master [core job 120149703110](https://github.com/chebizarro/nostrc/actions/runs/39872363501/job/120149703110) repeats the latter refusal. | **Unresolved.** Outer Xvfb/bus presence is not profile capability or signer approval. Requires real approved fixture/custody and coordinated profile evidence; no capability declaration, unwrapping, skip-as-success or inventory expansion here. |
| Standalone Linux libgo setup | PR19 [job 120149644640](https://github.com/chebizarro/nostrc/actions/runs/39872346136/job/120149644640), artifact `12467813181`: `default configured/PATH build tool identity mismatch: cmake`. | **Local setup repair only:** expose pip package `CMAKE_BIN_DIR`, preserving version 3.31.6 and strict identity check. Hosted patched-source lane remains NOT RUN. |
| Windows broad configure | Master [job 120149704291](https://github.com/chebizarro/nostrc/actions/runs/39872363501/job/120149704291): official NIP-44 JSON-vector SHA mismatch. | **Local checkout repair only:** process-scoped `core.autocrlf=false`. Physical Git regression reproduces CRLF mismatch and byte-exact restoration. Original hosted file bytes were not retained: CRLF causality is a supported inference, not direct hosted readback. Native Windows patched-source verification remains NOT RUN. |
| Hosted policy/kernel custody | PR19 [policy job 120149644494](https://github.com/chebizarro/nostrc/actions/runs/39872346136/job/120149644494), artifact `12467989087`: **808 tests / 44 failures / 15 errors / six skips**, 1054.796s. First pidns adapter cases exit125 before target readiness: permission denied opening `/proc/self/setgroups`. | **Unresolved `.23`**, evidence added to bead. Runtime primitive/provisioning must be established on the actual worker; do not weaken namespace cleanup or skip failures. |
| Broad static wrapper | Master [job 120149704517](https://github.com/chebizarro/nostrc/actions/runs/39872363501/job/120149704517): fails/errors appear before the ten-minute job cancellation. | Timeout increase alone is not a fix. Preserve cancellation and underlying policy failures; no timeout/coverage change here. |
| Sanitizer partition | PR19 artifact `12468361604`: `diagnostic nonmember fresh registration partition differs`. | **Unresolved.** Fresh registration evidence must agree with the existing reviewed authority. No automatic renewal, omitted registration or sanitizer-policy relaxation. |
| Selected MDK profiles | PR19 artifacts `12467461643` / `12468408026`: unavailable `gnome_keyring`, `nak`, `signer_endpoint`. | Actual executable fingerprints, separate private services/approved signer and original lifecycle proof remain distinct. Not repaired by ordinary-profile setup or local MDK partial evidence. |
| Other broad job failures | Latest master [core macOS job 120149704405](https://github.com/chebizarro/nostrc/actions/runs/39872363501/job/120149704405) and [JSON fuzz job 120150143480](https://github.com/chebizarro/nostrc/actions/runs/39872363501/job/120150143480) fail. | Job conclusions observed, not new test-level cause attribution. Separately, owner-confirmed `.11` safe remedy remains unresolved and post-burst gap exclusion rejected; `.29` remains outside scope. Later passes are not remedy/closure evidence. |

The retained PR19 policy log member SHA-256 is
`3efafbfd2877d70b8e97c1b4b0d217fb61f06d584ad8b064fec3f1e1d908444d`.
The broad static cancellation is a different execution of the suite, not that
complete 808-case result. No core/inventory/capability interface was edited.

## Finite implementation and verification

- **PASS:** two physical regressions: real Git checkout of the pinned vector with
  CRLF enabled/checkout-scoped override, and real miniature CMake configure with
  launcher-vs-binary PATH selection. The latter uses an explicit package-metadata
  fixture; it is not represented as installation or whole-profile acceptance.
- **PASS:** actual cached pip **3.31.6** package, extracted workflow PATH-export,
  miniature Ninja configure/default build in immutable image
  `sha256:419ac342fe39a966440ed569295b12b9f6b46cfb3c5129111e39e2d48114ab4a`.
  Selected/configured CMake is `/opt/cmake3316/cmake/data/bin/cmake`, binary SHA-256
  `57452fa01ed0f69552c06d3e25edd0d9375347b982dfb709393526cb45396d6b`.
  This is Linux/amd64 **emulated** setup proof, not native hosted/profile evidence.
- **PASS:** existing workflow suites **52/52**, failure-receipt regressions **17/17**,
  both changed workflows' actionlint/ShellCheck, four repository static checks and
  `git diff --check`. Setup regressions add **2/2** separately, not a full-suite run.
- **PASS:** fresh native Debug/Ninja default build, safe backend flags
  `-DNIP55L_SECRET_BACKEND=libsecret`
  `-DGROUNDHOG_STORE_KEY_DEFAULT_BACKEND=secret-service`; root Groundhog default OFF.
  Pinned submodules restored; all default targets build. Initial missing-submodule
  configure failure and interrupted network/pip setup remain retained. Root CTest
  **NOT RUN**; no keychain defaults/search list changed.
- **One bounded review:** first request lacked transported files; continuation of
  the same review received the exact diff/context inline and found **no actionable
  findings**. No second review or independently rerun test claim.

Raw commands/logs, original archives, source hashes and receipts:
`/tmp/nostrc-w11-20261023/`. The durable evidence index retains hashes and GitHub
links because temporary bytes and hosted artifacts can expire. Tested working-tree
setup bytes equal local commit `fc5333d58`; shipped source is unchanged from master.
No push/merge: **parent lands both local commits**.

## Actionable run boundary and W11 checklist

| Required evidence | Current result / finite next run |
|---|---|
| Corrected Linux libgo + Windows setup | Parent lands `fc5333d58`; inspect the ensuing ordinary master/next real PR lanes at their exact tested source. A Windows build-only manual dispatch is available after landing if the normal shadow lane does not exercise it. No hosted execution of local-only bytes is claimed. |
| Full selected-worker usability | Provision approved signer/resources and resolve actual hosted custody/partition failures first, then run supported exact-source lanes. Preserve refusal while those prerequisites are absent. |
| 20 reviewed representative PR comparisons | **0/20; NOT MET.** Existing 19 observed heads are insufficient even numerically, and failed/incomplete coverage cannot supply justified-omission/cost comparisons. Capture real future comparisons; no synthetic PRs or split/retry credit. |
| Two complete broad integrations | **0/2; NOT MET.** Existing PR19/master broad executions already exercise the real union and have failures. No unchanged cold broad/manual retry was dispatched. Rerun only after a concrete blocker/source/setup change with retained original failure. |
| Representative savings / critical paths / broad median | **NOT ESTABLISHED.** No complete qualifying paired sample or five-sample performance matrix is fabricated from nested durations or partial jobs. |
| Selected-skip/cancel, merge-group/protection/operator acceptance | Existing finite regressions are not live rollout approval. No new settings audit, merge-group execution or operator approval is claimed. Preserve old checks; no default, publisher, protection, trigger or schedule cutover. |

**W11 cannot finish from the existing evidence.** The useful deliverable is the
source-bound blocker diagnosis plus two bounded setup repairs, not waiting idle
for twenty PRs or relabeling local/partial passes as integrations.
