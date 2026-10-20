# Dependency-aware testing acceptance run ledger

## Follow-up snapshot — 2026-10-20T23:44:45.035117Z

**Current frozen snapshot**, captured sequentially
`2026-10-20T23:41:39.393572Z`–`2026-10-20T23:44:45.035117Z` UTC
(October 21 local). Base `1fad10a239f9cef72b7b056b8ab99679337d53c4`.
Not a live dashboard or atomic API snapshot. All older text below is retained
historical evidence, including prior pending/failed/cancelled observations.

### Reviewed root dispositions

All four workflow histories were paginated and matched API totals. Every
shadow root's attempts `1..run_attempt` and attempt-specific jobs were fetched,
source/attempt IDs checked; all 14 currently have only attempt 1. Counts derive
from these reviewed dispositions, **not parent colors or nested-job counts**.

| Run / attempt | API head | Event / parent | Actual broad accounting / qualifying disposition |
|---|---|---|---|
| [38833746063/1](https://github.com/chebizarro/nostrc/actions/runs/38833746063) | `5467c02d1` | push / failure | [failed](https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116770701220); rejected |
| [38845886339/1](https://github.com/chebizarro/nostrc/actions/runs/38845886339) | `33a101276` | push / failure | not launched: selection failed; rejected |
| [38856570439/1](https://github.com/chebizarro/nostrc/actions/runs/38856570439) | `8a031f443` | push / failure | [failed](https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116841163905); rejected |
| [38873978927/1](https://github.com/chebizarro/nostrc/actions/runs/38873978927) | `a47807050` | manual / cancelled | [failed](https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116900093654); rejected |
| [38878691465/1](https://github.com/chebizarro/nostrc/actions/runs/38878691465) | `249f9657f` | push / failure | [failed](https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116915712972); rejected |
| [39132650260/1](https://github.com/chebizarro/nostrc/actions/runs/39132650260) | `2743f1735` | push / failure | [failed](https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117700034296); rejected |
| [39194059430/1](https://github.com/chebizarro/nostrc/actions/runs/39194059430) | `5280ea324` | push / failure | [failed](https://github.com/chebizarro/nostrc/actions/runs/39194059430/job/117882302404); rejected |
| [39194781902/1](https://github.com/chebizarro/nostrc/actions/runs/39194781902) | `2c5b18c23` | push / failure | [failed](https://github.com/chebizarro/nostrc/actions/runs/39194781902/job/117883493320); rejected |
| [39196963896/1](https://github.com/chebizarro/nostrc/actions/runs/39196963896) | `e57ef16ed` | push / failure (was pending) | [failed](https://github.com/chebizarro/nostrc/actions/runs/39196963896/job/117892316778); rejected |
| [39198909741/1](https://github.com/chebizarro/nostrc/actions/runs/39198909741) | `fb7335cb0` | push / failure | [failed](https://github.com/chebizarro/nostrc/actions/runs/39198909741/job/117902065231); rejected |
| [39202094613/1](https://github.com/chebizarro/nostrc/actions/runs/39202094613) | `6ce261c42` | push / failure | [failed](https://github.com/chebizarro/nostrc/actions/runs/39202094613/job/117912056510); rejected |
| [39271217772/1](https://github.com/chebizarro/nostrc/actions/runs/39271217772) | `b72b9ec70` | push / failure | [failed](https://github.com/chebizarro/nostrc/actions/runs/39271217772/job/118146290151); rejected |
| [39280037488/1](https://github.com/chebizarro/nostrc/actions/runs/39280037488) | `d120bc972` | PR #1 / failure | [failed](https://github.com/chebizarro/nostrc/actions/runs/39280037488/job/118179549498); selected failures too; PR comparison rejected |
| [39280051357/1](https://github.com/chebizarro/nostrc/actions/runs/39280051357) | `1fad10a23` | push / failure | [failed](https://github.com/chebizarro/nostrc/actions/runs/39280051357/job/118172318805); rejected |

**0/20 qualifying reviewed distinct PR comparisons; 0/2 successful complete
broad integrations. W11 NOT MET / W12 NOT RUN.** Distinct representative PRs
still require source/range-bound legacy/selective evidence and reviewed
differences; complete broad acceptance still requires successful accounting and
reviewed full required-workload/receipt union. No criterion is waived.

Repository history since `2026-10-15T00:00:00Z`: **190 roots**, 167 push /
13 schedule / 6 PR / 4 manual. The six PR workflow events all have head
`d120bc972` (static, libhanami, Grotto, Groundhog, nostrc, shadow); these are
**one PR candidate**, not six distinct comparisons. No qualifying credit.
Shadow: 14 roots / 14 attempts, 12 failed pushes + 1 failed PR + 1 cancelled
manual. Thirteen launched broad calls each expose 47 jobs: **611 observations**
(541 success / 52 failure / 14 skipped / 4 cancelled), with **13 failed family
accounting jobs**. The other root never launched broad. No pending call remains.
Broad/worker/release standalone workflow histories remain 0/0/0, not zero nested
executions. Exact-source broad workflow bytes (all API heads) retain SHA-256
`79d5ce5af5f517d037b83fc5d4c92d3d31172a5ef059202e0a4b8ffe00d383b6`,
requiring every family result to equal success. Existing failures, cancellations,
advisory distinctions and skips are retained; no successful full union inferred.

### PR identity and exact failed workers

For PR #1, API/job head is `d120bc972bbb0f4c09951e376bcc229b41931e0e`;
**tested source is `39dcccd70e629adb3efb0d904700afe1e45793f9`**, the synthetic
merge with parents `b72b9ec70f4aee51a4a5f42447bc763cce5cc516` and that PR head.
The [Git commit API](https://api.github.com/repos/chebizarro/nostrc/git/commits/39dcccd70e629adb3efb0d904700afe1e45793f9)
and [actual worker log](https://github.com/chebizarro/nostrc/actions/runs/39280037488/job/118152909114)
(`refs/pull/1/merge`, checkout/input SHA) establish the relationship. Do not
silently equate API head and tested source. Every PR payload binds the tested
source, inventory and plan `a6ebb802ba08794d50d49d9ec1938fdfa29a4e1907199200ea3a583d3239553a`.
All attempt suffixes are 1; backend retries are not GitHub attempts.

The [follow-up audit](dependency-aware-testing-ci-2026-10-18-acceptance-audit.md#pr-1-worker-findings-and-bounded-next-fix)
records each lane's exact failure stage and findings. Nine failed selected
workers retain failures; only standalone libgo Linux/macOS and static policy
succeed as bounded diagnostics. Ordinary Groundhog/Grotto fail at resolve on
reviewed toolchain drift, before build/CTest; no missing-impact or membership
finding is reclassified by the new worker PATH fix. Shared sanitizer partition,
core/Grotto full audits and live MDK lifecycle refusal remain blockers.

| Newly reviewed aggregate | Artifact | ZIP SHA-256 | Retained result |
|---|---|---|---|
| `39196963896` | `12163810593` | `e008a02d6bada4d975205b4727a337bf5f265535b16c84ffd005e4b2ab34ba3d` | passed=false, eligible=false, complete request=true; 465 errors |
| `39198909741` | `12164233557` | `ccdaf5891a2f5a6e69d722947a806031aeff44c6af340c47ac0eed531bd0bda6` | passed=false, eligible=false, complete request=true; 465 errors |
| `39202094613` | `12166635041` | `b71ef1d0f3c85dfb299d33af9004d3147dbc425ae0aa92b0194a7ab8425b695f` | passed=false, eligible=false, complete request=true; 469 errors |
| `39271217772` | `12197688339` | `a959bb6e68989c6707bcf20b90ad8f2cae44503fb682f4e605b6f826da7725e0` | passed=false, eligible=false, complete request=true; 469 errors |
| `39280037488` | `12202839496` | `558efdd3b8e0f92da1a4abd7b6b50e66dcb94b472ad60acd63d59c6f6fb4d4cc` | passed=false, eligible=false, complete=false; 471 errors |
| `39280051357` | `12204875011` | `51b6c573eff92ea0e044c983e83c7b748e04a7acdecb3190d374021261e61383` | passed=false, eligible=false, complete=false; 471 errors |

### Integrity and reproducibility limits

534 API-visible artifacts. **171/171** small plan/receipt/aggregate ZIPs match
API SHA-256 and size, pass CRC, and bind payload identity where available:
13 plans, 13 failed aggregates, 116 normalized lane receipts, 9 historical raw
failed audits lacking payload plan/lane fields, 20 historical log-only archives.
The last 29 remain explicit negative/incomplete evidence, not normalized success.
All **100** cached historical archive/member hashes equal the previous index;
**71** additional archives were retrieved/validated against the new API capture.
Of the 363 artifacts outside this small set, 12 PR raw evidence ZIPs were also
inspected separately (matching API digest/size/CRC); the other 351 were not
retrieved. No downloaded binaries/installers were executed.

PR small-archive hashes (artifact IDs resolve under
`https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/<id>`):

```text
plan                    12198423491 cc98766b9d07bc9842874fb37626f854cc43624ce816c0a87f1be5ded7c53fe0
core.linux              12201515906 29255db6a5a06684bbed4b09ab6f2fdbbb68b17e96f5ba31cae221baffc6b566
core.linux.asan         12201400042 774ef0ddede653b86baf29f3c8de352bec4d31632651d74aac8df30ac4bf7a35
core.macos.asan         12201736755 182c69fc7019e1bf91de520d86c0edb5c4d2e4e196b1943ebff33deb3406a07a
grotto.linux            12201635211 985f749ff6982e84bce4eb6bf0a1b40bb5c25eea8e8dd57a64246233c222de7c
grotto.linux.asan_ubsan  12200198920 a473fbe6a8768923e8c44c8a9c695c8c39785eb83e27936ca0ed1579af27682b
groundhog.linux         12202406176 7e51d9bec0c7e2f1bc096d96b56690238d4548f4afc709d51f1b5a8ae33c254f
groundhog.sanitizers    12201771381 f145031a330fb051beb58eb9cb27d61dc30bfe89c3168366e4dc67d66483b036
groundhog.mdk011        12201192525 fe0ae490cac2c1d37b2a498b8717d4c8b7830597854e7344a2c6636dbe4f7451
groundhog.mdk08         12202011656 5230887ec1423701b40fdcac6824387f8b3410fc18d9710f76468d7446976ebb
libgo.linux.asan        12201242112 d09682777af3d3be22c2fc8afbe224a8ae0bc0e9a591fd4a2830442442c8dad7
libgo.macos.asan        12200958584 09f2d8ec06fd48843a9cf0532122f88624bb57f9111c3e33bcbbb8a2128b64eb
static.policy           12201942608 64c7a33eb79d7d36d1f548f656bb2df01b0c439a8470b539f21b8b66ebfe593f
```

Temporary raw capture: `/tmp/nostrc-ci-followup-20261021/`; it and GitHub raw
artifacts can expire. The retained hashes/links do not guarantee future raw-byte
availability. API histories use the same four workflow IDs and repository
window as the previous capture. `queries.json` retains each query's UTC start/
finish, API path, response SHA-256, pagination and totals; `capture-summary.json`
retains all full root source/attempt IDs. First five query windows are
23:41:39.393696–23:41:48.525439 (shadow), 23:41:48.525879–23:41:52.994207
(broad), 23:41:52.994549–23:41:55.604492 (worker),
23:41:55.604932–23:41:56.031878 (release),
23:41:56.032211–23:42:09.630055 (repository), all October 20 UTC.
Checks include cached historical equality, PR merge-parent/checkout identity,
source/plan/inventory/lane binding and root-deduplicated rejected dispositions.
Initial PR archive retrieval hit a TLS handshake timeout; retrieval subsequently
succeeded with verified bytes. No hosted attempt was retried or relabelled.

```text
capture.py                     0391e774f2c115c6264c5a80efaa653ed612ce6cb43e483996cd56515d192f68
capture-summary.json           23003b9158b7567e924ccdd530b1a58b0a06e6417fa90a16063723783557b846
queries.json                   e1e0db77da6c768b9b59d6d0d711b5be64c53d904839b4717c05b6e715f82377
validate_artifacts.py          3364fb7aa501b2f2ca49601b5033526763048d03bb5ca3c28c794cd379426bb1
artifact-validation-result    eecf397dff1a50f1d696e0230d71e304e17a9d70d97518a3b08a8a6b21eb538c
artifact-retrieval-index.json  ff214c9136c7af349ebc1041c6e43f2c9bee9d724785f1f1f1625a1ce7cd748d
pr1-tested-commit.json         8939bda8f0948394db06a8e49c4bc4cf78e9de53a5753055bfe184a2eed57a28
runner-software.md             3911b4bc1aa423be7b7469a75f3093f51306c02f2638f2a8b4f5ed31f3ffe791
```

Final source-bound verification: `verify-followup.py` PASS checks all API
response hashes/pagination, 14 rejected dispositions, repository PR deduplication,
171 artifact identities / 100 historical equal, PR merge/source/lane binding,
ordinary refusal/incomplete flags, historical document-body preservation and the
five-file ownership boundary. It also proves inventory differs only in the two
worker source-accounting hashes, not any profile or subset authority. Script
SHA-256 `a836c6385d0d69e9f2a93fd540814498b22b67bd9fbea3c455662faac5b4963c`;
log `75c51e5ecd18dac4c9d2427b68c54a19e83cd8c5f21458708792a0717aeb5f71`.
[Local implementation gates and their limits](dependency-aware-testing-ci-2026-10-18-acceptance-audit.md#pr-1-worker-findings-and-bounded-next-fix)
are separate from this frozen hosted snapshot, never additions to its counters.
Jev advisory range triage: 89 requests / 755,636 input tokens / USD 0.031736;
range suggestions guided bounded reads, while broad inventory ranking was not
decisive and structured source/hash checks supplied the evidence. No truth,
acceptance authority or measured savings is assigned to these judgments.

Protection, release mode, baseline costs and native/product acceptance were
**not refreshed**. No default/enforcement/release, legacy gate/trigger/schedule,
protection or acceptance-policy cutover; no unmet criterion/bead closed.
Version assessment: internal worker/tests/evidence only, **no component bump**.

## Historical 2026-10-19 snapshot retained

The following body is preserved unchanged, including its dated “current” wording.

**Current versioned snapshot: 2026-10-19T04:10:01.512997Z**, observed over
`2026-10-19T04:09:10.542307Z`–`2026-10-19T04:10:01.512997Z` UTC.
This is not a live dashboard or an atomic API snapshot; per-query windows are
retained below. Later uploads/status changes require another review, not inferred
acceptance. Beads remains the task tracker.

## Current count at this snapshot

- **0/20 qualifying reviewed distinct PR comparisons; 0/2 successful complete broad integrations.**
- **9 shadow roots / 9 attempts**: 7 failed pushes, 1 cancelled manual run,
  1 pending push. Every API-visible root currently has attempt 1; all were queried.
- **7 failed nested broad accounting jobs**, 1 selection failure before broad
  launch, 1 incomplete nested broad call. The cancelled parent retained failed
  broad accounting; the pending parent already has launched/failed children.
- **160 repository roots** since `2026-10-15T00:00:00Z`: 147 pushes, 9 scheduled,
  4 manual, **zero PR events**. Broad/worker/release each have zero standalone
  history records; their nested jobs are not additional integrations.
- **292 API-visible artifacts; 100 selected small archives verified** (8 plans,
  7 aggregates, 85 receipt-labelled archives). This includes 59 historical
  archives revalidated unchanged and **41 newly reviewed** archives (14 + 14 + 13).
  192 other archives (1,186,405,706 metadata bytes) were not downloaded. Selected
  bytes total 5,243,572; no installers, application binaries or source archives
  were downloaded or executed.

See the [criterion audit](dependency-aware-testing-ci-2026-10-18-acceptance-audit.md#current-api-refresh--2026-10-19)
for current blockers. **W11 NOT MET / W12 NOT RUN**; no bead closure, policy,
default, protection, requirement, schedule or release-mode change; docs-only,
no component version bump.

## Current root and nested accounting review

| Run / attempt | Source | Event | Parent | Actual nested broad accounting | Small archives |
|---|---|---|---|---|---|
| [38833746063/1](https://github.com/chebizarro/nostrc/actions/runs/38833746063) | `5467c02d1` | push | failure | [failure](https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116770701220) | 9 |
| [38845886339/1](https://github.com/chebizarro/nostrc/actions/runs/38845886339) | `33a101276` | push | failure | not launched (selection failed) | 0 |
| [38856570439/1](https://github.com/chebizarro/nostrc/actions/runs/38856570439) | `8a031f443` | push | failure | [failure](https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116841163905) | 12 |
| [38873978927/1](https://github.com/chebizarro/nostrc/actions/runs/38873978927) | `a47807050` | workflow_dispatch | cancelled | [failure](https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116900093654) | 12 |
| [38878691465/1](https://github.com/chebizarro/nostrc/actions/runs/38878691465) | `249f9657f` | push | failure | [failure](https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116915712972) | 12 |
| [39132650260/1](https://github.com/chebizarro/nostrc/actions/runs/39132650260) | `2743f1735` | push | failure | [failure](https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117700034296) | 14 |
| [39194059430/1](https://github.com/chebizarro/nostrc/actions/runs/39194059430) | `5280ea324` | push | failure | [failure](https://github.com/chebizarro/nostrc/actions/runs/39194059430/job/117882302404) | 14 |
| [39194781902/1](https://github.com/chebizarro/nostrc/actions/runs/39194781902) | `2c5b18c23` | push | failure | [failure](https://github.com/chebizarro/nostrc/actions/runs/39194781902/job/117883493320) | 14 |
| [39196963896/1](https://github.com/chebizarro/nostrc/actions/runs/39196963896) | `e57ef16ed` | push | pending | not yet returned; call incomplete | 13 |

The latest **completed** root `39194781902` (`2c5b18c23`) has 47 nested
broad jobs: 40 success, 2 failure (JSON fuzz and family accounting), 4 cancelled
Grotto variants (including the explicitly advisory TSan variant), 1 skipped fuzz
coverage report. Root `39194059430` (`5280ea324`) has 47: 43 success, 3 failure
(macOS core ASan/UBSan, JSON fuzz, family accounting), 1 skipped coverage report.
No failed/cancelled/skipped job is relabelled success.

Pending root `39196963896` (`e57ef16ed`) already exposes **45 nested broad
jobs**: 34 success, 1 failure (JSON fuzz), 7 in progress, 3 pending. Family
accounting, coverage-report and dependency-gate jobs/aggregate have not yet been
returned; none is assumed successful or permanently absent. Its 12 selected
workers finished (3 success / 9 failure) and all 12 lane receipts are retained,
but that is not complete broad-root or aggregate evidence.

Across all roots, 374 nested broad jobs are 315 success, 38 failure, 7 skipped,
4 cancelled, 7 in progress and 3 pending. These **are job observations, not 374
integrations**. Exact-source `dependency-broad.yml` accounting requires every
family result to equal success; the actual accounting job conclusions above,
not parent colors or standalone history emptiness, determine rejection. Full
required-workload union acceptance is not established by a successful subset.

## Artifact and identity validation

All 100 downloaded archive sizes and SHA-256 digests equal their frozen API
metadata; all ZIP CRC checks pass; every member hash is retained (historical 59
below, new 41 in the current data). Metadata `workflow_run.id/head_sha`, exact
source, plan/inventory identity where present, lane/profile and attempt-suffixed
artifact names were checked against the parent API and exact-source upload
workflows. All returned attempts are 1; backend `failure.attempt` is a separate
local retry counter, **not** the GitHub attempt identity.

Identity granularity is explicit: **56 normalized lane receipts** bind payload
source/plan/inventory/lane; **9 historical raw failed audits** lack payload
plan/lane fields and bind only their available source/inventory plus API/name/
member-path provenance; **20 historical log-only archives** have no payload
identity or normalized receipt. The latter 29 are retained negative/incomplete
evidence, never upgraded to normalized execution coverage. Eight plan payloads
and seven aggregate payloads bind their exact parent source/plan. Initial local
validation stopped on an assumed `plan_id` in a historical raw audit, after ZIP
integrity succeeded; the corrected validator explicitly classifies those old
schemas and log-only archives. Both validation logs are retained, not overwritten.

Fresh captures and scripts are in `/tmp/nostrc-acceptance-refresh-20261019/`.
`capture.py` checks pagination totals, distinct roots and every attempt's jobs;
`validate_artifacts.py --cached` rechecks the freshly retrieved bytes without
redownloading; `build_refresh.py` derives counters, checks all 59 historical
archive/member hashes and retains the historical snapshot. Script, API-response,
workflow and result hashes are committed below. Local `/tmp` captures and GitHub
artifacts expire: retained metadata/hashes do not promise future raw availability.
No old raw capture/generator was treated as current authority or modified.

## Current canonical data

This schema-2 snapshot incorporates the unchanged historical lane/member detail
by run/attempt/artifact ID reference, not by silently rewriting that evidence.
Historical local gate, protection and release-mode observations were **not**
rerun or refreshed and remain attributed to their original time/source below.

<!-- acceptance-ledger-refresh-json:start -->
```json
{
  "schema_version": 2,
  "repository": "chebizarro/nostrc",
  "snapshot_recorded_at": "2026-10-19T04:10:01.512997Z",
  "capture_started_at": "2026-10-19T04:09:10.542307Z",
  "capture_interval_notice": "Sequential API observations, not an atomic server snapshot. Each query retains its UTC observation window. Artifact downloads validate the frozen metadata; no later run status or uploads are silently incorporated.",
  "documentation_source_sha": "e57ef16ed1aeff152d808aec407c1b6f86bfc6be",
  "scope": {"workflow_ids":[386168204,386168202,386168205,386168209],"workflow_history":"All API-visible roots; every attempt 1..run_attempt fetched; all currently return attempt=1. Deleted/inaccessible histories not inferred.","standalone_workflow_counts":{"shadow":9,"broad":0,"worker":0,"release":0},"repository_context_since":"2026-10-15T00:00:00Z","repository_context_returned":160,"repository_context_event_counts":{"push":147,"schedule":9,"workflow_dispatch":4},"repository_context_pr_run_count":0,"raw_capture_directory":"/tmp/nostrc-acceptance-refresh-20261019","raw_retention":"Local temporary files are not durable storage; committed identities/hashes and historical data survive retention, not a guarantee of retrievable artifact bytes.","protection_release_mode_local_gates":"NOT re-queried/re-executed in this refresh; original source/time-bound observations retained only as history."},
  "qualification_policy": {"pr":"Distinct representative PR with source/range-bound legacy and selective evidence, reviewed difference classification and regression for any legacy-only failure. A green run alone is insufficient; pushes/manual runs and local test cases are not PRs.","broad":"Complete successful full broad family accounting and reviewed required-workload/receipt union at exact source; advisory distinctions remain explicit. Root-run deduplication prevents counting nested jobs/artifacts as integrations.","counter_rule":"Counters are derived only from explicit accepted dispositions, not status colors, candidate count, unit test counts or complete-request flags.","cutover":"Unchanged W11 evidence plus operator approval; W12 is not authorized by this audit."},
  "counts": {"qualifying_pr_comparisons":0,"target_pr_comparisons":20,"qualifying_broad_integrations":0,"target_broad_integrations":2,"shadow_roots":9,"shadow_attempts":9,"shadow_failed":7,"shadow_cancelled":1,"shadow_pending":1,"broad_accounting_failed":7,"broad_not_launched":1,"broad_incomplete":1,"nested_broad_jobs":374,"api_visible_artifacts":292,"artifacts_retrieved":100,"new_artifacts_since_historical_snapshot":41,"historical_artifacts_revalidated":59},
  "historical_snapshot": {"snapshot_recorded_at":"2026-10-19T03:01:13.808922Z","reconciliation_updated_at":"2026-10-19T03:11:57.005478Z","original_document_sha256":"e332d96e79e59f74caa0bb78a4c5e950da3ced9f5e2e8deab309ad0e2437a843","canonical_json_sha256":"46a64ca566b3f8fdbffdbad54e5fda8e1f6759483f2cf27f0a1da4ffbb62a56e","retained":"Historical canonical data and original prose below; all six failures/cancellations, local evidence, hashes and prior dispositions unchanged.","artifact_revalidation":"All 59 historical artifact IDs, archive digests and member hashes equal the original committed data; no historical bytes overwritten."},
  "artifact_integrity": {"archives_checked":100,"published_digests_matching":100,"crc_failures":0,"semantic_identity_checks":{"plan":8,"lane_receipt":56,"validation":7,"historical_log_only_no_payload_identity":20,"historical_raw_failed_audit_no_payload_plan_id":9},"excluded_archive_count":192,"excluded_bytes_not_downloaded":1186405706,"attempt_identity":"Artifact name suffix tied to parent run/attempt API and exact-source upload workflow. failure.attempt is backend-local, not GitHub run_attempt.","binary_execution":false},
  "queries": [
    {"name":"shadow-runs-pages","api_path":"repos/chebizarro/nostrc/actions/workflows/386168204/runs?per_page=100","started_at":"2026-10-19T04:09:10.542449Z","finished_at":"2026-10-19T04:09:11.287471Z","response_sha256":"5b3f97d860959f77b7d777fd15f77d8bba7a614017e1bf125d5a0ef0f78b589e","pages":1,"reported_total":9,"returned":9},
    {"name":"broad-runs-pages","api_path":"repos/chebizarro/nostrc/actions/workflows/386168202/runs?per_page=100","started_at":"2026-10-19T04:09:11.287926Z","finished_at":"2026-10-19T04:09:11.663051Z","response_sha256":"f2aa3f2f9ea19c33f16347ee49859287a9b12cde87a89f7d6927ecb020c6b673","pages":1,"reported_total":0,"returned":0},
    {"name":"worker-runs-pages","api_path":"repos/chebizarro/nostrc/actions/workflows/386168205/runs?per_page=100","started_at":"2026-10-19T04:09:11.663346Z","finished_at":"2026-10-19T04:09:12.055827Z","response_sha256":"f2aa3f2f9ea19c33f16347ee49859287a9b12cde87a89f7d6927ecb020c6b673","pages":1,"reported_total":0,"returned":0},
    {"name":"release-runs-pages","api_path":"repos/chebizarro/nostrc/actions/workflows/386168209/runs?per_page=100","started_at":"2026-10-19T04:09:12.056130Z","finished_at":"2026-10-19T04:09:12.424450Z","response_sha256":"f2aa3f2f9ea19c33f16347ee49859287a9b12cde87a89f7d6927ecb020c6b673","pages":1,"reported_total":0,"returned":0},
    {"name":"repository-runs-pages","api_path":"repos/chebizarro/nostrc/actions/runs","started_at":"2026-10-19T04:09:12.424715Z","finished_at":"2026-10-19T04:09:15.963852Z","response_sha256":"115ee6788d37ea2d1d0d0664c41c8c9781a893cb3122fa7efecc3b68dee1a7f5","pages":2,"reported_total":160,"returned":160},
    {"name":"38833746063-attempt1","api_path":"repos/chebizarro/nostrc/actions/runs/38833746063/attempts/1","started_at":"2026-10-19T04:09:15.965489Z","finished_at":"2026-10-19T04:09:16.383350Z","response_sha256":"baaec1a197f001b4eaeb524bf3a72e220f5785553a55c90a106a6f4bcfbac96b"},
    {"name":"38833746063-attempt1-jobs","api_path":"repos/chebizarro/nostrc/actions/runs/38833746063/attempts/1/jobs?per_page=100","started_at":"2026-10-19T04:09:16.383685Z","finished_at":"2026-10-19T04:09:21.981933Z","response_sha256":"563f0b90430297ad0c5ce2f3c0188052994ad0cae388dbfb9876283e11e82d3e","pages":1,"reported_total":56,"returned":56},
    {"name":"38833746063-artifacts","api_path":"repos/chebizarro/nostrc/actions/runs/38833746063/artifacts?per_page=100","started_at":"2026-10-19T04:09:21.982417Z","finished_at":"2026-10-19T04:09:22.367858Z","response_sha256":"eedb8b69ce4fabbff85bb9e0fe8bf9ec2a8ac692d861c18342e18f855f6f6681","pages":1,"reported_total":24,"returned":24},
    {"name":"38845886339-attempt1","api_path":"repos/chebizarro/nostrc/actions/runs/38845886339/attempts/1","started_at":"2026-10-19T04:09:22.368649Z","finished_at":"2026-10-19T04:09:22.859482Z","response_sha256":"88731c86f2dfecb310efe1b80bea29ce47eb0b1107e008cb19e3512a12b343d0"},
    {"name":"38845886339-attempt1-jobs","api_path":"repos/chebizarro/nostrc/actions/runs/38845886339/attempts/1/jobs?per_page=100","started_at":"2026-10-19T04:09:22.860282Z","finished_at":"2026-10-19T04:09:23.551894Z","response_sha256":"08f567f62918431d8db119e9d899ccc25e0254e10c81641f25fe091e5fa66a5f","pages":1,"reported_total":10,"returned":10},
    {"name":"38845886339-artifacts","api_path":"repos/chebizarro/nostrc/actions/runs/38845886339/artifacts?per_page=100","started_at":"2026-10-19T04:09:23.552911Z","finished_at":"2026-10-19T04:09:23.896135Z","response_sha256":"af3c25b59e7048d2e4b6ea03a688b2eced4e1786b5ae40f71516bc3ce0d4149e","pages":1,"reported_total":0,"returned":0},
    {"name":"38856570439-attempt1","api_path":"repos/chebizarro/nostrc/actions/runs/38856570439/attempts/1","started_at":"2026-10-19T04:09:23.896394Z","finished_at":"2026-10-19T04:09:24.306827Z","response_sha256":"9ea8c06438584fa5465f36130b6ee7233c395df6855877032639f79133daf3df"},
    {"name":"38856570439-attempt1-jobs","api_path":"repos/chebizarro/nostrc/actions/runs/38856570439/attempts/1/jobs?per_page=100","started_at":"2026-10-19T04:09:24.307119Z","finished_at":"2026-10-19T04:09:31.446876Z","response_sha256":"b062fefc04e80e870552c2e7fe0a1fbaaa049d952a76e7f27254b265019b7c8d","pages":1,"reported_total":59,"returned":59},
    {"name":"38856570439-artifacts","api_path":"repos/chebizarro/nostrc/actions/runs/38856570439/artifacts?per_page=100","started_at":"2026-10-19T04:09:31.447492Z","finished_at":"2026-10-19T04:09:31.862943Z","response_sha256":"3dde8db5aa493bc71b3d7cce9ed42fea2398a2e71316ee8c7a74b7c030621469","pages":1,"reported_total":29,"returned":29},
    {"name":"38873978927-attempt1","api_path":"repos/chebizarro/nostrc/actions/runs/38873978927/attempts/1","started_at":"2026-10-19T04:09:31.863884Z","finished_at":"2026-10-19T04:09:32.275616Z","response_sha256":"b1dccf4298dcdbcc211a83d5d2f9c648f73e85ac6ec5ee28a03996d7769d78d2"},
    {"name":"38873978927-attempt1-jobs","api_path":"repos/chebizarro/nostrc/actions/runs/38873978927/attempts/1/jobs?per_page=100","started_at":"2026-10-19T04:09:32.276429Z","finished_at":"2026-10-19T04:09:38.573923Z","response_sha256":"4be0e5b0f18d878677919b450062643a906443dca293b07d332883f056c258a8","pages":1,"reported_total":59,"returned":59},
    {"name":"38873978927-artifacts","api_path":"repos/chebizarro/nostrc/actions/runs/38873978927/artifacts?per_page=100","started_at":"2026-10-19T04:09:38.574360Z","finished_at":"2026-10-19T04:09:39.013703Z","response_sha256":"df7c39ade9f214e42d830c2fcf9a441a4ee49a222e7a17fe39c4e71a1bc1bc28","pages":1,"reported_total":33,"returned":33},
    {"name":"38878691465-attempt1","api_path":"repos/chebizarro/nostrc/actions/runs/38878691465/attempts/1","started_at":"2026-10-19T04:09:39.014486Z","finished_at":"2026-10-19T04:09:39.447709Z","response_sha256":"3b6a1f0d8393219eb9ad351f1daa5d28824101f4406832eb0b3c05a337f42e14"},
    {"name":"38878691465-attempt1-jobs","api_path":"repos/chebizarro/nostrc/actions/runs/38878691465/attempts/1/jobs?per_page=100","started_at":"2026-10-19T04:09:39.448656Z","finished_at":"2026-10-19T04:09:45.423190Z","response_sha256":"83e935791f34378de2c3f90225e2b6125e00f3e394642cf46acb3d2caba9e5cb","pages":1,"reported_total":59,"returned":59},
    {"name":"38878691465-artifacts","api_path":"repos/chebizarro/nostrc/actions/runs/38878691465/artifacts?per_page=100","started_at":"2026-10-19T04:09:45.423951Z","finished_at":"2026-10-19T04:09:45.798011Z","response_sha256":"6ebb4b5f438538c48552876f6df757338fd6dfd339da7546ba59600c166e1a21","pages":1,"reported_total":34,"returned":34},
    {"name":"39132650260-attempt1","api_path":"repos/chebizarro/nostrc/actions/runs/39132650260/attempts/1","started_at":"2026-10-19T04:09:45.798325Z","finished_at":"2026-10-19T04:09:46.208593Z","response_sha256":"fbc83197e0b642b1d8e766170c969be13054aeab81775a763ce924b6243508e3"},
    {"name":"39132650260-attempt1-jobs","api_path":"repos/chebizarro/nostrc/actions/runs/39132650260/attempts/1/jobs?per_page=100","started_at":"2026-10-19T04:09:46.209590Z","finished_at":"2026-10-19T04:09:52.479726Z","response_sha256":"fc11496c942e6ded99dc0c7ba9d8da2da142554299c4948fd207253fbafc93c1","pages":1,"reported_total":61,"returned":61},
    {"name":"39132650260-artifacts","api_path":"repos/chebizarro/nostrc/actions/runs/39132650260/artifacts?per_page=100","started_at":"2026-10-19T04:09:52.480165Z","finished_at":"2026-10-19T04:09:52.861814Z","response_sha256":"d18c33dcde213fe2dc71544f50e1f27ff789e378cdfdc5c5bfffa10339edf77e","pages":1,"reported_total":46,"returned":46},
    {"name":"39194059430-attempt1","api_path":"repos/chebizarro/nostrc/actions/runs/39194059430/attempts/1","started_at":"2026-10-19T04:09:52.862167Z","finished_at":"2026-10-19T04:09:53.251266Z","response_sha256":"ac2f7a02cc4068f4eefd8ed6aa3ad915de1ef9fed676abe227353ec9aef3acff"},
    {"name":"39194059430-attempt1-jobs","api_path":"repos/chebizarro/nostrc/actions/runs/39194059430/attempts/1/jobs?per_page=100","started_at":"2026-10-19T04:09:53.251537Z","finished_at":"2026-10-19T04:09:55.337808Z","response_sha256":"5786cac76da35535bf2f585c06bcbaf288058619fe989bf6fb130dbd0bec38d6","pages":1,"reported_total":61,"returned":61},
    {"name":"39194059430-artifacts","api_path":"repos/chebizarro/nostrc/actions/runs/39194059430/artifacts?per_page=100","started_at":"2026-10-19T04:09:55.338754Z","finished_at":"2026-10-19T04:09:55.697682Z","response_sha256":"45fe1453f9a9cfd230ba3ffdbfc177a456784bb0056c6cfd78edca4aab996640","pages":1,"reported_total":47,"returned":47},
    {"name":"39194781902-attempt1","api_path":"repos/chebizarro/nostrc/actions/runs/39194781902/attempts/1","started_at":"2026-10-19T04:09:55.698099Z","finished_at":"2026-10-19T04:09:56.113292Z","response_sha256":"0575e5b206bbff2e0e4274a8f22b362e02dff509256c1fd0dc22604f844756b7"},
    {"name":"39194781902-attempt1-jobs","api_path":"repos/chebizarro/nostrc/actions/runs/39194781902/attempts/1/jobs?per_page=100","started_at":"2026-10-19T04:09:56.113784Z","finished_at":"2026-10-19T04:09:58.236689Z","response_sha256":"59ad49e79326223c99283223ad76bc912546a323edcd0267f154bd1901fc7c5a","pages":1,"reported_total":61,"returned":61},
    {"name":"39194781902-artifacts","api_path":"repos/chebizarro/nostrc/actions/runs/39194781902/artifacts?per_page=100","started_at":"2026-10-19T04:09:58.237569Z","finished_at":"2026-10-19T04:09:58.608605Z","response_sha256":"5c687f8d8d9b7e56cdf6e583b47e97ec0764b72b43f879efdc21222c304acc89","pages":1,"reported_total":42,"returned":42},
    {"name":"39196963896-attempt1","api_path":"repos/chebizarro/nostrc/actions/runs/39196963896/attempts/1","started_at":"2026-10-19T04:09:58.609442Z","finished_at":"2026-10-19T04:09:59.001776Z","response_sha256":"9b5796e40eb1c552f2f41aa695e33568a923f04fc59fb082aec7eb7359b299ac"},
    {"name":"39196963896-attempt1-jobs","api_path":"repos/chebizarro/nostrc/actions/runs/39196963896/attempts/1/jobs?per_page=100","started_at":"2026-10-19T04:09:59.002725Z","finished_at":"2026-10-19T04:10:01.137078Z","response_sha256":"e01c5ae557fb86765c24182375b4ddf234467cf95e7b66eb76eee0014a2fe35e","pages":1,"reported_total":58,"returned":58},
    {"name":"39196963896-artifacts","api_path":"repos/chebizarro/nostrc/actions/runs/39196963896/artifacts?per_page=100","started_at":"2026-10-19T04:10:01.137827Z","finished_at":"2026-10-19T04:10:01.511833Z","response_sha256":"542595e55818aa75a945ad457f8f365059bc550a8e1186bc370006c767128b60","pages":1,"reported_total":37,"returned":37}
  ],
  "runs": [
    {"id":38833746063,"run_attempt":1,"head_sha":"5467c02d11f74efc1a85c833024efb654bbf98b5","event":"push","head_branch":"master","status":"completed","conclusion":"failure","created_at":"2026-10-16T01:11:15Z","updated_at":"2026-10-16T01:56:42Z","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38833746063","pull_request_numbers":[],"qualification":{"pr_comparison":"ineligible_event","accepted_pr_numbers":[],"broad_integration":"failed","accepted_broad":false,"reason":"No PR event or reviewed legacy/selective pair. Broad required union not accepted: failed."},"job_count":56,"job_conclusion_or_status_counts":{"success":37,"failure":18,"skipped":1},"broad":{"nested_job_count":47,"conclusion_or_status_counts":{"success":36,"failure":10,"skipped":1},"accounting_job":{"id":116770701220,"name":"broad / Broad family accounting","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116770701220"},"failed_jobs":[{"id":116758467389,"name":"broad / bp_extended / nightly-matrix","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758467389"},{"id":116758467415,"name":"broad / homed / build-and-test (debian-bookworm, debian:bookworm, apt-get update, apt-get install -y build-essent...","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758467415"},{"id":116758467440,"name":"broad / homed / build-and-test (fedora-40, fedora:40, dnf -y update, dnf -y install @development-tools cmake ninj...","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758467440"},{"id":116758467468,"name":"broad / homed / build-and-test (ubuntu-24.04, ubuntu:24.04, apt-get update, apt-get install -y build-essential cm...","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758467468"},{"id":116758467475,"name":"broad / homed / build-and-test (archlinux, archlinux:latest, pacman -Syu --noconfirm, pacman -S --noconfirm base-...","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758467475"},{"id":116758467498,"name":"broad / bp / bp-matrix","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758467498"},{"id":116758736579,"name":"broad / fuzz / Fuzz json-event","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758736579"},{"id":116758736596,"name":"broad / fuzz / Fuzz nip49","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758736596"},{"id":116758736663,"name":"broad / fuzz / Fuzz bip39","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758736663"},{"id":116770701220,"name":"broad / Broad family accounting","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116770701220"}],"other_non_success_jobs":[{"id":116760531207,"name":"broad / fuzz / Generate coverage report","status":"completed","conclusion":"skipped","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116760531207"}]},"gate_job":{"id":116770728893,"name":"Dependency gate","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116770728893"},"artifact_api_total":24,"small_artifact_ids":[12020390181,12018835234,12018750668,12018681047,12018632028,12018551806,12018358112,12017873407,12017618868],"plans":[{"artifact_id":12018358112,"plan_id":"13177f5cf5ac03e21ad9c92f4c0508112c9739a82aa39fca0a9cee9cc4071d39","inventory_hash":"acab1855f888236d1681f3b217a75a6bf89ba43a00dd157fef2e3b0bed783c9f","base":null,"head":"5467c02d11f74efc1a85c833024efb654bbf98b5","tested_sha":"5467c02d11f74efc1a85c833024efb654bbf98b5","source_identity":{"kind":"committed","sha":"5467c02d11f74efc1a85c833024efb654bbf98b5"},"context":"ci","effective_mode":"broad","partial_profiles":[],"expected_lane_ids":["core.linux","grotto.linux","groundhog.linux","groundhog.mdk08","libgo.linux.asan","libgo.macos.asan","static.policy"]}],"aggregates":[{"artifact_id":12020390181,"plan_id":"13177f5cf5ac03e21ad9c92f4c0508112c9739a82aa39fca0a9cee9cc4071d39","tested_sha":"5467c02d11f74efc1a85c833024efb654bbf98b5","passed":false,"complete":true,"complete_gate_eligible":null,"error_count":18,"first_errors":["selected worker job conclusions missing/skipped/failed/cancelled","duplicate/unexpected lane: None"]}],"workflow_source":{"dependency-tests.yml":{"sha256":"d5b47588bd665c9dd14f2074ebfa071831e16a6cefb39743668eed070cd80be0","source_url":"https://github.com/chebizarro/nostrc/blob/5467c02d11f74efc1a85c833024efb654bbf98b5/.github/workflows/dependency-tests.yml","artifact_attempt_lines":[{"line":61,"text":"name: dependency-plan-${{ steps.plan.outputs.plan_id }}-${{ github.run_attempt }}"},{"line":132,"text":"validation_artifact: dependency-validation-${{ needs.select.outputs.plan_id }}-${{ github.run_attempt }}"},{"line":152,"text":"name: dependency-plan-${{ needs.select.outputs.plan_id }}-${{ github.run_attempt }}"},{"line":157,"text":"pattern: dependency-receipt-${{ needs.select.outputs.plan_id }}-*-${{ github.run_attempt }}"},{"line":170,"text":"name: dependency-validation-${{ needs.select.outputs.plan_id }}-${{ github.run_attempt }}"}]},"dependency-broad.yml":{"sha256":"79d5ce5af5f517d037b83fc5d4c92d3d31172a5ef059202e0a4b8ffe00d383b6","source_url":"https://github.com/chebizarro/nostrc/blob/5467c02d11f74efc1a85c833024efb654bbf98b5/.github/workflows/dependency-broad.yml","artifact_attempt_lines":[{"line":98,"text":"name: Broad family accounting"}]},"dependency-worker.yml":{"sha256":"c2fa589d11dbb83a43a9c69a3c42a29f46186978590fe7854e0555ff88dce01d","source_url":"https://github.com/chebizarro/nostrc/blob/5467c02d11f74efc1a85c833024efb654bbf98b5/.github/workflows/dependency-worker.yml","artifact_attempt_lines":[{"line":35,"text":"name: dependency-plan-${{ inputs.plan_id }}-${{ github.run_attempt }}"},{"line":85,"text":"name: dependency-receipt-${{ inputs.plan_id }}-${{ inputs.profile }}-${{ github.run_attempt }}"},{"line":92,"text":"name: dependency-evidence-${{ inputs.plan_id }}-${{ inputs.profile }}-${{ github.run_attempt }}"}]}},"historical_detail_reference":"Historical canonical data below: same run/attempt, lane details, member hashes and dispositions retained unchanged."},
    {"id":38845886339,"run_attempt":1,"head_sha":"33a1012767c9db5a46057280178134a544b86af7","event":"push","head_branch":"master","status":"completed","conclusion":"failure","created_at":"2026-10-16T03:30:47Z","updated_at":"2026-10-16T03:32:15Z","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38845886339","pull_request_numbers":[],"qualification":{"pr_comparison":"ineligible_event","accepted_pr_numbers":[],"broad_integration":"not_launched","accepted_broad":false,"reason":"No PR event or reviewed legacy/selective pair. Broad required union not accepted: not_launched."},"job_count":10,"job_conclusion_or_status_counts":{"failure":2,"skipped":8},"broad":{"nested_job_count":0,"conclusion_or_status_counts":{},"accounting_job":null,"failed_jobs":[],"other_non_success_jobs":[]},"gate_job":{"id":116796544583,"name":"Dependency gate","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38845886339/job/116796544583"},"artifact_api_total":0,"small_artifact_ids":[],"plans":[],"aggregates":[],"workflow_source":{"dependency-tests.yml":{"sha256":"d5b47588bd665c9dd14f2074ebfa071831e16a6cefb39743668eed070cd80be0","source_url":"https://github.com/chebizarro/nostrc/blob/33a1012767c9db5a46057280178134a544b86af7/.github/workflows/dependency-tests.yml","artifact_attempt_lines":[{"line":61,"text":"name: dependency-plan-${{ steps.plan.outputs.plan_id }}-${{ github.run_attempt }}"},{"line":132,"text":"validation_artifact: dependency-validation-${{ needs.select.outputs.plan_id }}-${{ github.run_attempt }}"},{"line":152,"text":"name: dependency-plan-${{ needs.select.outputs.plan_id }}-${{ github.run_attempt }}"},{"line":157,"text":"pattern: dependency-receipt-${{ needs.select.outputs.plan_id }}-*-${{ github.run_attempt }}"},{"line":170,"text":"name: dependency-validation-${{ needs.select.outputs.plan_id }}-${{ github.run_attempt }}"}]},"dependency-broad.yml":{"sha256":"79d5ce5af5f517d037b83fc5d4c92d3d31172a5ef059202e0a4b8ffe00d383b6","source_url":"https://github.com/chebizarro/nostrc/blob/33a1012767c9db5a46057280178134a544b86af7/.github/workflows/dependency-broad.yml","artifact_attempt_lines":[{"line":98,"text":"name: Broad family accounting"}]},"dependency-worker.yml":{"sha256":"c2fa589d11dbb83a43a9c69a3c42a29f46186978590fe7854e0555ff88dce01d","source_url":"https://github.com/chebizarro/nostrc/blob/33a1012767c9db5a46057280178134a544b86af7/.github/workflows/dependency-worker.yml","artifact_attempt_lines":[{"line":35,"text":"name: dependency-plan-${{ inputs.plan_id }}-${{ github.run_attempt }}"},{"line":85,"text":"name: dependency-receipt-${{ inputs.plan_id }}-${{ inputs.profile }}-${{ github.run_attempt }}"},{"line":92,"text":"name: dependency-evidence-${{ inputs.plan_id }}-${{ inputs.profile }}-${{ github.run_attempt }}"}]}},"historical_detail_reference":"Historical canonical data below: same run/attempt, lane details, member hashes and dispositions retained unchanged."},
    {"id":38856570439,"run_attempt":1,"head_sha":"8a031f443ac84850345a4cf0fb6ab5a8c3b32814","event":"push","head_branch":"master","status":"completed","conclusion":"failure","created_at":"2026-10-16T05:35:29Z","updated_at":"2026-10-16T06:18:19Z","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38856570439","pull_request_numbers":[],"qualification":{"pr_comparison":"ineligible_event","accepted_pr_numbers":[],"broad_integration":"failed","accepted_broad":false,"reason":"No PR event or reviewed legacy/selective pair. Broad required union not accepted: failed."},"job_count":59,"job_conclusion_or_status_counts":{"success":39,"failure":19,"skipped":1},"broad":{"nested_job_count":47,"conclusion_or_status_counts":{"success":36,"failure":10,"skipped":1},"accounting_job":{"id":116841163905,"name":"broad / Broad family accounting","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116841163905"},"failed_jobs":[{"id":116829330879,"name":"broad / homed / build-and-test (fedora-40, fedora:40, dnf -y update, dnf -y install @development-tools cmake ninj...","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330879"},{"id":116829330888,"name":"broad / bp_extended / nightly-matrix","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330888"},{"id":116829330914,"name":"broad / homed / build-and-test (debian-bookworm, debian:bookworm, apt-get update, apt-get install -y build-essent...","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330914"},{"id":116829330915,"name":"broad / homed / build-and-test (ubuntu-24.04, ubuntu:24.04, apt-get update, apt-get install -y build-essential cm...","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330915"},{"id":116829330967,"name":"broad / homed / build-and-test (archlinux, archlinux:latest, pacman -Syu --noconfirm, pacman -S --noconfirm base-...","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330967"},{"id":116829331088,"name":"broad / bp / bp-matrix","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829331088"},{"id":116831017659,"name":"broad / fuzz / Fuzz bip39","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116831017659"},{"id":116831017669,"name":"broad / fuzz / Fuzz json-event","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116831017669"},{"id":116831017860,"name":"broad / fuzz / Fuzz nip49","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116831017860"},{"id":116841163905,"name":"broad / Broad family accounting","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116841163905"}],"other_non_success_jobs":[{"id":116831289438,"name":"broad / fuzz / Generate coverage report","status":"completed","conclusion":"skipped","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116831289438"}]},"gate_job":{"id":116841190994,"name":"Dependency gate","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116841190994"},"artifact_api_total":29,"small_artifact_ids":[12028398157,12027925197,12027870329,12027860350,12027855083,12027775498,12027745788,12027457503,12027452461,12027323348,12027248137,12026609380],"plans":[{"artifact_id":12027855083,"plan_id":"3b0264bf4f094946e192da6f7db6340e25e16cc6af97084c0253017426e321df","inventory_hash":"00045270c2a511ce96c674fe4a835842ed27719fa3cbcf6a4dc7a3cbd70fca5d","base":null,"head":"8a031f443ac84850345a4cf0fb6ab5a8c3b32814","tested_sha":"8a031f443ac84850345a4cf0fb6ab5a8c3b32814","source_identity":{"kind":"committed","sha":"8a031f443ac84850345a4cf0fb6ab5a8c3b32814"},"context":"ci","effective_mode":"broad","partial_profiles":[],"expected_lane_ids":["core.linux","core.linux.asan","core.macos.asan","grotto.linux","groundhog.linux","groundhog.linux.sanitizers","groundhog.mdk08","libgo.linux.asan","libgo.macos.asan","static.policy"]}],"aggregates":[{"artifact_id":12028398157,"plan_id":"3b0264bf4f094946e192da6f7db6340e25e16cc6af97084c0253017426e321df","tested_sha":"8a031f443ac84850345a4cf0fb6ab5a8c3b32814","passed":false,"complete":true,"complete_gate_eligible":false,"error_count":25,"first_errors":["inventory coverage/enforcement readiness cannot satisfy complete gate","selected worker job conclusions missing/skipped/failed/cancelled"]}],"workflow_source":{"dependency-tests.yml":{"sha256":"215dd788507eb2fff04dd3ed70193126006ec642bf949b3689fc8b1415e486e2","source_url":"https://github.com/chebizarro/nostrc/blob/8a031f443ac84850345a4cf0fb6ab5a8c3b32814/.github/workflows/dependency-tests.yml","artifact_attempt_lines":[{"line":61,"text":"name: dependency-plan-${{ steps.plan.outputs.plan_id }}-${{ github.run_attempt }}"},{"line":156,"text":"validation_artifact: dependency-validation-${{ needs.select.outputs.plan_id }}-${{ github.run_attempt }}"},{"line":176,"text":"name: dependency-plan-${{ needs.select.outputs.plan_id }}-${{ github.run_attempt }}"},{"line":181,"text":"pattern: dependency-receipt-${{ needs.select.outputs.plan_id }}-*-${{ github.run_attempt }}"},{"line":194,"text":"name: dependency-validation-${{ needs.select.outputs.plan_id }}-${{ github.run_attempt }}"}]},"dependency-broad.yml":{"sha256":"79d5ce5af5f517d037b83fc5d4c92d3d31172a5ef059202e0a4b8ffe00d383b6","source_url":"https://github.com/chebizarro/nostrc/blob/8a031f443ac84850345a4cf0fb6ab5a8c3b32814/.github/workflows/dependency-broad.yml","artifact_attempt_lines":[{"line":98,"text":"name: Broad family accounting"}]},"dependency-worker.yml":{"sha256":"71a0e5194529c416862a4ba493ccabcf559d56eb5bcf1bad20850a973b54c086","source_url":"https://github.com/chebizarro/nostrc/blob/8a031f443ac84850345a4cf0fb6ab5a8c3b32814/.github/workflows/dependency-worker.yml","artifact_attempt_lines":[{"line":35,"text":"name: dependency-plan-${{ inputs.plan_id }}-${{ github.run_attempt }}"},{"line":92,"text":"name: dependency-receipt-${{ inputs.plan_id }}-${{ inputs.profile }}-${{ github.run_attempt }}"},{"line":99,"text":"name: dependency-evidence-${{ inputs.plan_id }}-${{ inputs.profile }}-${{ github.run_attempt }}"}]}},"historical_detail_reference":"Historical canonical data below: same run/attempt, lane details, member hashes and dispositions retained unchanged."},
    {"id":38873978927,"run_attempt":1,"head_sha":"a478070509569c26917dd5fb13a548b1f68eade9","event":"workflow_dispatch","head_branch":"ci/testing-final-integration-20261016","status":"completed","conclusion":"cancelled","created_at":"2026-10-16T08:29:01Z","updated_at":"2026-10-16T09:19:49Z","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38873978927","pull_request_numbers":[],"qualification":{"pr_comparison":"ineligible_event","accepted_pr_numbers":[],"broad_integration":"failed","accepted_broad":false,"reason":"No PR event or reviewed legacy/selective pair. Broad required union not accepted: failed."},"job_count":59,"job_conclusion_or_status_counts":{"success":45,"failure":13,"skipped":1},"broad":{"nested_job_count":47,"conclusion_or_status_counts":{"success":41,"failure":5,"skipped":1},"accounting_job":{"id":116900093654,"name":"broad / Broad family accounting","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116900093654"},"failed_jobs":[{"id":116884596656,"name":"broad / groundhog / groundhog-sanitizers","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116884596656"},{"id":116884597017,"name":"broad / bp / bp-matrix","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116884597017"},{"id":116884597175,"name":"broad / bp_extended / nightly-matrix","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116884597175"},{"id":116885248265,"name":"broad / fuzz / Fuzz json-event","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116885248265"},{"id":116900093654,"name":"broad / Broad family accounting","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116900093654"}],"other_non_success_jobs":[{"id":116889115593,"name":"broad / fuzz / Generate coverage report","status":"completed","conclusion":"skipped","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116889115593"}]},"gate_job":{"id":116900256052,"name":"Dependency gate","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116900256052"},"artifact_api_total":33,"small_artifact_ids":[12036857589,12035130067,12034842125,12034791870,12034733294,12034472127,12034417836,12034264843,12034124206,12033953011,12033829723,12033788282],"plans":[{"artifact_id":12034472127,"plan_id":"3aba1a4e60bdf008fe6079e12d9baeaa67bd06f29517b4d319ee0a6e57112744","inventory_hash":"6b272061d8c62dec6cc67813576d3d5f0c6923a859fba672971c66a213e0edbc","base":null,"head":"a478070509569c26917dd5fb13a548b1f68eade9","tested_sha":"a478070509569c26917dd5fb13a548b1f68eade9","source_identity":{"kind":"committed","sha":"a478070509569c26917dd5fb13a548b1f68eade9"},"context":"ci","effective_mode":"broad","partial_profiles":[],"expected_lane_ids":["core.linux","core.linux.asan","core.macos.asan","grotto.linux","groundhog.linux","groundhog.linux.sanitizers","groundhog.mdk08","libgo.linux.asan","libgo.macos.asan","static.policy"]}],"aggregates":[{"artifact_id":12036857589,"plan_id":"3aba1a4e60bdf008fe6079e12d9baeaa67bd06f29517b4d319ee0a6e57112744","tested_sha":"a478070509569c26917dd5fb13a548b1f68eade9","passed":false,"complete":true,"complete_gate_eligible":false,"error_count":24,"first_errors":["inventory coverage/enforcement readiness cannot satisfy complete gate","selected worker job conclusions missing/skipped/failed/cancelled"]}],"workflow_source":{"dependency-tests.yml":{"sha256":"215dd788507eb2fff04dd3ed70193126006ec642bf949b3689fc8b1415e486e2","source_url":"https://github.com/chebizarro/nostrc/blob/a478070509569c26917dd5fb13a548b1f68eade9/.github/workflows/dependency-tests.yml","artifact_attempt_lines":[{"line":61,"text":"name: dependency-plan-${{ steps.plan.outputs.plan_id }}-${{ github.run_attempt }}"},{"line":156,"text":"validation_artifact: dependency-validation-${{ needs.select.outputs.plan_id }}-${{ github.run_attempt }}"},{"line":176,"text":"name: dependency-plan-${{ needs.select.outputs.plan_id }}-${{ github.run_attempt }}"},{"line":181,"text":"pattern: dependency-receipt-${{ needs.select.outputs.plan_id }}-*-${{ github.run_attempt }}"},{"line":194,"text":"name: dependency-validation-${{ needs.select.outputs.plan_id }}-${{ github.run_attempt }}"}]},"dependency-broad.yml":{"sha256":"79d5ce5af5f517d037b83fc5d4c92d3d31172a5ef059202e0a4b8ffe00d383b6","source_url":"https://github.com/chebizarro/nostrc/blob/a478070509569c26917dd5fb13a548b1f68eade9/.github/workflows/dependency-broad.yml","artifact_attempt_lines":[{"line":98,"text":"name: Broad family accounting"}]},"dependency-worker.yml":{"sha256":"1cc2f197566eadcd1bd79c1e5e7f6dad3c4de76d89076e9c2dcaa002a1d38cd6","source_url":"https://github.com/chebizarro/nostrc/blob/a478070509569c26917dd5fb13a548b1f68eade9/.github/workflows/dependency-worker.yml","artifact_attempt_lines":[{"line":35,"text":"name: dependency-plan-${{ inputs.plan_id }}-${{ github.run_attempt }}"},{"line":123,"text":"name: dependency-receipt-${{ inputs.plan_id }}-${{ inputs.profile }}-${{ github.run_attempt }}"},{"line":130,"text":"name: dependency-evidence-${{ inputs.plan_id }}-${{ inputs.profile }}-${{ github.run_attempt }}"}]}},"historical_detail_reference":"Historical canonical data below: same run/attempt, lane details, member hashes and dispositions retained unchanged."},
    {"id":38878691465,"run_attempt":1,"head_sha":"249f9657fb230947ba486fb3cd92894316f9acc3","event":"push","head_branch":"master","status":"completed","conclusion":"failure","created_at":"2026-10-16T09:10:58Z","updated_at":"2026-10-16T09:53:52Z","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38878691465","pull_request_numbers":[],"qualification":{"pr_comparison":"ineligible_event","accepted_pr_numbers":[],"broad_integration":"failed","accepted_broad":false,"reason":"No PR event or reviewed legacy/selective pair. Broad required union not accepted: failed."},"job_count":59,"job_conclusion_or_status_counts":{"success":45,"failure":13,"skipped":1},"broad":{"nested_job_count":47,"conclusion_or_status_counts":{"success":41,"failure":5,"skipped":1},"accounting_job":{"id":116915712972,"name":"broad / Broad family accounting","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116915712972"},"failed_jobs":[{"id":116899992180,"name":"broad / bp_extended / nightly-matrix","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116899992180"},{"id":116899992250,"name":"broad / bp / bp-matrix","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116899992250"},{"id":116899992256,"name":"broad / groundhog / groundhog-sanitizers","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116899992256"},{"id":116902162597,"name":"broad / fuzz / Fuzz json-event","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116902162597"},{"id":116915712972,"name":"broad / Broad family accounting","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116915712972"}],"other_non_success_jobs":[{"id":116905942425,"name":"broad / fuzz / Generate coverage report","status":"completed","conclusion":"skipped","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116905942425"}]},"gate_job":{"id":116915750038,"name":"Dependency gate","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116915750038"},"artifact_api_total":34,"small_artifact_ids":[12038423327,12037146583,12037010902,12036853940,12036847585,12036762746,12036653196,12036638304,12036446528,12036349109,12036264453,12035779772],"plans":[{"artifact_id":12036446528,"plan_id":"ef5fe08b9b15da727973302b5e13b6c79376062eba33b579d6d0b81da195425c","inventory_hash":"e074ff04674faa90f3335b9a5177a5c236ebf5d74f5e8ab81cf4e8943c73a95a","base":null,"head":"249f9657fb230947ba486fb3cd92894316f9acc3","tested_sha":"249f9657fb230947ba486fb3cd92894316f9acc3","source_identity":{"kind":"committed","sha":"249f9657fb230947ba486fb3cd92894316f9acc3"},"context":"ci","effective_mode":"broad","partial_profiles":[],"expected_lane_ids":["core.linux","core.linux.asan","core.macos.asan","grotto.linux","groundhog.linux","groundhog.linux.sanitizers","groundhog.mdk08","libgo.linux.asan","libgo.macos.asan","static.policy"]}],"aggregates":[{"artifact_id":12038423327,"plan_id":"ef5fe08b9b15da727973302b5e13b6c79376062eba33b579d6d0b81da195425c","tested_sha":"249f9657fb230947ba486fb3cd92894316f9acc3","passed":false,"complete":true,"complete_gate_eligible":false,"error_count":23,"first_errors":["inventory coverage/enforcement readiness cannot satisfy complete gate","selected worker job conclusions missing/skipped/failed/cancelled"]}],"workflow_source":{"dependency-tests.yml":{"sha256":"215dd788507eb2fff04dd3ed70193126006ec642bf949b3689fc8b1415e486e2","source_url":"https://github.com/chebizarro/nostrc/blob/249f9657fb230947ba486fb3cd92894316f9acc3/.github/workflows/dependency-tests.yml","artifact_attempt_lines":[{"line":61,"text":"name: dependency-plan-${{ steps.plan.outputs.plan_id }}-${{ github.run_attempt }}"},{"line":156,"text":"validation_artifact: dependency-validation-${{ needs.select.outputs.plan_id }}-${{ github.run_attempt }}"},{"line":176,"text":"name: dependency-plan-${{ needs.select.outputs.plan_id }}-${{ github.run_attempt }}"},{"line":181,"text":"pattern: dependency-receipt-${{ needs.select.outputs.plan_id }}-*-${{ github.run_attempt }}"},{"line":194,"text":"name: dependency-validation-${{ needs.select.outputs.plan_id }}-${{ github.run_attempt }}"}]},"dependency-broad.yml":{"sha256":"79d5ce5af5f517d037b83fc5d4c92d3d31172a5ef059202e0a4b8ffe00d383b6","source_url":"https://github.com/chebizarro/nostrc/blob/249f9657fb230947ba486fb3cd92894316f9acc3/.github/workflows/dependency-broad.yml","artifact_attempt_lines":[{"line":98,"text":"name: Broad family accounting"}]},"dependency-worker.yml":{"sha256":"1cc2f197566eadcd1bd79c1e5e7f6dad3c4de76d89076e9c2dcaa002a1d38cd6","source_url":"https://github.com/chebizarro/nostrc/blob/249f9657fb230947ba486fb3cd92894316f9acc3/.github/workflows/dependency-worker.yml","artifact_attempt_lines":[{"line":35,"text":"name: dependency-plan-${{ inputs.plan_id }}-${{ github.run_attempt }}"},{"line":123,"text":"name: dependency-receipt-${{ inputs.plan_id }}-${{ inputs.profile }}-${{ github.run_attempt }}"},{"line":130,"text":"name: dependency-evidence-${{ inputs.plan_id }}-${{ inputs.profile }}-${{ github.run_attempt }}"}]}},"historical_detail_reference":"Historical canonical data below: same run/attempt, lane details, member hashes and dispositions retained unchanged."},
    {"id":39132650260,"run_attempt":1,"head_sha":"2743f17354ac13527d8d8919f81abc711396c959","event":"push","head_branch":"master","status":"completed","conclusion":"failure","created_at":"2026-10-18T13:41:27Z","updated_at":"2026-10-18T14:24:47Z","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39132650260","pull_request_numbers":[],"qualification":{"pr_comparison":"ineligible_event","accepted_pr_numbers":[],"broad_integration":"failed","accepted_broad":false,"reason":"No PR event or reviewed legacy/selective pair. Broad required union not accepted: failed."},"job_count":61,"job_conclusion_or_status_counts":{"success":48,"failure":12,"skipped":1},"broad":{"nested_job_count":47,"conclusion_or_status_counts":{"success":44,"failure":2,"skipped":1},"accounting_job":{"id":117700034296,"name":"broad / Broad family accounting","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117700034296"},"failed_jobs":[{"id":117691683008,"name":"broad / fuzz / Fuzz json-event","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117691683008"},{"id":117700034296,"name":"broad / Broad family accounting","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117700034296"}],"other_non_success_jobs":[{"id":117694708765,"name":"broad / fuzz / Generate coverage report","status":"completed","conclusion":"skipped","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117694708765"}]},"gate_job":{"id":117700054727,"name":"Dependency gate","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117700054727"},"artifact_api_total":46,"small_artifact_ids":[12140195214,12139515154,12139196110,12139046572,12138776972,12138657091,12138638626,12138633221,12138577637,12138467933,12138424589,12138414686,12138179654,12138098321],"plans":[{"artifact_id":12138098321,"plan_id":"3590a9cf34ef97302aaf6d04f2f65efbe66b884a0e5092f4f8ad606a7234485c","inventory_hash":"c2ef7ee5281253cae2df2da3fb85a2ef83b8d4f7bb21c1ce20abf8d986bcd9ab","base":null,"head":"2743f17354ac13527d8d8919f81abc711396c959","tested_sha":"2743f17354ac13527d8d8919f81abc711396c959","source_identity":{"kind":"committed","sha":"2743f17354ac13527d8d8919f81abc711396c959"},"context":"ci","effective_mode":"broad","partial_profiles":[],"expected_lane_ids":["core.linux","core.linux.asan","core.macos.asan","grotto.linux","grotto.linux.asan_ubsan","groundhog.linux","groundhog.linux.sanitizers","groundhog.mdk011","groundhog.mdk08","libgo.linux.asan","libgo.macos.asan","static.policy"]}],"aggregates":[{"artifact_id":12140195214,"plan_id":"3590a9cf34ef97302aaf6d04f2f65efbe66b884a0e5092f4f8ad606a7234485c","tested_sha":"2743f17354ac13527d8d8919f81abc711396c959","passed":false,"complete":true,"complete_gate_eligible":false,"error_count":467,"first_errors":["inventory coverage/enforcement readiness cannot satisfy complete gate","selected worker job conclusions missing/skipped/failed/cancelled"]}],"workflow_source":{"dependency-tests.yml":{"sha256":"a18010ceab7be2d96632ef8266c24fcb2e71e970dbdb4c09905ad17265f45f15","source_url":"https://github.com/chebizarro/nostrc/blob/2743f17354ac13527d8d8919f81abc711396c959/.github/workflows/dependency-tests.yml","artifact_attempt_lines":[{"line":61,"text":"name: dependency-plan-${{ steps.plan.outputs.plan_id }}-${{ github.run_attempt }}"},{"line":172,"text":"validation_artifact: dependency-validation-${{ needs.select.outputs.plan_id }}-${{ github.run_attempt }}"},{"line":192,"text":"name: dependency-plan-${{ needs.select.outputs.plan_id }}-${{ github.run_attempt }}"},{"line":197,"text":"pattern: dependency-receipt-${{ needs.select.outputs.plan_id }}-*-${{ github.run_attempt }}"},{"line":210,"text":"name: dependency-validation-${{ needs.select.outputs.plan_id }}-${{ github.run_attempt }}"}]},"dependency-broad.yml":{"sha256":"79d5ce5af5f517d037b83fc5d4c92d3d31172a5ef059202e0a4b8ffe00d383b6","source_url":"https://github.com/chebizarro/nostrc/blob/2743f17354ac13527d8d8919f81abc711396c959/.github/workflows/dependency-broad.yml","artifact_attempt_lines":[{"line":98,"text":"name: Broad family accounting"}]},"dependency-worker.yml":{"sha256":"db84c2c495a96ad0d76132bb788e54a4d0410d82d3da4e277e6d52408c719b12","source_url":"https://github.com/chebizarro/nostrc/blob/2743f17354ac13527d8d8919f81abc711396c959/.github/workflows/dependency-worker.yml","artifact_attempt_lines":[{"line":35,"text":"name: dependency-plan-${{ inputs.plan_id }}-${{ github.run_attempt }}"},{"line":167,"text":"name: dependency-receipt-${{ inputs.plan_id }}-${{ inputs.profile }}-${{ github.run_attempt }}"},{"line":174,"text":"name: dependency-evidence-${{ inputs.plan_id }}-${{ inputs.profile }}-${{ github.run_attempt }}"}]}},"historical_detail_reference":"Historical canonical data below: same run/attempt, lane details, member hashes and dispositions retained unchanged."},
    {"id":39194059430,"run_attempt":1,"head_sha":"5280ea324ec474e9d0644638565a2e5a01748b51","event":"push","head_branch":"master","status":"completed","conclusion":"failure","created_at":"2026-10-19T03:18:21Z","updated_at":"2026-10-19T04:04:23Z","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194059430","pull_request_numbers":[],"qualification":{"pr_comparison":"ineligible_event","accepted_pr_numbers":[],"broad_integration":"failed","accepted_broad":false,"reason":"No PR event or reviewed legacy/selective pair. Broad required union not accepted: failed."},"job_count":61,"job_conclusion_or_status_counts":{"success":47,"failure":13,"skipped":1},"broad":{"nested_job_count":47,"conclusion_or_status_counts":{"success":43,"failure":3,"skipped":1},"accounting_job":{"id":117882302404,"name":"broad / Broad family accounting","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194059430/job/117882302404"},"failed_jobs":[{"id":117872921493,"name":"broad / core / macos-latest • asan_ubsan","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194059430/job/117872921493"},{"id":117872963370,"name":"broad / fuzz / Fuzz json-event","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194059430/job/117872963370"},{"id":117882302404,"name":"broad / Broad family accounting","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194059430/job/117882302404"}],"other_non_success_jobs":[{"id":117875589692,"name":"broad / fuzz / Generate coverage report","status":"completed","conclusion":"skipped","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194059430/job/117875589692"}]},"gate_job":{"id":117883031219,"name":"Dependency gate","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194059430/job/117883031219"},"artifact_api_total":47,"small_artifact_ids":[12162288486,12161630431,12161580189,12161555809,12161446081,12161425599,12161297425,12161287274,12161151680,12160928529,12160903749,12160897422,12160774441,12160754395],"plans":[{"artifact_id":12160928529,"plan_id":"fccd676fa5ec6c3fbdb78fa3a889aed6dba0923d8ea00333cfa1cae35974ceb4","inventory_hash":"c2ef7ee5281253cae2df2da3fb85a2ef83b8d4f7bb21c1ce20abf8d986bcd9ab","base":null,"head":"5280ea324ec474e9d0644638565a2e5a01748b51","tested_sha":"5280ea324ec474e9d0644638565a2e5a01748b51","source_identity":{"kind":"committed","sha":"5280ea324ec474e9d0644638565a2e5a01748b51"},"context":"ci","effective_mode":"broad","partial_profiles":[],"expected_lane_ids":["core.linux","core.linux.asan","core.macos.asan","grotto.linux","grotto.linux.asan_ubsan","groundhog.linux","groundhog.linux.sanitizers","groundhog.mdk011","groundhog.mdk08","libgo.linux.asan","libgo.macos.asan","static.policy"]}],"aggregates":[{"artifact_id":12162288486,"plan_id":"fccd676fa5ec6c3fbdb78fa3a889aed6dba0923d8ea00333cfa1cae35974ceb4","tested_sha":"5280ea324ec474e9d0644638565a2e5a01748b51","passed":false,"complete":true,"complete_gate_eligible":false,"error_count":467,"first_errors":["inventory coverage/enforcement readiness cannot satisfy complete gate","selected worker job conclusions missing/skipped/failed/cancelled"]}],"workflow_source":{"dependency-tests.yml":{"sha256":"a18010ceab7be2d96632ef8266c24fcb2e71e970dbdb4c09905ad17265f45f15","source_url":"https://github.com/chebizarro/nostrc/blob/5280ea324ec474e9d0644638565a2e5a01748b51/.github/workflows/dependency-tests.yml","artifact_attempt_lines":[{"line":61,"text":"name: dependency-plan-${{ steps.plan.outputs.plan_id }}-${{ github.run_attempt }}"},{"line":172,"text":"validation_artifact: dependency-validation-${{ needs.select.outputs.plan_id }}-${{ github.run_attempt }}"},{"line":192,"text":"name: dependency-plan-${{ needs.select.outputs.plan_id }}-${{ github.run_attempt }}"},{"line":197,"text":"pattern: dependency-receipt-${{ needs.select.outputs.plan_id }}-*-${{ github.run_attempt }}"},{"line":210,"text":"name: dependency-validation-${{ needs.select.outputs.plan_id }}-${{ github.run_attempt }}"}]},"dependency-broad.yml":{"sha256":"79d5ce5af5f517d037b83fc5d4c92d3d31172a5ef059202e0a4b8ffe00d383b6","source_url":"https://github.com/chebizarro/nostrc/blob/5280ea324ec474e9d0644638565a2e5a01748b51/.github/workflows/dependency-broad.yml","artifact_attempt_lines":[{"line":98,"text":"name: Broad family accounting"}]},"dependency-worker.yml":{"sha256":"db84c2c495a96ad0d76132bb788e54a4d0410d82d3da4e277e6d52408c719b12","source_url":"https://github.com/chebizarro/nostrc/blob/5280ea324ec474e9d0644638565a2e5a01748b51/.github/workflows/dependency-worker.yml","artifact_attempt_lines":[{"line":35,"text":"name: dependency-plan-${{ inputs.plan_id }}-${{ github.run_attempt }}"},{"line":167,"text":"name: dependency-receipt-${{ inputs.plan_id }}-${{ inputs.profile }}-${{ github.run_attempt }}"},{"line":174,"text":"name: dependency-evidence-${{ inputs.plan_id }}-${{ inputs.profile }}-${{ github.run_attempt }}"}]}},"lane_receipts":[{"artifact_id":12161630431,"lane_id":"core.macos.asan","profile":"core.macos.asan","plan_id":"fccd676fa5ec6c3fbdb78fa3a889aed6dba0923d8ea00333cfa1cae35974ceb4","receipt_kind":"lane_failure_v1","job_conclusion":"failure","complete":false,"complete_gate_eligible":false,"registration_audit":"failed","input_audit":"failed","failure_stage":"preaudit","failure_errors":[{"message":"pre-build input audit failed; original findings retained","type":"CheckFailure"}],"input_audit_gap_counts":{"unclaimed_registration":3},"phase_configuration":"Debug","resolved_configuration":"Debug","observed_execution_count":0,"check_outcome_counts":{"missing":48}},{"artifact_id":12161580189,"lane_id":"grotto.linux.asan_ubsan","profile":"grotto.linux.asan_ubsan","plan_id":"fccd676fa5ec6c3fbdb78fa3a889aed6dba0923d8ea00333cfa1cae35974ceb4","receipt_kind":"lane_failure_v1","job_conclusion":"failure","complete":false,"complete_gate_eligible":false,"registration_audit":"failed","input_audit":"failed","failure_stage":"preaudit","failure_errors":[{"message":"pre-build input audit failed; original findings retained","type":"CheckFailure"}],"input_audit_gap_counts":{"unclaimed_registration":322,"target_mapping":1,"missing_impact_edge":330,"generated_runtime_without_target":2},"phase_configuration":"Debug","resolved_configuration":"Debug","observed_execution_count":0,"check_outcome_counts":{"missing":4}},{"artifact_id":12161555809,"lane_id":"core.linux","profile":"core.linux","plan_id":"fccd676fa5ec6c3fbdb78fa3a889aed6dba0923d8ea00333cfa1cae35974ceb4","receipt_kind":"lane_failure_v1","job_conclusion":"failure","complete":false,"complete_gate_eligible":false,"registration_audit":"not_performed","input_audit":"not_completed","failure_stage":"configure","failure_errors":[{"message":"profile build type Debug differs from realized ''","type":"RealizationError"}],"input_audit_gap_counts":{},"phase_configuration":null,"resolved_configuration":null,"observed_execution_count":0,"check_outcome_counts":{"missing":3}},{"artifact_id":12161446081,"lane_id":"libgo.linux.asan","profile":"libgo.linux.asan","plan_id":"fccd676fa5ec6c3fbdb78fa3a889aed6dba0923d8ea00333cfa1cae35974ceb4","receipt_kind":null,"job_conclusion":"success","complete":null,"complete_gate_eligible":false,"registration_audit":"passed","input_audit":"passed","failure_stage":null,"failure_errors":[],"input_audit_gap_counts":{},"phase_configuration":"Debug","resolved_configuration":null,"observed_execution_count":33,"check_outcome_counts":{"passed":2}},{"artifact_id":12161425599,"lane_id":"groundhog.linux","profile":"groundhog.linux","plan_id":"fccd676fa5ec6c3fbdb78fa3a889aed6dba0923d8ea00333cfa1cae35974ceb4","receipt_kind":"lane_failure_v1","job_conclusion":"failure","complete":false,"complete_gate_eligible":false,"registration_audit":"failed","input_audit":"failed","failure_stage":"preaudit","failure_errors":[{"message":"pre-build input audit failed; original findings retained","type":"CheckFailure"}],"input_audit_gap_counts":{"unclaimed_registration":408,"missing_impact_edge":9473,"check_descriptor_owner_disagreement":1,"target_mapping":1},"phase_configuration":"Debug","resolved_configuration":"Debug","observed_execution_count":0,"check_outcome_counts":{"missing":10}},{"artifact_id":12161297425,"lane_id":"static.policy","profile":"static.policy","plan_id":"fccd676fa5ec6c3fbdb78fa3a889aed6dba0923d8ea00333cfa1cae35974ceb4","receipt_kind":null,"job_conclusion":"success","complete":null,"complete_gate_eligible":false,"registration_audit":"passed","input_audit":"passed","failure_stage":null,"failure_errors":[],"input_audit_gap_counts":{},"phase_configuration":"","resolved_configuration":null,"observed_execution_count":1,"check_outcome_counts":{"passed":1}},{"artifact_id":12161287274,"lane_id":"grotto.linux","profile":"grotto.linux","plan_id":"fccd676fa5ec6c3fbdb78fa3a889aed6dba0923d8ea00333cfa1cae35974ceb4","receipt_kind":"lane_failure_v1","job_conclusion":"failure","complete":false,"complete_gate_eligible":false,"registration_audit":"failed","input_audit":"failed","failure_stage":"preaudit","failure_errors":[{"message":"pre-build input audit failed; original findings retained","type":"CheckFailure"}],"input_audit_gap_counts":{"unclaimed_registration":402,"missing_impact_edge":9104,"generated_runtime_without_target":2},"phase_configuration":"Debug","resolved_configuration":"Debug","observed_execution_count":0,"check_outcome_counts":{"missing":4}},{"artifact_id":12161151680,"lane_id":"groundhog.mdk011","profile":"groundhog.mdk011","plan_id":"fccd676fa5ec6c3fbdb78fa3a889aed6dba0923d8ea00333cfa1cae35974ceb4","receipt_kind":"lane_failure_v1","job_conclusion":"failure","complete":false,"complete_gate_eligible":false,"registration_audit":"not_performed","input_audit":"not_completed","failure_stage":"capability","failure_errors":[{"message":"live peer/image lifecycle requires the existing guarded adapter","type":"CheckFailure"}],"input_audit_gap_counts":{},"phase_configuration":"Debug","resolved_configuration":"Debug","observed_execution_count":0,"check_outcome_counts":{"missing":16}},{"artifact_id":12160903749,"lane_id":"groundhog.linux.sanitizers","profile":"groundhog.linux.sanitizers","plan_id":"fccd676fa5ec6c3fbdb78fa3a889aed6dba0923d8ea00333cfa1cae35974ceb4","receipt_kind":"lane_failure_v1","job_conclusion":"failure","complete":false,"complete_gate_eligible":false,"registration_audit":"not_performed","input_audit":"not_completed","failure_stage":"resolve","failure_errors":[{"message":"diagnostic nonmember fresh registration partition differs","type":"RealizationError"}],"input_audit_gap_counts":{},"phase_configuration":"Debug","resolved_configuration":null,"observed_execution_count":0,"check_outcome_counts":{"missing":62}},{"artifact_id":12160897422,"lane_id":"groundhog.mdk08","profile":"groundhog.mdk08","plan_id":"fccd676fa5ec6c3fbdb78fa3a889aed6dba0923d8ea00333cfa1cae35974ceb4","receipt_kind":"lane_failure_v1","job_conclusion":"failure","complete":false,"complete_gate_eligible":false,"registration_audit":"not_performed","input_audit":"not_completed","failure_stage":"configure","failure_errors":[{"message":"profile build type Debug differs from realized ''","type":"RealizationError"}],"input_audit_gap_counts":{},"phase_configuration":null,"resolved_configuration":null,"observed_execution_count":0,"check_outcome_counts":{"missing":1}},{"artifact_id":12160774441,"lane_id":"core.linux.asan","profile":"core.linux.asan","plan_id":"fccd676fa5ec6c3fbdb78fa3a889aed6dba0923d8ea00333cfa1cae35974ceb4","receipt_kind":"lane_failure_v1","job_conclusion":"failure","complete":false,"complete_gate_eligible":false,"registration_audit":"failed","input_audit":"failed","failure_stage":"preaudit","failure_errors":[{"message":"pre-build input audit failed; original findings retained","type":"CheckFailure"}],"input_audit_gap_counts":{"unclaimed_registration":3},"phase_configuration":"Debug","resolved_configuration":"Debug","observed_execution_count":0,"check_outcome_counts":{"missing":48}},{"artifact_id":12160754395,"lane_id":"libgo.macos.asan","profile":"libgo.macos.asan","plan_id":"fccd676fa5ec6c3fbdb78fa3a889aed6dba0923d8ea00333cfa1cae35974ceb4","receipt_kind":null,"job_conclusion":"success","complete":null,"complete_gate_eligible":false,"registration_audit":"passed","input_audit":"passed","failure_stage":null,"failure_errors":[],"input_audit_gap_counts":{},"phase_configuration":"Debug","resolved_configuration":null,"observed_execution_count":32,"check_outcome_counts":{"passed":2}}],"selection_and_worker_jobs":[{"id":117872853406,"name":"select","status":"completed","conclusion":"success","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194059430/job/117872853406"},{"id":117872920861,"name":"groundhog / execute","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194059430/job/117872920861"},{"id":117872920878,"name":"core_native / execute","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194059430/job/117872920878"},{"id":117872920932,"name":"grotto_asan / execute","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194059430/job/117872920932"},{"id":117872920939,"name":"grotto / execute","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194059430/job/117872920939"},{"id":117872920940,"name":"core / execute","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194059430/job/117872920940"},{"id":117872920947,"name":"mdk08 / execute","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194059430/job/117872920947"},{"id":117872920991,"name":"policy / execute","status":"completed","conclusion":"success","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194059430/job/117872920991"},{"id":117872921020,"name":"core_asan / execute","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194059430/job/117872921020"},{"id":117872921021,"name":"libgo_native / execute","status":"completed","conclusion":"success","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194059430/job/117872921021"},{"id":117872921070,"name":"groundhog_sanitizers / execute","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194059430/job/117872921070"},{"id":117872921095,"name":"mdk011 / execute","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194059430/job/117872921095"},{"id":117872921217,"name":"libgo / execute","status":"completed","conclusion":"success","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194059430/job/117872921217"},{"id":117883031219,"name":"Dependency gate","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194059430/job/117883031219"}]},
    {"id":39194781902,"run_attempt":1,"head_sha":"2c5b18c23344b3a4c32faf2738e6c34064ab1af7","event":"push","head_branch":"master","status":"completed","conclusion":"failure","created_at":"2026-10-19T03:27:34Z","updated_at":"2026-10-19T04:04:41Z","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194781902","pull_request_numbers":[],"qualification":{"pr_comparison":"ineligible_event","accepted_pr_numbers":[],"broad_integration":"failed","accepted_broad":false,"reason":"No PR event or reviewed legacy/selective pair. Broad required union not accepted: failed."},"job_count":61,"job_conclusion_or_status_counts":{"success":44,"failure":12,"cancelled":4,"skipped":1},"broad":{"nested_job_count":47,"conclusion_or_status_counts":{"success":40,"cancelled":4,"failure":2,"skipped":1},"accounting_job":{"id":117883493320,"name":"broad / Broad family accounting","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194781902/job/117883493320"},"failed_jobs":[{"id":117875216973,"name":"broad / fuzz / Fuzz json-event","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194781902/job/117875216973"},{"id":117883493320,"name":"broad / Broad family accounting","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194781902/job/117883493320"}],"other_non_success_jobs":[{"id":117875138816,"name":"broad / grotto / ubuntu-24.04 / asan-ubsan","status":"completed","conclusion":"cancelled","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194781902/job/117875138816"},{"id":117875138972,"name":"broad / grotto / ubuntu-24.04 / tsan (ADVISORY, non-gating)","status":"completed","conclusion":"cancelled","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194781902/job/117875138972"},{"id":117875139027,"name":"broad / grotto / ubuntu-latest / native","status":"completed","conclusion":"cancelled","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194781902/job/117875139027"},{"id":117875139085,"name":"broad / grotto / ubuntu-24.04 / native","status":"completed","conclusion":"cancelled","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194781902/job/117875139085"},{"id":117878837268,"name":"broad / fuzz / Generate coverage report","status":"completed","conclusion":"skipped","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194781902/job/117878837268"}]},"gate_job":{"id":117883760954,"name":"Dependency gate","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194781902/job/117883760954"},"artifact_api_total":42,"small_artifact_ids":[12162865868,12162075104,12161920854,12161786368,12161662149,12161497298,12161389193,12161267348,12161259410,12161214778,12161167413,12161162961,12161152656,12161134740],"plans":[{"artifact_id":12161167413,"plan_id":"368f3aa180642f6b60ed07b55731cfaab38a3f0193d8aef9b19148599b40a368","inventory_hash":"7a482cce308450e8646cf6faf56fc73a47934f53f0d71fd0778bb46acd970b85","base":null,"head":"2c5b18c23344b3a4c32faf2738e6c34064ab1af7","tested_sha":"2c5b18c23344b3a4c32faf2738e6c34064ab1af7","source_identity":{"kind":"committed","sha":"2c5b18c23344b3a4c32faf2738e6c34064ab1af7"},"context":"ci","effective_mode":"broad","partial_profiles":[],"expected_lane_ids":["core.linux","core.linux.asan","core.macos.asan","grotto.linux","grotto.linux.asan_ubsan","groundhog.linux","groundhog.linux.sanitizers","groundhog.mdk011","groundhog.mdk08","libgo.linux.asan","libgo.macos.asan","static.policy"]}],"aggregates":[{"artifact_id":12162865868,"plan_id":"368f3aa180642f6b60ed07b55731cfaab38a3f0193d8aef9b19148599b40a368","tested_sha":"2c5b18c23344b3a4c32faf2738e6c34064ab1af7","passed":false,"complete":true,"complete_gate_eligible":false,"error_count":465,"first_errors":["inventory coverage/enforcement readiness cannot satisfy complete gate","selected worker job conclusions missing/skipped/failed/cancelled"]}],"workflow_source":{"dependency-tests.yml":{"sha256":"a18010ceab7be2d96632ef8266c24fcb2e71e970dbdb4c09905ad17265f45f15","source_url":"https://github.com/chebizarro/nostrc/blob/2c5b18c23344b3a4c32faf2738e6c34064ab1af7/.github/workflows/dependency-tests.yml","artifact_attempt_lines":[{"line":61,"text":"name: dependency-plan-${{ steps.plan.outputs.plan_id }}-${{ github.run_attempt }}"},{"line":172,"text":"validation_artifact: dependency-validation-${{ needs.select.outputs.plan_id }}-${{ github.run_attempt }}"},{"line":192,"text":"name: dependency-plan-${{ needs.select.outputs.plan_id }}-${{ github.run_attempt }}"},{"line":197,"text":"pattern: dependency-receipt-${{ needs.select.outputs.plan_id }}-*-${{ github.run_attempt }}"},{"line":210,"text":"name: dependency-validation-${{ needs.select.outputs.plan_id }}-${{ github.run_attempt }}"}]},"dependency-broad.yml":{"sha256":"79d5ce5af5f517d037b83fc5d4c92d3d31172a5ef059202e0a4b8ffe00d383b6","source_url":"https://github.com/chebizarro/nostrc/blob/2c5b18c23344b3a4c32faf2738e6c34064ab1af7/.github/workflows/dependency-broad.yml","artifact_attempt_lines":[{"line":98,"text":"name: Broad family accounting"}]},"dependency-worker.yml":{"sha256":"db84c2c495a96ad0d76132bb788e54a4d0410d82d3da4e277e6d52408c719b12","source_url":"https://github.com/chebizarro/nostrc/blob/2c5b18c23344b3a4c32faf2738e6c34064ab1af7/.github/workflows/dependency-worker.yml","artifact_attempt_lines":[{"line":35,"text":"name: dependency-plan-${{ inputs.plan_id }}-${{ github.run_attempt }}"},{"line":167,"text":"name: dependency-receipt-${{ inputs.plan_id }}-${{ inputs.profile }}-${{ github.run_attempt }}"},{"line":174,"text":"name: dependency-evidence-${{ inputs.plan_id }}-${{ inputs.profile }}-${{ github.run_attempt }}"}]}},"lane_receipts":[{"artifact_id":12162075104,"lane_id":"core.linux.asan","profile":"core.linux.asan","plan_id":"368f3aa180642f6b60ed07b55731cfaab38a3f0193d8aef9b19148599b40a368","receipt_kind":"lane_failure_v1","job_conclusion":"failure","complete":false,"complete_gate_eligible":false,"registration_audit":"failed","input_audit":"failed","failure_stage":"preaudit","failure_errors":[{"message":"pre-build input audit failed; original findings retained","type":"CheckFailure"}],"input_audit_gap_counts":{"unclaimed_registration":3},"phase_configuration":"Debug","resolved_configuration":"Debug","observed_execution_count":0,"check_outcome_counts":{"missing":48}},{"artifact_id":12161920854,"lane_id":"core.macos.asan","profile":"core.macos.asan","plan_id":"368f3aa180642f6b60ed07b55731cfaab38a3f0193d8aef9b19148599b40a368","receipt_kind":"lane_failure_v1","job_conclusion":"failure","complete":false,"complete_gate_eligible":false,"registration_audit":"failed","input_audit":"failed","failure_stage":"preaudit","failure_errors":[{"message":"pre-build input audit failed; original findings retained","type":"CheckFailure"}],"input_audit_gap_counts":{"unclaimed_registration":3},"phase_configuration":"Debug","resolved_configuration":"Debug","observed_execution_count":0,"check_outcome_counts":{"missing":48}},{"artifact_id":12161786368,"lane_id":"grotto.linux.asan_ubsan","profile":"grotto.linux.asan_ubsan","plan_id":"368f3aa180642f6b60ed07b55731cfaab38a3f0193d8aef9b19148599b40a368","receipt_kind":"lane_failure_v1","job_conclusion":"failure","complete":false,"complete_gate_eligible":false,"registration_audit":"failed","input_audit":"failed","failure_stage":"preaudit","failure_errors":[{"message":"pre-build input audit failed; original findings retained","type":"CheckFailure"}],"input_audit_gap_counts":{"unclaimed_registration":322,"target_mapping":1,"missing_impact_edge":330,"generated_runtime_without_target":2},"phase_configuration":"Debug","resolved_configuration":"Debug","observed_execution_count":0,"check_outcome_counts":{"missing":4}},{"artifact_id":12161662149,"lane_id":"groundhog.mdk08","profile":"groundhog.mdk08","plan_id":"368f3aa180642f6b60ed07b55731cfaab38a3f0193d8aef9b19148599b40a368","receipt_kind":"lane_failure_v1","job_conclusion":"failure","complete":false,"complete_gate_eligible":false,"registration_audit":"not_performed","input_audit":"not_completed","failure_stage":"capability","failure_errors":[{"message":"live peer/image lifecycle requires the existing guarded adapter","type":"CheckFailure"}],"input_audit_gap_counts":{},"phase_configuration":"Debug","resolved_configuration":"Debug","observed_execution_count":0,"check_outcome_counts":{"missing":1}},{"artifact_id":12161497298,"lane_id":"static.policy","profile":"static.policy","plan_id":"368f3aa180642f6b60ed07b55731cfaab38a3f0193d8aef9b19148599b40a368","receipt_kind":null,"job_conclusion":"success","complete":null,"complete_gate_eligible":false,"registration_audit":"passed","input_audit":"passed","failure_stage":null,"failure_errors":[],"input_audit_gap_counts":{},"phase_configuration":"","resolved_configuration":null,"observed_execution_count":1,"check_outcome_counts":{"passed":1}},{"artifact_id":12161389193,"lane_id":"groundhog.linux.sanitizers","profile":"groundhog.linux.sanitizers","plan_id":"368f3aa180642f6b60ed07b55731cfaab38a3f0193d8aef9b19148599b40a368","receipt_kind":"lane_failure_v1","job_conclusion":"failure","complete":false,"complete_gate_eligible":false,"registration_audit":"not_performed","input_audit":"not_completed","failure_stage":"resolve","failure_errors":[{"message":"diagnostic nonmember fresh registration partition differs","type":"RealizationError"}],"input_audit_gap_counts":{},"phase_configuration":"Debug","resolved_configuration":null,"observed_execution_count":0,"check_outcome_counts":{"missing":62}},{"artifact_id":12161267348,"lane_id":"libgo.macos.asan","profile":"libgo.macos.asan","plan_id":"368f3aa180642f6b60ed07b55731cfaab38a3f0193d8aef9b19148599b40a368","receipt_kind":null,"job_conclusion":"success","complete":null,"complete_gate_eligible":false,"registration_audit":"passed","input_audit":"passed","failure_stage":null,"failure_errors":[],"input_audit_gap_counts":{},"phase_configuration":"Debug","resolved_configuration":null,"observed_execution_count":32,"check_outcome_counts":{"passed":2}},{"artifact_id":12161259410,"lane_id":"libgo.linux.asan","profile":"libgo.linux.asan","plan_id":"368f3aa180642f6b60ed07b55731cfaab38a3f0193d8aef9b19148599b40a368","receipt_kind":null,"job_conclusion":"success","complete":null,"complete_gate_eligible":false,"registration_audit":"passed","input_audit":"passed","failure_stage":null,"failure_errors":[],"input_audit_gap_counts":{},"phase_configuration":"Debug","resolved_configuration":null,"observed_execution_count":33,"check_outcome_counts":{"passed":2}},{"artifact_id":12161214778,"lane_id":"grotto.linux","profile":"grotto.linux","plan_id":"368f3aa180642f6b60ed07b55731cfaab38a3f0193d8aef9b19148599b40a368","receipt_kind":"lane_failure_v1","job_conclusion":"failure","complete":false,"complete_gate_eligible":false,"registration_audit":"failed","input_audit":"failed","failure_stage":"preaudit","failure_errors":[{"message":"pre-build input audit failed; original findings retained","type":"CheckFailure"}],"input_audit_gap_counts":{"unclaimed_registration":402,"missing_impact_edge":9104,"generated_runtime_without_target":2},"phase_configuration":"Debug","resolved_configuration":"Debug","observed_execution_count":0,"check_outcome_counts":{"missing":4}},{"artifact_id":12161162961,"lane_id":"groundhog.mdk011","profile":"groundhog.mdk011","plan_id":"368f3aa180642f6b60ed07b55731cfaab38a3f0193d8aef9b19148599b40a368","receipt_kind":"lane_failure_v1","job_conclusion":"failure","complete":false,"complete_gate_eligible":false,"registration_audit":"not_performed","input_audit":"not_completed","failure_stage":"capability","failure_errors":[{"message":"live peer/image lifecycle requires the existing guarded adapter","type":"CheckFailure"}],"input_audit_gap_counts":{},"phase_configuration":"Debug","resolved_configuration":"Debug","observed_execution_count":0,"check_outcome_counts":{"missing":16}},{"artifact_id":12161152656,"lane_id":"core.linux","profile":"core.linux","plan_id":"368f3aa180642f6b60ed07b55731cfaab38a3f0193d8aef9b19148599b40a368","receipt_kind":"lane_failure_v1","job_conclusion":"failure","complete":false,"complete_gate_eligible":false,"registration_audit":"failed","input_audit":"failed","failure_stage":"preaudit","failure_errors":[{"message":"pre-build input audit failed; original findings retained","type":"CheckFailure"}],"input_audit_gap_counts":{"unclaimed_registration":429,"missing_impact_edge":1378,"generated_runtime_without_target":2,"target_mapping":1},"phase_configuration":"Debug","resolved_configuration":"Debug","observed_execution_count":0,"check_outcome_counts":{"missing":3}},{"artifact_id":12161134740,"lane_id":"groundhog.linux","profile":"groundhog.linux","plan_id":"368f3aa180642f6b60ed07b55731cfaab38a3f0193d8aef9b19148599b40a368","receipt_kind":"lane_failure_v1","job_conclusion":"failure","complete":false,"complete_gate_eligible":false,"registration_audit":"failed","input_audit":"failed","failure_stage":"preaudit","failure_errors":[{"message":"pre-build input audit failed; original findings retained","type":"CheckFailure"}],"input_audit_gap_counts":{"unclaimed_registration":408,"missing_impact_edge":9473,"check_descriptor_owner_disagreement":1,"target_mapping":1},"phase_configuration":"Debug","resolved_configuration":"Debug","observed_execution_count":0,"check_outcome_counts":{"missing":10}}],"selection_and_worker_jobs":[{"id":117875070205,"name":"select","status":"completed","conclusion":"success","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194781902/job/117875070205"},{"id":117875136936,"name":"core / execute","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194781902/job/117875136936"},{"id":117875136954,"name":"grotto_asan / execute","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194781902/job/117875136954"},{"id":117875136983,"name":"core_asan / execute","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194781902/job/117875136983"},{"id":117875137062,"name":"libgo_native / execute","status":"completed","conclusion":"success","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194781902/job/117875137062"},{"id":117875137089,"name":"groundhog / execute","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194781902/job/117875137089"},{"id":117875137113,"name":"mdk08 / execute","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194781902/job/117875137113"},{"id":117875137117,"name":"core_native / execute","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194781902/job/117875137117"},{"id":117875137139,"name":"grotto / execute","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194781902/job/117875137139"},{"id":117875137252,"name":"mdk011 / execute","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194781902/job/117875137252"},{"id":117875137366,"name":"policy / execute","status":"completed","conclusion":"success","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194781902/job/117875137366"},{"id":117875137418,"name":"libgo / execute","status":"completed","conclusion":"success","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194781902/job/117875137418"},{"id":117875137464,"name":"groundhog_sanitizers / execute","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194781902/job/117875137464"},{"id":117883760954,"name":"Dependency gate","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39194781902/job/117883760954"}]},
    {"id":39196963896,"run_attempt":1,"head_sha":"e57ef16ed1aeff152d808aec407c1b6f86bfc6be","event":"push","head_branch":"master","status":"pending","conclusion":null,"created_at":"2026-10-19T03:55:28Z","updated_at":"2026-10-19T03:55:44Z","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39196963896","pull_request_numbers":[],"qualification":{"pr_comparison":"ineligible_event","accepted_pr_numbers":[],"broad_integration":"incomplete","accepted_broad":false,"reason":"No PR event or reviewed legacy/selective pair. Broad required union not accepted: incomplete."},"job_count":58,"job_conclusion_or_status_counts":{"success":38,"failure":10,"in_progress":7,"pending":3},"broad":{"nested_job_count":45,"conclusion_or_status_counts":{"success":34,"in_progress":7,"pending":3,"failure":1},"accounting_job":null,"failed_jobs":[{"id":117883093746,"name":"broad / fuzz / Fuzz json-event","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39196963896/job/117883093746"}],"other_non_success_jobs":[{"id":117881758090,"name":"broad / groundhog / groundhog","status":"in_progress","conclusion":null,"html_url":"https://github.com/chebizarro/nostrc/actions/runs/39196963896/job/117881758090"},{"id":117881758116,"name":"broad / core / ubuntu-latest • asan_ubsan","status":"in_progress","conclusion":null,"html_url":"https://github.com/chebizarro/nostrc/actions/runs/39196963896/job/117881758116"},{"id":117881758366,"name":"broad / groundhog / groundhog-sanitizers","status":"in_progress","conclusion":null,"html_url":"https://github.com/chebizarro/nostrc/actions/runs/39196963896/job/117881758366"},{"id":117881758411,"name":"broad / bp_extended / nightly-matrix","status":"in_progress","conclusion":null,"html_url":"https://github.com/chebizarro/nostrc/actions/runs/39196963896/job/117881758411"},{"id":117881759141,"name":"broad / grotto / ubuntu-24.04 / asan-ubsan","status":"pending","conclusion":null,"html_url":"https://github.com/chebizarro/nostrc/actions/runs/39196963896/job/117881759141"},{"id":117881759266,"name":"broad / grotto / ubuntu-24.04 / native","status":"in_progress","conclusion":null,"html_url":"https://github.com/chebizarro/nostrc/actions/runs/39196963896/job/117881759266"},{"id":117881759286,"name":"broad / grotto / ubuntu-24.04 / tsan (ADVISORY, non-gating)","status":"pending","conclusion":null,"html_url":"https://github.com/chebizarro/nostrc/actions/runs/39196963896/job/117881759286"},{"id":117881759351,"name":"broad / grotto / ubuntu-latest / native","status":"pending","conclusion":null,"html_url":"https://github.com/chebizarro/nostrc/actions/runs/39196963896/job/117881759351"},{"id":117883093565,"name":"broad / fuzz / Fuzz nip49","status":"in_progress","conclusion":null,"html_url":"https://github.com/chebizarro/nostrc/actions/runs/39196963896/job/117883093565"},{"id":117883093625,"name":"broad / fuzz / Fuzz bip39","status":"in_progress","conclusion":null,"html_url":"https://github.com/chebizarro/nostrc/actions/runs/39196963896/job/117883093625"}]},"gate_job":null,"artifact_api_total":37,"small_artifact_ids":[12162710461,12162620869,12162580931,12162500926,12162378133,12162377959,12162278290,12162198588,12162083957,12162024105,12161969867,12161944729,12161519019],"plans":[{"artifact_id":12162500926,"plan_id":"237a4ae7e06f9a651e7f909b8cf2c7425696fe18fd890ba4597e956091de080a","inventory_hash":"7a482cce308450e8646cf6faf56fc73a47934f53f0d71fd0778bb46acd970b85","base":null,"head":"e57ef16ed1aeff152d808aec407c1b6f86bfc6be","tested_sha":"e57ef16ed1aeff152d808aec407c1b6f86bfc6be","source_identity":{"kind":"committed","sha":"e57ef16ed1aeff152d808aec407c1b6f86bfc6be"},"context":"ci","effective_mode":"broad","partial_profiles":[],"expected_lane_ids":["core.linux","core.linux.asan","core.macos.asan","grotto.linux","grotto.linux.asan_ubsan","groundhog.linux","groundhog.linux.sanitizers","groundhog.mdk011","groundhog.mdk08","libgo.linux.asan","libgo.macos.asan","static.policy"]}],"aggregates":[],"workflow_source":{"dependency-tests.yml":{"sha256":"a18010ceab7be2d96632ef8266c24fcb2e71e970dbdb4c09905ad17265f45f15","source_url":"https://github.com/chebizarro/nostrc/blob/e57ef16ed1aeff152d808aec407c1b6f86bfc6be/.github/workflows/dependency-tests.yml","artifact_attempt_lines":[{"line":61,"text":"name: dependency-plan-${{ steps.plan.outputs.plan_id }}-${{ github.run_attempt }}"},{"line":172,"text":"validation_artifact: dependency-validation-${{ needs.select.outputs.plan_id }}-${{ github.run_attempt }}"},{"line":192,"text":"name: dependency-plan-${{ needs.select.outputs.plan_id }}-${{ github.run_attempt }}"},{"line":197,"text":"pattern: dependency-receipt-${{ needs.select.outputs.plan_id }}-*-${{ github.run_attempt }}"},{"line":210,"text":"name: dependency-validation-${{ needs.select.outputs.plan_id }}-${{ github.run_attempt }}"}]},"dependency-broad.yml":{"sha256":"79d5ce5af5f517d037b83fc5d4c92d3d31172a5ef059202e0a4b8ffe00d383b6","source_url":"https://github.com/chebizarro/nostrc/blob/e57ef16ed1aeff152d808aec407c1b6f86bfc6be/.github/workflows/dependency-broad.yml","artifact_attempt_lines":[{"line":98,"text":"name: Broad family accounting"}]},"dependency-worker.yml":{"sha256":"db84c2c495a96ad0d76132bb788e54a4d0410d82d3da4e277e6d52408c719b12","source_url":"https://github.com/chebizarro/nostrc/blob/e57ef16ed1aeff152d808aec407c1b6f86bfc6be/.github/workflows/dependency-worker.yml","artifact_attempt_lines":[{"line":35,"text":"name: dependency-plan-${{ inputs.plan_id }}-${{ github.run_attempt }}"},{"line":167,"text":"name: dependency-receipt-${{ inputs.plan_id }}-${{ inputs.profile }}-${{ github.run_attempt }}"},{"line":174,"text":"name: dependency-evidence-${{ inputs.plan_id }}-${{ inputs.profile }}-${{ github.run_attempt }}"}]}},"lane_receipts":[{"artifact_id":12162710461,"lane_id":"core.macos.asan","profile":"core.macos.asan","plan_id":"237a4ae7e06f9a651e7f909b8cf2c7425696fe18fd890ba4597e956091de080a","receipt_kind":"lane_failure_v1","job_conclusion":"failure","complete":false,"complete_gate_eligible":false,"registration_audit":"failed","input_audit":"failed","failure_stage":"preaudit","failure_errors":[{"message":"pre-build input audit failed; original findings retained","type":"CheckFailure"}],"input_audit_gap_counts":{"unclaimed_registration":3},"phase_configuration":"Debug","resolved_configuration":"Debug","observed_execution_count":0,"check_outcome_counts":{"missing":48}},{"artifact_id":12162620869,"lane_id":"core.linux","profile":"core.linux","plan_id":"237a4ae7e06f9a651e7f909b8cf2c7425696fe18fd890ba4597e956091de080a","receipt_kind":"lane_failure_v1","job_conclusion":"failure","complete":false,"complete_gate_eligible":false,"registration_audit":"failed","input_audit":"failed","failure_stage":"preaudit","failure_errors":[{"message":"pre-build input audit failed; original findings retained","type":"CheckFailure"}],"input_audit_gap_counts":{"unclaimed_registration":429,"missing_impact_edge":1380,"generated_runtime_without_target":2},"phase_configuration":"Debug","resolved_configuration":"Debug","observed_execution_count":0,"check_outcome_counts":{"missing":3}},{"artifact_id":12162580931,"lane_id":"groundhog.mdk08","profile":"groundhog.mdk08","plan_id":"237a4ae7e06f9a651e7f909b8cf2c7425696fe18fd890ba4597e956091de080a","receipt_kind":"lane_failure_v1","job_conclusion":"failure","complete":false,"complete_gate_eligible":false,"registration_audit":"not_performed","input_audit":"not_completed","failure_stage":"capability","failure_errors":[{"message":"live peer/image lifecycle requires the existing guarded adapter","type":"CheckFailure"}],"input_audit_gap_counts":{},"phase_configuration":"Debug","resolved_configuration":"Debug","observed_execution_count":0,"check_outcome_counts":{"missing":1}},{"artifact_id":12162378133,"lane_id":"grotto.linux","profile":"grotto.linux","plan_id":"237a4ae7e06f9a651e7f909b8cf2c7425696fe18fd890ba4597e956091de080a","receipt_kind":"lane_failure_v1","job_conclusion":"failure","complete":false,"complete_gate_eligible":false,"registration_audit":"failed","input_audit":"failed","failure_stage":"preaudit","failure_errors":[{"message":"pre-build input audit failed; original findings retained","type":"CheckFailure"}],"input_audit_gap_counts":{"unclaimed_registration":402,"missing_impact_edge":9104,"generated_runtime_without_target":2},"phase_configuration":"Debug","resolved_configuration":"Debug","observed_execution_count":0,"check_outcome_counts":{"missing":4}},{"artifact_id":12162377959,"lane_id":"libgo.macos.asan","profile":"libgo.macos.asan","plan_id":"237a4ae7e06f9a651e7f909b8cf2c7425696fe18fd890ba4597e956091de080a","receipt_kind":null,"job_conclusion":"success","complete":null,"complete_gate_eligible":false,"registration_audit":"passed","input_audit":"passed","failure_stage":null,"failure_errors":[],"input_audit_gap_counts":{},"phase_configuration":"Debug","resolved_configuration":null,"observed_execution_count":32,"check_outcome_counts":{"passed":2}},{"artifact_id":12162278290,"lane_id":"static.policy","profile":"static.policy","plan_id":"237a4ae7e06f9a651e7f909b8cf2c7425696fe18fd890ba4597e956091de080a","receipt_kind":null,"job_conclusion":"success","complete":null,"complete_gate_eligible":false,"registration_audit":"passed","input_audit":"passed","failure_stage":null,"failure_errors":[],"input_audit_gap_counts":{},"phase_configuration":"","resolved_configuration":null,"observed_execution_count":1,"check_outcome_counts":{"passed":1}},{"artifact_id":12162198588,"lane_id":"libgo.linux.asan","profile":"libgo.linux.asan","plan_id":"237a4ae7e06f9a651e7f909b8cf2c7425696fe18fd890ba4597e956091de080a","receipt_kind":null,"job_conclusion":"success","complete":null,"complete_gate_eligible":false,"registration_audit":"passed","input_audit":"passed","failure_stage":null,"failure_errors":[],"input_audit_gap_counts":{},"phase_configuration":"Debug","resolved_configuration":null,"observed_execution_count":33,"check_outcome_counts":{"passed":2}},{"artifact_id":12162083957,"lane_id":"grotto.linux.asan_ubsan","profile":"grotto.linux.asan_ubsan","plan_id":"237a4ae7e06f9a651e7f909b8cf2c7425696fe18fd890ba4597e956091de080a","receipt_kind":"lane_failure_v1","job_conclusion":"failure","complete":false,"complete_gate_eligible":false,"registration_audit":"failed","input_audit":"failed","failure_stage":"preaudit","failure_errors":[{"message":"pre-build input audit failed; original findings retained","type":"CheckFailure"}],"input_audit_gap_counts":{"unclaimed_registration":321,"missing_impact_edge":330,"generated_runtime_without_target":2},"phase_configuration":"Debug","resolved_configuration":"Debug","observed_execution_count":0,"check_outcome_counts":{"missing":4}},{"artifact_id":12162024105,"lane_id":"core.linux.asan","profile":"core.linux.asan","plan_id":"237a4ae7e06f9a651e7f909b8cf2c7425696fe18fd890ba4597e956091de080a","receipt_kind":"lane_failure_v1","job_conclusion":"failure","complete":false,"complete_gate_eligible":false,"registration_audit":"failed","input_audit":"failed","failure_stage":"preaudit","failure_errors":[{"message":"pre-build input audit failed; original findings retained","type":"CheckFailure"}],"input_audit_gap_counts":{"unclaimed_registration":3},"phase_configuration":"Debug","resolved_configuration":"Debug","observed_execution_count":0,"check_outcome_counts":{"missing":48}},{"artifact_id":12161969867,"lane_id":"groundhog.mdk011","profile":"groundhog.mdk011","plan_id":"237a4ae7e06f9a651e7f909b8cf2c7425696fe18fd890ba4597e956091de080a","receipt_kind":"lane_failure_v1","job_conclusion":"failure","complete":false,"complete_gate_eligible":false,"registration_audit":"not_performed","input_audit":"not_completed","failure_stage":"capability","failure_errors":[{"message":"live peer/image lifecycle requires the existing guarded adapter","type":"CheckFailure"}],"input_audit_gap_counts":{},"phase_configuration":"Debug","resolved_configuration":"Debug","observed_execution_count":0,"check_outcome_counts":{"missing":16}},{"artifact_id":12161944729,"lane_id":"groundhog.linux.sanitizers","profile":"groundhog.linux.sanitizers","plan_id":"237a4ae7e06f9a651e7f909b8cf2c7425696fe18fd890ba4597e956091de080a","receipt_kind":"lane_failure_v1","job_conclusion":"failure","complete":false,"complete_gate_eligible":false,"registration_audit":"not_performed","input_audit":"not_completed","failure_stage":"resolve","failure_errors":[{"message":"diagnostic nonmember fresh registration partition differs","type":"RealizationError"}],"input_audit_gap_counts":{},"phase_configuration":"Debug","resolved_configuration":null,"observed_execution_count":0,"check_outcome_counts":{"missing":62}},{"artifact_id":12161519019,"lane_id":"groundhog.linux","profile":"groundhog.linux","plan_id":"237a4ae7e06f9a651e7f909b8cf2c7425696fe18fd890ba4597e956091de080a","receipt_kind":"lane_failure_v1","job_conclusion":"failure","complete":false,"complete_gate_eligible":false,"registration_audit":"failed","input_audit":"failed","failure_stage":"preaudit","failure_errors":[{"message":"pre-build input audit failed; original findings retained","type":"CheckFailure"}],"input_audit_gap_counts":{"unclaimed_registration":408,"missing_impact_edge":9475,"check_descriptor_owner_disagreement":1},"phase_configuration":"Debug","resolved_configuration":"Debug","observed_execution_count":0,"check_outcome_counts":{"missing":10}}],"selection_and_worker_jobs":[{"id":117881700971,"name":"select","status":"completed","conclusion":"success","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39196963896/job/117881700971"},{"id":117881757755,"name":"grotto_asan / execute","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39196963896/job/117881757755"},{"id":117881757784,"name":"groundhog / execute","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39196963896/job/117881757784"},{"id":117881757786,"name":"groundhog_sanitizers / execute","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39196963896/job/117881757786"},{"id":117881757809,"name":"core_native / execute","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39196963896/job/117881757809"},{"id":117881757818,"name":"core / execute","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39196963896/job/117881757818"},{"id":117881757820,"name":"mdk08 / execute","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39196963896/job/117881757820"},{"id":117881757827,"name":"core_asan / execute","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39196963896/job/117881757827"},{"id":117881757878,"name":"mdk011 / execute","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39196963896/job/117881757878"},{"id":117881757909,"name":"libgo_native / execute","status":"completed","conclusion":"success","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39196963896/job/117881757909"},{"id":117881757950,"name":"policy / execute","status":"completed","conclusion":"success","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39196963896/job/117881757950"},{"id":117881757963,"name":"libgo / execute","status":"completed","conclusion":"success","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39196963896/job/117881757963"},{"id":117881758013,"name":"grotto / execute","status":"completed","conclusion":"failure","html_url":"https://github.com/chebizarro/nostrc/actions/runs/39196963896/job/117881758013"}]}
  ],
  "new_artifacts": [
    {"run_id":39196963896,"run_attempt":1,"source_sha":"e57ef16ed1aeff152d808aec407c1b6f86bfc6be","artifact_id":12162710461,"name":"dependency-receipt-237a4ae7e06f9a651e7f909b8cf2c7425696fe18fd890ba4597e956091de080a-core.macos.asan-1","expired":false,"created_at":"2026-10-19T03:57:01Z","updated_at":"2026-10-19T03:57:01Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12162710461","metadata_digest":"sha256:8040544eee1e98d3c7eab51f3908fc4396fe1443fedadc483394e20eaafce2c8","download_sha256":"8040544eee1e98d3c7eab51f3908fc4396fe1443fedadc483394e20eaafce2c8","download_size_bytes":57903,"crc":"passed","member_sha256":{"core.macos.asan/worker.log":"173430c24f2ae508d76f8d596ffe08f3a12825f6f07420e0018ddb2d32374e15","core.macos.asan/receipt.json":"88933a4b14375854237b44f219452c03b7714068ce9eb3e817d41901ab1f4983"}},
    {"run_id":39196963896,"run_attempt":1,"source_sha":"e57ef16ed1aeff152d808aec407c1b6f86bfc6be","artifact_id":12162620869,"name":"dependency-receipt-237a4ae7e06f9a651e7f909b8cf2c7425696fe18fd890ba4597e956091de080a-core.linux-1","expired":false,"created_at":"2026-10-19T03:57:36Z","updated_at":"2026-10-19T03:57:36Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12162620869","metadata_digest":"sha256:e3c513ea073f04f3d00d0d66c7f73635ff37e47c8b684b860411927204b23371","download_sha256":"e3c513ea073f04f3d00d0d66c7f73635ff37e47c8b684b860411927204b23371","download_size_bytes":100134,"crc":"passed","member_sha256":{"core.linux/receipt.json":"8243a1d7eefcd39777bc26641124b2d9b9df2b90259c3123121928e23e4f11ee","core.linux/worker.log":"173430c24f2ae508d76f8d596ffe08f3a12825f6f07420e0018ddb2d32374e15"}},
    {"run_id":39196963896,"run_attempt":1,"source_sha":"e57ef16ed1aeff152d808aec407c1b6f86bfc6be","artifact_id":12162580931,"name":"dependency-receipt-237a4ae7e06f9a651e7f909b8cf2c7425696fe18fd890ba4597e956091de080a-groundhog.mdk08-1","expired":false,"created_at":"2026-10-19T03:57:12Z","updated_at":"2026-10-19T03:57:12Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12162580931","metadata_digest":"sha256:a69688773090fb3a9b438f8333b90fd0c0e06fdec1e4fe129bbb831e06cc1ff4","download_sha256":"a69688773090fb3a9b438f8333b90fd0c0e06fdec1e4fe129bbb831e06cc1ff4","download_size_bytes":17590,"crc":"passed","member_sha256":{"groundhog.mdk08/receipt.json":"350a9147ac7da4ac92e8f431a9fc4d8b877654033926c25f4c7b6844b06a7b15","groundhog.mdk08/worker.log":"c3248197fc46c39acc93fb321661c79fa213f9f3ab8d6d7c86ed0ff9fb1d050e"}},
    {"run_id":39196963896,"run_attempt":1,"source_sha":"e57ef16ed1aeff152d808aec407c1b6f86bfc6be","artifact_id":12162500926,"name":"dependency-plan-237a4ae7e06f9a651e7f909b8cf2c7425696fe18fd890ba4597e956091de080a-1","expired":false,"created_at":"2026-10-19T03:55:42Z","updated_at":"2026-10-19T03:55:42Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12162500926","metadata_digest":"sha256:86688b0aa7651522d29375abb38c5143bb78df6fc16b4b966c9384854b645ba2","download_sha256":"86688b0aa7651522d29375abb38c5143bb78df6fc16b4b966c9384854b645ba2","download_size_bytes":7689,"crc":"passed","member_sha256":{"plan.json":"aab74a25a5f972cb05cf053adafe53a20b3fedadd22e776278994ffb322eeca5"}},
    {"run_id":39196963896,"run_attempt":1,"source_sha":"e57ef16ed1aeff152d808aec407c1b6f86bfc6be","artifact_id":12162378133,"name":"dependency-receipt-237a4ae7e06f9a651e7f909b8cf2c7425696fe18fd890ba4597e956091de080a-grotto.linux-1","expired":false,"created_at":"2026-10-19T04:05:06Z","updated_at":"2026-10-19T04:05:06Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12162378133","metadata_digest":"sha256:7ea211ee6ce4956e907d384057022d265dca98d098981ec2e9305fcfc59df3a2","download_sha256":"7ea211ee6ce4956e907d384057022d265dca98d098981ec2e9305fcfc59df3a2","download_size_bytes":192926,"crc":"passed","member_sha256":{"grotto.linux/receipt.json":"1be23efc22aed33a25b5dc8a54fe9e2ca4b1361bff83ca1c72b0fbf5ec307860","grotto.linux/worker.log":"173430c24f2ae508d76f8d596ffe08f3a12825f6f07420e0018ddb2d32374e15"}},
    {"run_id":39196963896,"run_attempt":1,"source_sha":"e57ef16ed1aeff152d808aec407c1b6f86bfc6be","artifact_id":12162377959,"name":"dependency-receipt-237a4ae7e06f9a651e7f909b8cf2c7425696fe18fd890ba4597e956091de080a-libgo.macos.asan-1","expired":false,"created_at":"2026-10-19T04:04:00Z","updated_at":"2026-10-19T04:04:00Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12162377959","metadata_digest":"sha256:c82b8912a520ebb1df6e382bc7525f05439a40865e57c2302d8a7ee61286bf1c","download_sha256":"c82b8912a520ebb1df6e382bc7525f05439a40865e57c2302d8a7ee61286bf1c","download_size_bytes":27751,"crc":"passed","member_sha256":{"libgo.macos.asan/receipt.json":"e586c1b58461540111e17825984ef10fc711db4d649d82b495f8532e4341b2da","libgo.macos.asan/worker.log":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}},
    {"run_id":39196963896,"run_attempt":1,"source_sha":"e57ef16ed1aeff152d808aec407c1b6f86bfc6be","artifact_id":12162278290,"name":"dependency-receipt-237a4ae7e06f9a651e7f909b8cf2c7425696fe18fd890ba4597e956091de080a-static.policy-1","expired":false,"created_at":"2026-10-19T04:02:47Z","updated_at":"2026-10-19T04:02:47Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12162278290","metadata_digest":"sha256:b7395872b55a4686d5792edcaf1c12cd59771ec572addbc3afe07d2850dea4f6","download_sha256":"b7395872b55a4686d5792edcaf1c12cd59771ec572addbc3afe07d2850dea4f6","download_size_bytes":22087,"crc":"passed","member_sha256":{"static.policy/receipt.json":"24536c3443cd12470b184663086966bf83b8bc3614c4e025234500ed081d4e4c","static.policy/worker.log":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}},
    {"run_id":39196963896,"run_attempt":1,"source_sha":"e57ef16ed1aeff152d808aec407c1b6f86bfc6be","artifact_id":12162198588,"name":"dependency-receipt-237a4ae7e06f9a651e7f909b8cf2c7425696fe18fd890ba4597e956091de080a-libgo.linux.asan-1","expired":false,"created_at":"2026-10-19T04:03:28Z","updated_at":"2026-10-19T04:03:28Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12162198588","metadata_digest":"sha256:a20abe0a9ee928da15d684aae2a7961730970283fa9bb1c6a07d106de0fa1fe8","download_sha256":"a20abe0a9ee928da15d684aae2a7961730970283fa9bb1c6a07d106de0fa1fe8","download_size_bytes":24616,"crc":"passed","member_sha256":{"libgo.linux.asan/receipt.json":"846abd54cdddd28ded304d9f2104145222b6d4bfd79107541fcc4d5c89889400","libgo.linux.asan/worker.log":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}},
    {"run_id":39196963896,"run_attempt":1,"source_sha":"e57ef16ed1aeff152d808aec407c1b6f86bfc6be","artifact_id":12162083957,"name":"dependency-receipt-237a4ae7e06f9a651e7f909b8cf2c7425696fe18fd890ba4597e956091de080a-grotto.linux.asan_ubsan-1","expired":false,"created_at":"2026-10-19T03:58:31Z","updated_at":"2026-10-19T03:58:31Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12162083957","metadata_digest":"sha256:60f6698a88a7cfb537492c92fa39b68dd6ac5eb24b0e55c44ca3b3184c684cef","download_sha256":"60f6698a88a7cfb537492c92fa39b68dd6ac5eb24b0e55c44ca3b3184c684cef","download_size_bytes":96437,"crc":"passed","member_sha256":{"grotto.linux.asan_ubsan/receipt.json":"d7fba7cad47e2daad4cf2567399fe09bae5d9db8e8564131e9800a1371666667","grotto.linux.asan_ubsan/worker.log":"173430c24f2ae508d76f8d596ffe08f3a12825f6f07420e0018ddb2d32374e15"}},
    {"run_id":39196963896,"run_attempt":1,"source_sha":"e57ef16ed1aeff152d808aec407c1b6f86bfc6be","artifact_id":12162024105,"name":"dependency-receipt-237a4ae7e06f9a651e7f909b8cf2c7425696fe18fd890ba4597e956091de080a-core.linux.asan-1","expired":false,"created_at":"2026-10-19T03:57:40Z","updated_at":"2026-10-19T03:57:40Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12162024105","metadata_digest":"sha256:38927fe1e23d2f4aa270ab8a8fef2ef983da490e387e4df89c632e8138b29401","download_sha256":"38927fe1e23d2f4aa270ab8a8fef2ef983da490e387e4df89c632e8138b29401","download_size_bytes":57857,"crc":"passed","member_sha256":{"core.linux.asan/receipt.json":"df78637605aaaa0f0ac1f436f535dd53979511e9e24a67dc3a676f08ba78e913","core.linux.asan/worker.log":"173430c24f2ae508d76f8d596ffe08f3a12825f6f07420e0018ddb2d32374e15"}},
    {"run_id":39196963896,"run_attempt":1,"source_sha":"e57ef16ed1aeff152d808aec407c1b6f86bfc6be","artifact_id":12161969867,"name":"dependency-receipt-237a4ae7e06f9a651e7f909b8cf2c7425696fe18fd890ba4597e956091de080a-groundhog.mdk011-1","expired":false,"created_at":"2026-10-19T03:59:52Z","updated_at":"2026-10-19T03:59:52Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12161969867","metadata_digest":"sha256:5f0baca2fc14ded68f96c7542e7a3ebe4d7335120aa9cd16dba57927dc51b133","download_sha256":"5f0baca2fc14ded68f96c7542e7a3ebe4d7335120aa9cd16dba57927dc51b133","download_size_bytes":16369,"crc":"passed","member_sha256":{"groundhog.mdk011/receipt.json":"5bea57f13fcc8cc65737138a450b1b6d165d7bb8a30c2b853aaa4ef6bfdbbb12","groundhog.mdk011/worker.log":"c3248197fc46c39acc93fb321661c79fa213f9f3ab8d6d7c86ed0ff9fb1d050e"}},
    {"run_id":39196963896,"run_attempt":1,"source_sha":"e57ef16ed1aeff152d808aec407c1b6f86bfc6be","artifact_id":12161944729,"name":"dependency-receipt-237a4ae7e06f9a651e7f909b8cf2c7425696fe18fd890ba4597e956091de080a-groundhog.linux.sanitizers-1","expired":false,"created_at":"2026-10-19T03:57:27Z","updated_at":"2026-10-19T03:57:27Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12161944729","metadata_digest":"sha256:e250bcb1b6dad37f33f538d4b8b4a5cc81b3c741ef501fe56736803ebe612274","download_sha256":"e250bcb1b6dad37f33f538d4b8b4a5cc81b3c741ef501fe56736803ebe612274","download_size_bytes":7376,"crc":"passed","member_sha256":{"groundhog.linux.sanitizers/receipt.json":"ab7a4a2d5207ee658029fc79b40fb95d335f78a039aaf530f13c000df7c0e939","groundhog.linux.sanitizers/worker.log":"389e76c63f8828b5a7a5f6faf9587d06aff79d72a44ecab7151c0eaac496496c"}},
    {"run_id":39196963896,"run_attempt":1,"source_sha":"e57ef16ed1aeff152d808aec407c1b6f86bfc6be","artifact_id":12161519019,"name":"dependency-receipt-237a4ae7e06f9a651e7f909b8cf2c7425696fe18fd890ba4597e956091de080a-groundhog.linux-1","expired":false,"created_at":"2026-10-19T03:57:40Z","updated_at":"2026-10-19T03:57:40Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12161519019","metadata_digest":"sha256:7e62daceb2e298a44300f50b447f9e55b85bbee7d796c1190c80c43eabcb41f5","download_sha256":"7e62daceb2e298a44300f50b447f9e55b85bbee7d796c1190c80c43eabcb41f5","download_size_bytes":176375,"crc":"passed","member_sha256":{"groundhog.linux/receipt.json":"dc23990fe77fef2a93491af0cb56e7281d1f31660d77e84103309a8de42fb8c8","groundhog.linux/worker.log":"173430c24f2ae508d76f8d596ffe08f3a12825f6f07420e0018ddb2d32374e15"}},
    {"run_id":39194781902,"run_attempt":1,"source_sha":"2c5b18c23344b3a4c32faf2738e6c34064ab1af7","artifact_id":12162865868,"name":"dependency-validation-368f3aa180642f6b60ed07b55731cfaab38a3f0193d8aef9b19148599b40a368-1","expired":false,"created_at":"2026-10-19T04:04:37Z","updated_at":"2026-10-19T04:04:37Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12162865868","metadata_digest":"sha256:8c056684c37f9f2baa39a71639cd1fc7bc71ab6d1c572f2b102ae5c11f99adb2","download_sha256":"8c056684c37f9f2baa39a71639cd1fc7bc71ab6d1c572f2b102ae5c11f99adb2","download_size_bytes":594559,"crc":"passed","member_sha256":{"validation.json":"ecb09e18115d01ffd68bfaf2f5d05e73b30c2884a346a5329f5f66994527860b"}},
    {"run_id":39194781902,"run_attempt":1,"source_sha":"2c5b18c23344b3a4c32faf2738e6c34064ab1af7","artifact_id":12162075104,"name":"dependency-receipt-368f3aa180642f6b60ed07b55731cfaab38a3f0193d8aef9b19148599b40a368-core.linux.asan-1","expired":false,"created_at":"2026-10-19T03:29:32Z","updated_at":"2026-10-19T03:29:32Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12162075104","metadata_digest":"sha256:a3d282226c1cb4efccdd3931b818395f4742ab5a084fefd375918c6b0924a629","download_sha256":"a3d282226c1cb4efccdd3931b818395f4742ab5a084fefd375918c6b0924a629","download_size_bytes":57861,"crc":"passed","member_sha256":{"core.linux.asan/receipt.json":"6598b1269a3e500a4118b43e979839fc5d46183c455c82ac41a7d67814534abe","core.linux.asan/worker.log":"173430c24f2ae508d76f8d596ffe08f3a12825f6f07420e0018ddb2d32374e15"}},
    {"run_id":39194781902,"run_attempt":1,"source_sha":"2c5b18c23344b3a4c32faf2738e6c34064ab1af7","artifact_id":12161920854,"name":"dependency-receipt-368f3aa180642f6b60ed07b55731cfaab38a3f0193d8aef9b19148599b40a368-core.macos.asan-1","expired":false,"created_at":"2026-10-19T03:30:36Z","updated_at":"2026-10-19T03:30:36Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12161920854","metadata_digest":"sha256:24c8f5aed187b1639b45902d5a32caf11752cd9140ffa2f5429f6f7cff8a860d","download_sha256":"24c8f5aed187b1639b45902d5a32caf11752cd9140ffa2f5429f6f7cff8a860d","download_size_bytes":57904,"crc":"passed","member_sha256":{"core.macos.asan/receipt.json":"a7085099ff3cd9c59344d1c70ae81d8e3cd0bdfac111187f60f463f888da5579","core.macos.asan/worker.log":"173430c24f2ae508d76f8d596ffe08f3a12825f6f07420e0018ddb2d32374e15"}},
    {"run_id":39194781902,"run_attempt":1,"source_sha":"2c5b18c23344b3a4c32faf2738e6c34064ab1af7","artifact_id":12161786368,"name":"dependency-receipt-368f3aa180642f6b60ed07b55731cfaab38a3f0193d8aef9b19148599b40a368-grotto.linux.asan_ubsan-1","expired":false,"created_at":"2026-10-19T03:29:39Z","updated_at":"2026-10-19T03:29:39Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12161786368","metadata_digest":"sha256:3bc8a711e2937d1c436192535560314b411edc62221f551db6be85581f956a21","download_sha256":"3bc8a711e2937d1c436192535560314b411edc62221f551db6be85581f956a21","download_size_bytes":96534,"crc":"passed","member_sha256":{"grotto.linux.asan_ubsan/receipt.json":"da8f9de0dc5b9b5e5ff081356647e18f2ba8515e4debbbddca2be0a2d3f7df6b","grotto.linux.asan_ubsan/worker.log":"173430c24f2ae508d76f8d596ffe08f3a12825f6f07420e0018ddb2d32374e15"}},
    {"run_id":39194781902,"run_attempt":1,"source_sha":"2c5b18c23344b3a4c32faf2738e6c34064ab1af7","artifact_id":12161662149,"name":"dependency-receipt-368f3aa180642f6b60ed07b55731cfaab38a3f0193d8aef9b19148599b40a368-groundhog.mdk08-1","expired":false,"created_at":"2026-10-19T03:32:35Z","updated_at":"2026-10-19T03:32:35Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12161662149","metadata_digest":"sha256:099737fc758330b9aaaab114386c71520d16bad47664f4e25e7f2f9e0dc7cd3e","download_sha256":"099737fc758330b9aaaab114386c71520d16bad47664f4e25e7f2f9e0dc7cd3e","download_size_bytes":17597,"crc":"passed","member_sha256":{"groundhog.mdk08/receipt.json":"f97dcef0c221fe3ad888dd9f7f7f60be21950140b8dc6c917f31fc2c9aa64e4b","groundhog.mdk08/worker.log":"c3248197fc46c39acc93fb321661c79fa213f9f3ab8d6d7c86ed0ff9fb1d050e"}},
    {"run_id":39194781902,"run_attempt":1,"source_sha":"2c5b18c23344b3a4c32faf2738e6c34064ab1af7","artifact_id":12161497298,"name":"dependency-receipt-368f3aa180642f6b60ed07b55731cfaab38a3f0193d8aef9b19148599b40a368-static.policy-1","expired":false,"created_at":"2026-10-19T03:38:39Z","updated_at":"2026-10-19T03:38:39Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12161497298","metadata_digest":"sha256:8ec52ac209375adacc784e52892a9409a1d06318089edc451520c19c8a95c8dd","download_sha256":"8ec52ac209375adacc784e52892a9409a1d06318089edc451520c19c8a95c8dd","download_size_bytes":22097,"crc":"passed","member_sha256":{"static.policy/receipt.json":"71f5109f96a99b8cdf8c3578eb099d2c0c1a49830dace58b5d4cded6ed9f9e7d","static.policy/worker.log":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}},
    {"run_id":39194781902,"run_attempt":1,"source_sha":"2c5b18c23344b3a4c32faf2738e6c34064ab1af7","artifact_id":12161389193,"name":"dependency-receipt-368f3aa180642f6b60ed07b55731cfaab38a3f0193d8aef9b19148599b40a368-groundhog.linux.sanitizers-1","expired":false,"created_at":"2026-10-19T03:38:23Z","updated_at":"2026-10-19T03:38:23Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12161389193","metadata_digest":"sha256:919c2eb4d42b808fc59218a19d7e6db6db54538effa3d225c271f9823ffcc7d8","download_sha256":"919c2eb4d42b808fc59218a19d7e6db6db54538effa3d225c271f9823ffcc7d8","download_size_bytes":7378,"crc":"passed","member_sha256":{"groundhog.linux.sanitizers/receipt.json":"3f6cc829fb2c24ee194ed1f1341faa35ca92d9824b984598f93ad82b1658616a","groundhog.linux.sanitizers/worker.log":"389e76c63f8828b5a7a5f6faf9587d06aff79d72a44ecab7151c0eaac496496c"}},
    {"run_id":39194781902,"run_attempt":1,"source_sha":"2c5b18c23344b3a4c32faf2738e6c34064ab1af7","artifact_id":12161267348,"name":"dependency-receipt-368f3aa180642f6b60ed07b55731cfaab38a3f0193d8aef9b19148599b40a368-libgo.macos.asan-1","expired":false,"created_at":"2026-10-19T03:31:00Z","updated_at":"2026-10-19T03:31:00Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12161267348","metadata_digest":"sha256:3c84f1998fe15ae99ac6808f0cd311204af8c4a61b9f32555f42e3f13dd94a44","download_sha256":"3c84f1998fe15ae99ac6808f0cd311204af8c4a61b9f32555f42e3f13dd94a44","download_size_bytes":27750,"crc":"passed","member_sha256":{"libgo.macos.asan/worker.log":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","libgo.macos.asan/receipt.json":"47194e717c59615567a3f40c5098492083c27976a0be8a1d0782936740f6e21b"}},
    {"run_id":39194781902,"run_attempt":1,"source_sha":"2c5b18c23344b3a4c32faf2738e6c34064ab1af7","artifact_id":12161259410,"name":"dependency-receipt-368f3aa180642f6b60ed07b55731cfaab38a3f0193d8aef9b19148599b40a368-libgo.linux.asan-1","expired":false,"created_at":"2026-10-19T03:35:55Z","updated_at":"2026-10-19T03:35:55Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12161259410","metadata_digest":"sha256:011c516852bfdd8be2685b14e603bdd81d1730a1ad54be620a4ae8483ed1d443","download_sha256":"011c516852bfdd8be2685b14e603bdd81d1730a1ad54be620a4ae8483ed1d443","download_size_bytes":24619,"crc":"passed","member_sha256":{"libgo.linux.asan/receipt.json":"f20a6e549a1fd968b82a9a221ac5cf33492cac4b34296e2ab73aa538d96b0b49","libgo.linux.asan/worker.log":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}},
    {"run_id":39194781902,"run_attempt":1,"source_sha":"2c5b18c23344b3a4c32faf2738e6c34064ab1af7","artifact_id":12161214778,"name":"dependency-receipt-368f3aa180642f6b60ed07b55731cfaab38a3f0193d8aef9b19148599b40a368-grotto.linux-1","expired":false,"created_at":"2026-10-19T03:36:55Z","updated_at":"2026-10-19T03:36:55Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12161214778","metadata_digest":"sha256:7a6eaf9ff544014a57bb18bb772bd5a198dd86479c283e04b91f28e9a6ce9968","download_sha256":"7a6eaf9ff544014a57bb18bb772bd5a198dd86479c283e04b91f28e9a6ce9968","download_size_bytes":192920,"crc":"passed","member_sha256":{"grotto.linux/receipt.json":"ec107f16cfa918cd0165d3561492f69d51947d4b1bed562553fadd644261ac2c","grotto.linux/worker.log":"173430c24f2ae508d76f8d596ffe08f3a12825f6f07420e0018ddb2d32374e15"}},
    {"run_id":39194781902,"run_attempt":1,"source_sha":"2c5b18c23344b3a4c32faf2738e6c34064ab1af7","artifact_id":12161167413,"name":"dependency-plan-368f3aa180642f6b60ed07b55731cfaab38a3f0193d8aef9b19148599b40a368-1","expired":false,"created_at":"2026-10-19T03:27:49Z","updated_at":"2026-10-19T03:27:49Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12161167413","metadata_digest":"sha256:76299ca99156b7fc8ce3ade555d094b8e92586f8dd551063aebe018578343fad","download_sha256":"76299ca99156b7fc8ce3ade555d094b8e92586f8dd551063aebe018578343fad","download_size_bytes":7594,"crc":"passed","member_sha256":{"plan.json":"30d026f8ca7ba1c263edbb74684498b523d783f2c1b535dd5f1f458a75460375"}},
    {"run_id":39194781902,"run_attempt":1,"source_sha":"2c5b18c23344b3a4c32faf2738e6c34064ab1af7","artifact_id":12161162961,"name":"dependency-receipt-368f3aa180642f6b60ed07b55731cfaab38a3f0193d8aef9b19148599b40a368-groundhog.mdk011-1","expired":false,"created_at":"2026-10-19T03:34:03Z","updated_at":"2026-10-19T03:34:03Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12161162961","metadata_digest":"sha256:a163496a32c842f8e4125a8d855a6731075ef5e32af6258a78bbe172a651e317","download_sha256":"a163496a32c842f8e4125a8d855a6731075ef5e32af6258a78bbe172a651e317","download_size_bytes":16371,"crc":"passed","member_sha256":{"groundhog.mdk011/receipt.json":"4e99605bbba4337f17002848ff459e7247bb9d0b76908d5a190119219538b9d3","groundhog.mdk011/worker.log":"c3248197fc46c39acc93fb321661c79fa213f9f3ab8d6d7c86ed0ff9fb1d050e"}},
    {"run_id":39194781902,"run_attempt":1,"source_sha":"2c5b18c23344b3a4c32faf2738e6c34064ab1af7","artifact_id":12161152656,"name":"dependency-receipt-368f3aa180642f6b60ed07b55731cfaab38a3f0193d8aef9b19148599b40a368-core.linux-1","expired":false,"created_at":"2026-10-19T03:29:35Z","updated_at":"2026-10-19T03:29:35Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12161152656","metadata_digest":"sha256:6ce38875cc72169731aabdd4e3734003cd1dbf4484c2e54b1eae89d721979331","download_sha256":"6ce38875cc72169731aabdd4e3734003cd1dbf4484c2e54b1eae89d721979331","download_size_bytes":100069,"crc":"passed","member_sha256":{"core.linux/receipt.json":"d079fe0c5ee542500752607dc76baa8d61d4be06653e56d5d839935a572d2eda","core.linux/worker.log":"173430c24f2ae508d76f8d596ffe08f3a12825f6f07420e0018ddb2d32374e15"}},
    {"run_id":39194781902,"run_attempt":1,"source_sha":"2c5b18c23344b3a4c32faf2738e6c34064ab1af7","artifact_id":12161134740,"name":"dependency-receipt-368f3aa180642f6b60ed07b55731cfaab38a3f0193d8aef9b19148599b40a368-groundhog.linux-1","expired":false,"created_at":"2026-10-19T03:34:45Z","updated_at":"2026-10-19T03:34:45Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12161134740","metadata_digest":"sha256:6669ca0191e90480756f263ff7446afaa3e09fa13d929262fc276000a1e63a74","download_sha256":"6669ca0191e90480756f263ff7446afaa3e09fa13d929262fc276000a1e63a74","download_size_bytes":176334,"crc":"passed","member_sha256":{"groundhog.linux/receipt.json":"a7d4cc8cd85e55d39fc979a711c9bc34fa39ec4bee862405e1f06059d4808220","groundhog.linux/worker.log":"173430c24f2ae508d76f8d596ffe08f3a12825f6f07420e0018ddb2d32374e15"}},
    {"run_id":39194059430,"run_attempt":1,"source_sha":"5280ea324ec474e9d0644638565a2e5a01748b51","artifact_id":12162288486,"name":"dependency-validation-fccd676fa5ec6c3fbdb78fa3a889aed6dba0923d8ea00333cfa1cae35974ceb4-1","expired":false,"created_at":"2026-10-19T04:04:20Z","updated_at":"2026-10-19T04:04:20Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12162288486","metadata_digest":"sha256:b1fe11ff08aa9233bf605a11cb789d5abdfdaf37c5a5120645375d901097109f","download_sha256":"b1fe11ff08aa9233bf605a11cb789d5abdfdaf37c5a5120645375d901097109f","download_size_bytes":510510,"crc":"passed","member_sha256":{"validation.json":"4bc1ff4a9ed96d4febf3124e97af8b0769e301bd58cb278f96501ecd78924df3"}},
    {"run_id":39194059430,"run_attempt":1,"source_sha":"5280ea324ec474e9d0644638565a2e5a01748b51","artifact_id":12161630431,"name":"dependency-receipt-fccd676fa5ec6c3fbdb78fa3a889aed6dba0923d8ea00333cfa1cae35974ceb4-core.macos.asan-1","expired":false,"created_at":"2026-10-19T03:20:09Z","updated_at":"2026-10-19T03:20:09Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12161630431","metadata_digest":"sha256:d723182ae914630beec2e2bda96ee6995b7c5ee396b4bd31d2fb488e8df38c51","download_sha256":"d723182ae914630beec2e2bda96ee6995b7c5ee396b4bd31d2fb488e8df38c51","download_size_bytes":57896,"crc":"passed","member_sha256":{"core.macos.asan/receipt.json":"d47aad6a303dd45e4d488978c9e766e5c0b9bf1874a4078a4daf69da6588ff24","core.macos.asan/worker.log":"173430c24f2ae508d76f8d596ffe08f3a12825f6f07420e0018ddb2d32374e15"}},
    {"run_id":39194059430,"run_attempt":1,"source_sha":"5280ea324ec474e9d0644638565a2e5a01748b51","artifact_id":12161580189,"name":"dependency-receipt-fccd676fa5ec6c3fbdb78fa3a889aed6dba0923d8ea00333cfa1cae35974ceb4-grotto.linux.asan_ubsan-1","expired":false,"created_at":"2026-10-19T03:20:50Z","updated_at":"2026-10-19T03:20:50Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12161580189","metadata_digest":"sha256:63458e71c33e9624a42303b21ba3a8cbc890a363d4b60510f7d97af18a4af555","download_sha256":"63458e71c33e9624a42303b21ba3a8cbc890a363d4b60510f7d97af18a4af555","download_size_bytes":96527,"crc":"passed","member_sha256":{"grotto.linux.asan_ubsan/receipt.json":"1051a3275aef3a6690f79a49ab77451dec0d4b8c40fa2131f63afbaa5c36253a","grotto.linux.asan_ubsan/worker.log":"173430c24f2ae508d76f8d596ffe08f3a12825f6f07420e0018ddb2d32374e15"}},
    {"run_id":39194059430,"run_attempt":1,"source_sha":"5280ea324ec474e9d0644638565a2e5a01748b51","artifact_id":12161555809,"name":"dependency-receipt-fccd676fa5ec6c3fbdb78fa3a889aed6dba0923d8ea00333cfa1cae35974ceb4-core.linux-1","expired":false,"created_at":"2026-10-19T03:20:38Z","updated_at":"2026-10-19T03:20:38Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12161555809","metadata_digest":"sha256:b060a5ff9fb43e459459b91554d83da46a5d05734457253bff8e8924b1520479","download_sha256":"b060a5ff9fb43e459459b91554d83da46a5d05734457253bff8e8924b1520479","download_size_bytes":5623,"crc":"passed","member_sha256":{"core.linux/receipt.json":"c645fede602ff7a6863ba66a9ae1b0009451f72b19b227df63356126b78c9141","core.linux/worker.log":"2709cb6f0bb486e513f02d6fb2b573e9f9885baa3268e8daf66e72594d85fe5f"}},
    {"run_id":39194059430,"run_attempt":1,"source_sha":"5280ea324ec474e9d0644638565a2e5a01748b51","artifact_id":12161446081,"name":"dependency-receipt-fccd676fa5ec6c3fbdb78fa3a889aed6dba0923d8ea00333cfa1cae35974ceb4-libgo.linux.asan-1","expired":false,"created_at":"2026-10-19T03:20:14Z","updated_at":"2026-10-19T03:20:14Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12161446081","metadata_digest":"sha256:753f7176d918c4c74f4eaf779e901b1ca6d163e7177e4aa7e936cc570dff38b3","download_sha256":"753f7176d918c4c74f4eaf779e901b1ca6d163e7177e4aa7e936cc570dff38b3","download_size_bytes":24599,"crc":"passed","member_sha256":{"libgo.linux.asan/receipt.json":"36d4fe1c50eae2a88114adb6f6c34d843bdac96414811a314ba37da50da426b6","libgo.linux.asan/worker.log":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}},
    {"run_id":39194059430,"run_attempt":1,"source_sha":"5280ea324ec474e9d0644638565a2e5a01748b51","artifact_id":12161425599,"name":"dependency-receipt-fccd676fa5ec6c3fbdb78fa3a889aed6dba0923d8ea00333cfa1cae35974ceb4-groundhog.linux-1","expired":false,"created_at":"2026-10-19T03:20:32Z","updated_at":"2026-10-19T03:20:32Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12161425599","metadata_digest":"sha256:bf5e9275147d1f53bad36f24bf0784e3fbdcebdc216dce40f27708ccb3b4baff","download_sha256":"bf5e9275147d1f53bad36f24bf0784e3fbdcebdc216dce40f27708ccb3b4baff","download_size_bytes":176327,"crc":"passed","member_sha256":{"groundhog.linux/receipt.json":"0b01f5f5af211dec0e39960e41e57d8af91ecc0a2229ee12b01c2395ec6d1f5a","groundhog.linux/worker.log":"173430c24f2ae508d76f8d596ffe08f3a12825f6f07420e0018ddb2d32374e15"}},
    {"run_id":39194059430,"run_attempt":1,"source_sha":"5280ea324ec474e9d0644638565a2e5a01748b51","artifact_id":12161297425,"name":"dependency-receipt-fccd676fa5ec6c3fbdb78fa3a889aed6dba0923d8ea00333cfa1cae35974ceb4-static.policy-1","expired":false,"created_at":"2026-10-19T03:21:56Z","updated_at":"2026-10-19T03:21:56Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12161297425","metadata_digest":"sha256:930d7af63987239e4ea478ac7aa2891962abb5dfcb8a5b67c5bcb3c7bf98ef1c","download_sha256":"930d7af63987239e4ea478ac7aa2891962abb5dfcb8a5b67c5bcb3c7bf98ef1c","download_size_bytes":22094,"crc":"passed","member_sha256":{"static.policy/receipt.json":"3b2edc3d4a903b3876df7822c4ad41da418c50a16d07a8ac3354e6d3ab62a063","static.policy/worker.log":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}},
    {"run_id":39194059430,"run_attempt":1,"source_sha":"5280ea324ec474e9d0644638565a2e5a01748b51","artifact_id":12161287274,"name":"dependency-receipt-fccd676fa5ec6c3fbdb78fa3a889aed6dba0923d8ea00333cfa1cae35974ceb4-grotto.linux-1","expired":false,"created_at":"2026-10-19T03:20:32Z","updated_at":"2026-10-19T03:20:32Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12161287274","metadata_digest":"sha256:b76fe9e8cec79e50efc6c94e4b42773161115b8ffc04badb7a110e48abf6a7b9","download_sha256":"b76fe9e8cec79e50efc6c94e4b42773161115b8ffc04badb7a110e48abf6a7b9","download_size_bytes":192914,"crc":"passed","member_sha256":{"grotto.linux/receipt.json":"bf3bcd065fb30ae1803bd35c9c295f43731f4d00294209c15acc082a46aacefb","grotto.linux/worker.log":"173430c24f2ae508d76f8d596ffe08f3a12825f6f07420e0018ddb2d32374e15"}},
    {"run_id":39194059430,"run_attempt":1,"source_sha":"5280ea324ec474e9d0644638565a2e5a01748b51","artifact_id":12161151680,"name":"dependency-receipt-fccd676fa5ec6c3fbdb78fa3a889aed6dba0923d8ea00333cfa1cae35974ceb4-groundhog.mdk011-1","expired":false,"created_at":"2026-10-19T03:20:37Z","updated_at":"2026-10-19T03:20:37Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12161151680","metadata_digest":"sha256:0fdd1ba728ecef409999779398ed122cd10a04a92df7a4f548f06a1ffd172d58","download_sha256":"0fdd1ba728ecef409999779398ed122cd10a04a92df7a4f548f06a1ffd172d58","download_size_bytes":16372,"crc":"passed","member_sha256":{"groundhog.mdk011/receipt.json":"dfca02b2ed2621a86d2409ce3a64c979b321afea2279ac61dfbbca085af8cf49","groundhog.mdk011/worker.log":"c3248197fc46c39acc93fb321661c79fa213f9f3ab8d6d7c86ed0ff9fb1d050e"}},
    {"run_id":39194059430,"run_attempt":1,"source_sha":"5280ea324ec474e9d0644638565a2e5a01748b51","artifact_id":12160928529,"name":"dependency-plan-fccd676fa5ec6c3fbdb78fa3a889aed6dba0923d8ea00333cfa1cae35974ceb4-1","expired":false,"created_at":"2026-10-19T03:18:36Z","updated_at":"2026-10-19T03:18:36Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12160928529","metadata_digest":"sha256:676a5184a703c8a623e56c0be7d130d8ded8cc95c00e17062cac988d71d0b4c6","download_sha256":"676a5184a703c8a623e56c0be7d130d8ded8cc95c00e17062cac988d71d0b4c6","download_size_bytes":7626,"crc":"passed","member_sha256":{"plan.json":"84d473578707be79f9194fa50ce0031f565233cdcc3490c5029766afe2749e94"}},
    {"run_id":39194059430,"run_attempt":1,"source_sha":"5280ea324ec474e9d0644638565a2e5a01748b51","artifact_id":12160903749,"name":"dependency-receipt-fccd676fa5ec6c3fbdb78fa3a889aed6dba0923d8ea00333cfa1cae35974ceb4-groundhog.linux.sanitizers-1","expired":false,"created_at":"2026-10-19T03:20:21Z","updated_at":"2026-10-19T03:20:21Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12160903749","metadata_digest":"sha256:e5ca2098b29e00c51853376709295281124ebed9d6004d110b04a4628b3602dd","download_sha256":"e5ca2098b29e00c51853376709295281124ebed9d6004d110b04a4628b3602dd","download_size_bytes":7371,"crc":"passed","member_sha256":{"groundhog.linux.sanitizers/receipt.json":"a76cefd789dc2d5374e46eaa0828bd9b0b854e48606add6109ad70337b25a551","groundhog.linux.sanitizers/worker.log":"389e76c63f8828b5a7a5f6faf9587d06aff79d72a44ecab7151c0eaac496496c"}},
    {"run_id":39194059430,"run_attempt":1,"source_sha":"5280ea324ec474e9d0644638565a2e5a01748b51","artifact_id":12160897422,"name":"dependency-receipt-fccd676fa5ec6c3fbdb78fa3a889aed6dba0923d8ea00333cfa1cae35974ceb4-groundhog.mdk08-1","expired":false,"created_at":"2026-10-19T03:20:27Z","updated_at":"2026-10-19T03:20:27Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12160897422","metadata_digest":"sha256:d6cd5143dde5117d77eeb9c37a696e89baa4d8ed27f5c7ce030e2b47ee4dd18b","download_sha256":"d6cd5143dde5117d77eeb9c37a696e89baa4d8ed27f5c7ce030e2b47ee4dd18b","download_size_bytes":5580,"crc":"passed","member_sha256":{"groundhog.mdk08/receipt.json":"85e8c212c2362d863c39a3310e412d9a48f7e455e03d2acd610a7af833f12d5e","groundhog.mdk08/worker.log":"2709cb6f0bb486e513f02d6fb2b573e9f9885baa3268e8daf66e72594d85fe5f"}},
    {"run_id":39194059430,"run_attempt":1,"source_sha":"5280ea324ec474e9d0644638565a2e5a01748b51","artifact_id":12160774441,"name":"dependency-receipt-fccd676fa5ec6c3fbdb78fa3a889aed6dba0923d8ea00333cfa1cae35974ceb4-core.linux.asan-1","expired":false,"created_at":"2026-10-19T03:20:54Z","updated_at":"2026-10-19T03:20:54Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12160774441","metadata_digest":"sha256:04cbed86ee9d1deb09d0f0c96a25d126451b79024d76fcd67e2d2be73b6571f3","download_sha256":"04cbed86ee9d1deb09d0f0c96a25d126451b79024d76fcd67e2d2be73b6571f3","download_size_bytes":57851,"crc":"passed","member_sha256":{"core.linux.asan/receipt.json":"6bd807510502250903c3cef7708dfe79a2b5c650e93138f5bc84ddac4bcecc84","core.linux.asan/worker.log":"173430c24f2ae508d76f8d596ffe08f3a12825f6f07420e0018ddb2d32374e15"}},
    {"run_id":39194059430,"run_attempt":1,"source_sha":"5280ea324ec474e9d0644638565a2e5a01748b51","artifact_id":12160754395,"name":"dependency-receipt-fccd676fa5ec6c3fbdb78fa3a889aed6dba0923d8ea00333cfa1cae35974ceb4-libgo.macos.asan-1","expired":false,"created_at":"2026-10-19T03:20:02Z","updated_at":"2026-10-19T03:20:02Z","api_url":"https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12160754395","metadata_digest":"sha256:481e8d659c3a57cef8102ffbcb35f983d7b1131da472645159222e3d60a5616e","download_sha256":"481e8d659c3a57cef8102ffbcb35f983d7b1131da472645159222e3d60a5616e","download_size_bytes":27748,"crc":"passed","member_sha256":{"libgo.macos.asan/receipt.json":"e47d8a5bb7816fdf84467a0bd0f01aeee46c144c816222df82f22866088bb132","libgo.macos.asan/worker.log":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}}
  ],
  "validation_scripts": [{"path":"/tmp/nostrc-acceptance-refresh-20261019/capture.py","sha256":"0391e774f2c115c6264c5a80efaa653ed612ce6cb43e483996cd56515d192f68"},{"path":"/tmp/nostrc-acceptance-refresh-20261019/validate_artifacts.py","sha256":"1df363933bde339a06365894945e7b26c43c80ac9266ff590d23d9452304a8f2"},{"path":"/tmp/nostrc-acceptance-refresh-20261019/build_refresh.py","sha256":"b23fdaa54411b1ff261db04e0d60651718be9603dc061ff2183c80a41ffc2334"},{"path":"/tmp/nostrc-acceptance-refresh-20261019/verify_refresh.py","sha256":"290e8c974bde9529777190123e0c3921e014c15b13eb6ee6100518ce9d22f682"}],
  "validation_results": [{"path":"/tmp/nostrc-acceptance-refresh-20261019/capture-summary.json","sha256":"3a38433e18a131528ea554aec39e7eb72aee25384e8709a226b21a3a4e89faec"},{"path":"/tmp/nostrc-acceptance-refresh-20261019/queries.json","sha256":"b170d1bbb3acdb9dbd5a70f93cd6fe2133e915878c16ea08584ebeeb85861001"},{"path":"/tmp/nostrc-acceptance-refresh-20261019/artifact-validation-result.json","sha256":"567109bddf275521d344e8f2e2bdcc714837ca476dced6e198d30e3df6ade8d6"},{"path":"/tmp/nostrc-acceptance-refresh-20261019/artifact-retrieval-index.json","sha256":"b6315f78bde94c5d41b5abae90ac217fa0e2a93a6b5915e1b017e47264de295b"},{"path":"/tmp/nostrc-acceptance-refresh-20261019/workflow-source-index.json","sha256":"071e0ab2a68e1c5d75ba00e3250a9ee92beddeeac1595e43884e57d5f7e2ee83"},{"path":"/tmp/nostrc-acceptance-refresh-20261019/artifact-validation.log","sha256":"1651102766c088d8b2ab79323d3d820e96c59a988a07a246901b1a4356821f1c"},{"path":"/tmp/nostrc-acceptance-refresh-20261019/artifact-validation-final.log","sha256":"377dedb0b6b89bbd90506722e3241ae02a0bad9d5dfa08bcf16b9d6b58bc2011"}]
}
```
<!-- acceptance-ledger-refresh-json:end -->

## Historical snapshot retained — 2026-10-19T03:01:13.808922Z

The following entire original body is historical, including its “current” and
“this snapshot” wording. It retains reconciliation time
`2026-10-19T03:11:57.005478Z`, every original failed/cancelled attempt, local
receipt, hash and review protocol; the new current snapshot above supersedes
only its live-counter interpretation. No original result has been erased.


**Versioned evidence snapshot, not a live dashboard.** Machine-readable normalized data is the JSON fence below; counters are derived from its accepted dispositions. Beads remains the task tracker.

## Count at this snapshot

- **0/20 qualifying PR comparisons**: no PR-triggered shadow run, and no PR-triggered repository run in the queried window.
- **0/2 qualifying broad integrations**: five nested broad accounting jobs failed; one selection failure prevented broad launch.
- **6 shadow attempts**: five failed pushes and one canceled manual run. The canceled parent still retained a failed nested broad accounting job.
- **59 small plan/receipt/aggregate artifacts** were retrieved and checked; installer binaries were not downloaded or executed.

See [criterion audit](dependency-aware-testing-ci-2026-10-18-acceptance-audit.md) for bead decisions and concrete blockers. This replaces assumed counters with source-linked observations, not approval to change policy.

## Backfilled root runs

| Run / attempt | Source | Event | Parent result | PR disposition | Nested broad accounting |
|---|---|---|---|---|---|
| [38833746063/1](https://github.com/chebizarro/nostrc/actions/runs/38833746063) | `5467c02d1` | push | failure | not a PR | [failed](https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116770701220) |
| [38845886339/1](https://github.com/chebizarro/nostrc/actions/runs/38845886339) | `33a101276` | push | failure | not a PR | not launched |
| [38856570439/1](https://github.com/chebizarro/nostrc/actions/runs/38856570439) | `8a031f443` | push | failure | not a PR | [failed](https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116841163905) |
| [38873978927/1](https://github.com/chebizarro/nostrc/actions/runs/38873978927) | `a47807050` | workflow_dispatch | cancelled | not a PR | [failed](https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116900093654) |
| [38878691465/1](https://github.com/chebizarro/nostrc/actions/runs/38878691465) | `249f9657f` | push | failure | not a PR | [failed](https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116915712972) |
| [39132650260/1](https://github.com/chebizarro/nostrc/actions/runs/39132650260) | `2743f1735` | push | failure | not a PR | [failed](https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117700034296) |

The reusable broad/worker/release workflows have zero standalone run records. That does **not** mean broad was never executed: five actual nested broad accounting jobs are captured above. No broad result is counted solely from a successful parent or a subset of jobs.

## Refresh and review protocol

1. Fetch the four workflow histories with pagination, repository context, every candidate attempt’s jobs and artifacts (enumerate1..N when run_attempt=N). Verify returned totals and source identities; record retrieval failures/expired artifacts rather than assuming success.
2. Reconcile new run IDs and attempts with this snapshot. Preserve earlier failures/cancellations when reruns exist; bind each artifact to its attempt/source/plan identity and never reuse an earlier receipt as a later attempt. Deduplicate PR numbers and root integrations, not artifacts.
3. Inspect source-bound plans, actual worker conclusions, normalized receipts and broad accounting. Review each legacy/selective difference; record accepted, failed, blocked, ineligible or unreviewed explicitly. No automatic PR acceptance from status color.
4. Derive counters from explicit accepted entries and check them against the displayed summary. Publish a new timestamped snapshot and update `.5` before reporting W11 progress or closing scope. A documentation push may launch a later run; it is outside the frozen snapshot, not silently counted or a reason for recursive ledger commits.
5. Capture missing evidence, measurements or remedies in beads. Never weaken existing admission, skips, sanitizer, cleanup or release rules to make a counter advance.

Read-only discovery used here (replace IDs/attempts from returned metadata; `gh` supplies existing authentication):

```sh
gh api --paginate --slurp "repos/chebizarro/nostrc/actions/workflows/386168204/runs?per_page=100"
gh api --paginate --slurp "repos/chebizarro/nostrc/actions/workflows/386168202/runs?per_page=100"
gh api --paginate --slurp "repos/chebizarro/nostrc/actions/workflows/386168205/runs?per_page=100"
gh api --paginate --slurp "repos/chebizarro/nostrc/actions/workflows/386168209/runs?per_page=100"
gh api --method GET --paginate --slurp repos/chebizarro/nostrc/actions/runs \
  -f per_page=100 -f "created=>=2026-10-15T00:00:00Z"
gh api --paginate --slurp "repos/chebizarro/nostrc/actions/runs/RUN_ID/attempts/ATTEMPT/jobs?per_page=100"
gh api --paginate --slurp "repos/chebizarro/nostrc/actions/runs/RUN_ID/artifacts?per_page=100"
```

The data is a reviewed backfill, not an implemented CI auto-updater. Refresh is owned by the migration session under `.5`; the owner is not expected to tally runs manually. Raw local files are not a permanent artifact store.

## Canonical data

<!-- acceptance-ledger-json:start -->
```json
{
  "schema_version": 1,
  "audit_source_sha": "2743f17354ac13527d8d8919f81abc711396c959",
  "snapshot_recorded_at": "2026-10-19T03:01:13.808922Z",
  "reconciliation_updated_at": "2026-10-19T03:11:57.005478Z",
  "repository": "chebizarro/nostrc",
  "scope": {
    "workflow_ids": [
      386168204,
      386168202,
      386168205,
      386168209
    ],
    "workflow_history": "All API-visible runs for these workflow IDs, every returned attempt=1. No claim to recover deleted/inaccessible history.",
    "repository_context_since": "2026-10-15T00:00:00Z",
    "repository_context_returned": 146,
    "repository_context_pr_run_count": 0,
    "standalone_reusable_history_is_not_execution_count": true,
    "candidate_keys": "workflow run_id plus run_attempt; nested jobs and duplicate artifacts are not separate root runs.",
    "future_runs": "Snapshot only; later runs, reruns, expirations and policy changes require refresh before acceptance claims.",
    "artifacts": "59 small plan/receipt/aggregate archives downloaded, CRC-checked and member-hashed. No application installers or source archives executed.",
    "raw_capture_directory": "/tmp/nostrc-acceptance-audit-20261018",
    "durability": "Curated metadata/verdicts/hashes below are committed. Raw captures are local and GitHub artifacts expire; do not claim raw bytes remain available after retention. No qualifying W11 entry is inferred from unavailable evidence."
  },
  "qualification_policy": {
    "pr": "Distinct representative PR with source/range-bound legacy and selective evidence, reviewed difference classification and regression for any legacy-only failure. A green run alone is insufficient; pushes/manual runs and local test cases are not PRs.",
    "broad": "Complete successful full broad family accounting and reviewed required-workload/receipt union at exact source; advisory distinctions remain explicit. Root-run deduplication prevents counting nested jobs/artifacts as integrations.",
    "counter_rule": "Counters are derived only from explicit accepted dispositions, not status colors, candidate count, unit test counts or complete-request flags.",
    "cutover": "Unchanged W11 evidence plus operator approval; W12 is not authorized by this audit."
  },
  "counts": {
    "qualifying_pr_comparisons": 0,
    "target_pr_comparisons": 20,
    "qualifying_broad_integrations": 0,
    "target_broad_integrations": 2,
    "shadow_attempts": 6,
    "shadow_failed": 5,
    "shadow_cancelled": 1,
    "broad_accounting_failed": 5,
    "broad_not_launched": 1,
    "artifacts_retrieved": 59
  },
  "local_test_evidence": {
    "source_sha": "2743f17354ac13527d8d8919f81abc711396c959",
    "command": "PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests/selection -v",
    "exit_code": 0,
    "tests": 437,
    "skips": 0,
    "failures": 0,
    "duration_seconds": 151.176,
    "raw_log_sha256": "70431a57f103923e3936ec40aa5ce541ad4225e82069dfc0b14ef6ddfcfee65e",
    "test_result_index_sha256": "8ab3ce4e1dad1fd1ebdc832b0c2b95419ba1e6bdcdf0852309c26527833924c3",
    "module_counts": {
      "test_aggregation": 16,
      "test_bp_metrics_reporting": 7,
      "test_broad_setup_contracts": 11,
      "test_cmake_inventory_compat": 6,
      "test_command_execution": 16,
      "test_command_output_bytes": 5,
      "test_command_signal_bytes": 3,
      "test_contained_custody_contract": 20,
      "test_diagnostic_eligibility": 8,
      "test_diagnostic_nonmembers": 14,
      "test_execution_parallelism": 24,
      "test_failure_receipts": 17,
      "test_git_ranges": 20,
      "test_grotto_profiles": 7,
      "test_local_command_adapter": 8,
      "test_mdk_cleanup": 3,
      "test_mdk_detached_cleanup": 4,
      "test_mdk_gate": 9,
      "test_nonmember_ownership": 7,
      "test_phase_environments": 21,
      "test_prepared_profile_contract": 6,
      "test_production_inventory": 8,
      "test_profile_contracts": 16,
      "test_realization": 16,
      "test_realization_exclusions": 7,
      "test_realization_inputs": 8,
      "test_realization_run": 16,
      "test_realization_source_identity": 9,
      "test_realization_support_inputs": 8,
      "test_realization_toolchains": 7,
      "test_release_plan": 12,
      "test_resource_profile_contracts": 4,
      "test_sanitizer_adapter": 9,
      "test_sanitizer_route_refusal": 2,
      "test_selection_command_contract": 11,
      "test_selector": 20,
      "test_selector_checkout_binding": 5,
      "test_selector_cli": 6,
      "test_shared_sanitizer_execution": 5,
      "test_workflow_contracts": 36
    },
    "source_status_after": "clean; exact source unchanged",
    "not_a_pr_or_broad_integration": true,
    "additional_gates": [
      {
        "name": "test-pre-push",
        "exit_code": 0,
        "log_sha256": "4b245ea79ada5162499e10be384b44a2b07f8dce77dd9a6ff9f31a70a722f80d"
      },
      {
        "name": "test-full-gate",
        "exit_code": 0,
        "log_sha256": "db4a542881d058bc3f1faf7f31c699b73a1786ec28ace942b839d51d6715f313"
      },
      {
        "name": "test-linux-gate-lifecycle",
        "exit_code": 0,
        "log_sha256": "ebad66e7a7b87c734ac1f9097bb661c11de80826049c74f670899a823d1ddc59"
      },
      {
        "name": "test-linux-gate-smoke",
        "exit_code": 0,
        "log_sha256": "03b09043fcd7392847932e90a9d9de0e4cb098be95979bae569ba966e47437da"
      },
      {
        "name": "test-sanitizer-gate",
        "exit_code": 0,
        "log_sha256": "df98c5f59b618bdac5e2a63e95cb5209a27a94ac00847cc18a1d5e2c3582c463"
      },
      {
        "name": "test-test-gate",
        "exit_code": 0,
        "log_sha256": "9677bca9f8c14b5572966291d629d8221c3741e774bc2a4d7f0ce814384b9ba4"
      },
      {
        "name": "check-unsequenced-args",
        "exit_code": 0,
        "log_sha256": "ae5d6a6c404c1c31aa46e4d3dcbe582ed6b857ecf0bce2148e48490070ad84ea"
      },
      {
        "name": "check-gui-test-gdk-frame",
        "exit_code": 0,
        "log_sha256": "55b03052fb1a38d28bb8dc76a3e2fe0b5c3c0f81c9e767fc2323801d3c9fe51a"
      },
      {
        "name": "check-linux-ci-packages",
        "exit_code": 0,
        "log_sha256": "83b776b6d4940082be5adfe8d6c138d546cf9fc49633c40e330d430430a956aa"
      },
      {
        "name": "check-privacy",
        "exit_code": 0,
        "log_sha256": "ca0cdaf0baa46808b995e23cdff13c12cd03fd213c8944bd2a5ca5e4c9b04250"
      },
      {
        "name": "actionlint",
        "exit_code": 0,
        "log_sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
      }
    ],
    "independent_release_identity_probe": {
      "observed_at": "2026-10-19T03:05:50.248704Z",
      "result_sha256": "ec147bf977b2846f857720b635fabdef65328c83be45e33a5549198fad8523c9",
      "result": {
        "source_sha": "2743f17354ac13527d8d8919f81abc711396c959",
        "scope": "local synthetic validation contract only; no publication or hosted acceptance",
        "all_success_package_map": {
          "linux.aarch64": "success",
          "linux.x86_64": "success",
          "source": "success"
        },
        "baseline_publish": true,
        "independent_refusals": [
          {
            "field": "tested_sha",
            "publish": false,
            "passed": false,
            "errors": [
              "missing/mismatched broad exact-tag validation"
            ]
          },
          {
            "field": "tag",
            "publish": false,
            "passed": false,
            "errors": [
              "missing/mismatched broad exact-tag validation"
            ]
          },
          {
            "field": "mode",
            "publish": false,
            "passed": false,
            "errors": [
              "missing/mismatched broad exact-tag validation"
            ]
          },
          {
            "field": "inventory_hash",
            "publish": false,
            "passed": false,
            "errors": [
              "missing/mismatched broad exact-tag validation"
            ]
          },
          {
            "field": "passed",
            "publish": false,
            "passed": false,
            "errors": [
              "missing/mismatched broad exact-tag validation"
            ]
          },
          {
            "field": "partial_profiles",
            "publish": false,
            "passed": false,
            "errors": [
              "missing/mismatched broad exact-tag validation"
            ]
          }
        ]
      }
    }
  },
  "operational_snapshot": {
    "branch": {
      "api_path": "repos/chebizarro/nostrc/branches/master",
      "exit_code": 0,
      "response_sha256": "454e63631500a2f8ac91cb61bbbabfdef600b6efc4f20d80f5be332952fa5e13",
      "observed": {
        "protected": false
      }
    },
    "classic-protection": {
      "api_path": "repos/chebizarro/nostrc/branches/master/protection",
      "exit_code": 1,
      "response_sha256": "3f6832090f9cd73706e68d317cc0c2f6ef7bb4a3b48c31bde797e69d525ecb21",
      "observed": {
        "message": "Branch not protected",
        "status": "404"
      }
    },
    "effective-rules": {
      "api_path": "repos/chebizarro/nostrc/rules/branches/master",
      "exit_code": 0,
      "response_sha256": "8b536427324a6fd78cbd4c401a29577dcdf00f4619b6da8252bc0d0e5ac2f3f7",
      "observed": []
    },
    "rulesets": {
      "api_path": "repos/chebizarro/nostrc/rulesets?includes_parents=true",
      "exit_code": 0,
      "response_sha256": "d0bfad170e956e8a7e10603b52fef6e7d28236e7c64003fae0ee5c01068202cc",
      "observed": []
    },
    "release-mode": {
      "api_path": "repos/chebizarro/nostrc/actions/variables/NOSTRC_DEPENDENCY_RELEASE_MODE",
      "exit_code": 1,
      "response_sha256": "0405f0a57fcfc0469e66af3eac338aa52a9fbee0891339f8605ff931dc6fba2a",
      "observed": {
        "message": "Not Found",
        "status": "404"
      }
    },
    "repository_access": {
      "api_path": "repos/chebizarro/nostrc",
      "exit_code": 0,
      "captured_at": "2026-10-19T02:46:57.874720Z",
      "response_sha256": "99622e046818403a3d581a172c54ac4524d0aa02deb0e0674c2bae926b65060c",
      "observed": {
        "default_branch": "master",
        "private": false,
        "permissions": {
          "admin": true,
          "maintain": true,
          "push": true,
          "triage": true,
          "pull": true
        },
        "allow_auto_merge": false
      }
    }
  },
  "queries": [
    {
      "role": "shadow",
      "api_path": "repos/chebizarro/nostrc/actions/workflows/386168204/runs?per_page=100",
      "pages": 1,
      "reported_total": 6,
      "returned": 6,
      "response_sha256": "2ecd29261aea76fa792113ca4b62461cae8e4b140bdaa0bcee9d4110553c8fee",
      "captured_at": "2026-10-19T02:42:40.004579Z"
    },
    {
      "role": "broad",
      "api_path": "repos/chebizarro/nostrc/actions/workflows/386168202/runs?per_page=100",
      "pages": 1,
      "reported_total": 0,
      "returned": 0,
      "response_sha256": "f2aa3f2f9ea19c33f16347ee49859287a9b12cde87a89f7d6927ecb020c6b673",
      "captured_at": "2026-10-19T02:42:40.354859Z"
    },
    {
      "role": "worker",
      "api_path": "repos/chebizarro/nostrc/actions/workflows/386168205/runs?per_page=100",
      "pages": 1,
      "reported_total": 0,
      "returned": 0,
      "response_sha256": "f2aa3f2f9ea19c33f16347ee49859287a9b12cde87a89f7d6927ecb020c6b673",
      "captured_at": "2026-10-19T02:42:40.739187Z"
    },
    {
      "role": "release",
      "api_path": "repos/chebizarro/nostrc/actions/workflows/386168209/runs?per_page=100",
      "pages": 1,
      "reported_total": 0,
      "returned": 0,
      "response_sha256": "f2aa3f2f9ea19c33f16347ee49859287a9b12cde87a89f7d6927ecb020c6b673",
      "captured_at": "2026-10-19T02:42:41.058872Z"
    },
    {
      "role": "repository_window",
      "api_path": "repos/chebizarro/nostrc/actions/runs?per_page=100&created=>=2026-10-15T00:00:00Z",
      "pages": 2,
      "reported_total": 146,
      "returned": 146,
      "response_sha256": "abe44675c168b0678eabd06467c54317ec7bdc2cb6d690997202ff577ed0d39d",
      "captured_at": "2026-10-19T02:50:31.078002Z",
      "event_counts": {
        "push": 133,
        "schedule": 9,
        "workflow_dispatch": 4
      }
    },
    {
      "role": "38833746063-attempt1-jobs",
      "pages": 1,
      "reported_total": 56,
      "returned": 56,
      "response_sha256": "563f0b90430297ad0c5ce2f3c0188052994ad0cae388dbfb9876283e11e82d3e",
      "captured_at": "2026-10-19T02:44:12.051991Z"
    },
    {
      "role": "38833746063-artifacts",
      "pages": 1,
      "reported_total": 24,
      "returned": 24,
      "response_sha256": "eedb8b69ce4fabbff85bb9e0fe8bf9ec2a8ac692d861c18342e18f855f6f6681",
      "captured_at": "2026-10-19T02:44:07.449968Z"
    },
    {
      "role": "38833746063-same-sha-runs",
      "pages": 1,
      "reported_total": 13,
      "returned": 13,
      "response_sha256": "4eceb4131ca24b818fc663866cb172c261274441ac041358764351015d267655",
      "captured_at": "2026-10-19T02:44:08.019976Z"
    },
    {
      "role": "38845886339-attempt1-jobs",
      "pages": 1,
      "reported_total": 10,
      "returned": 10,
      "response_sha256": "08f567f62918431d8db119e9d899ccc25e0254e10c81641f25fe091e5fa66a5f",
      "captured_at": "2026-10-19T02:44:06.400236Z"
    },
    {
      "role": "38845886339-artifacts",
      "pages": 1,
      "reported_total": 0,
      "returned": 0,
      "response_sha256": "af3c25b59e7048d2e4b6ea03a688b2eced4e1786b5ae40f71516bc3ce0d4149e",
      "captured_at": "2026-10-19T02:44:06.459734Z"
    },
    {
      "role": "38845886339-same-sha-runs",
      "pages": 1,
      "reported_total": 12,
      "returned": 12,
      "response_sha256": "514a61123809cc39f8a63912bd4acbfaa5a2c907a0205983a24e3eba12c86e4f",
      "captured_at": "2026-10-19T02:44:07.048312Z"
    },
    {
      "role": "38856570439-attempt1-jobs",
      "pages": 1,
      "reported_total": 59,
      "returned": 59,
      "response_sha256": "b062fefc04e80e870552c2e7fe0a1fbaaa049d952a76e7f27254b265019b7c8d",
      "captured_at": "2026-10-19T02:44:10.456618Z"
    },
    {
      "role": "38856570439-artifacts",
      "pages": 1,
      "reported_total": 29,
      "returned": 29,
      "response_sha256": "3dde8db5aa493bc71b3d7cce9ed42fea2398a2e71316ee8c7a74b7c030621469",
      "captured_at": "2026-10-19T02:44:05.124866Z"
    },
    {
      "role": "38856570439-same-sha-runs",
      "pages": 1,
      "reported_total": 8,
      "returned": 8,
      "response_sha256": "2784f01cba9b3a96d4cea8ba7b7812f22b144d3ee05290380135674c8bc45a3a",
      "captured_at": "2026-10-19T02:44:05.671318Z"
    },
    {
      "role": "38873978927-attempt1-jobs",
      "pages": 1,
      "reported_total": 59,
      "returned": 59,
      "response_sha256": "4be0e5b0f18d878677919b450062643a906443dca293b07d332883f056c258a8",
      "captured_at": "2026-10-19T02:44:06.142123Z"
    },
    {
      "role": "38873978927-artifacts",
      "pages": 1,
      "reported_total": 33,
      "returned": 33,
      "response_sha256": "df7c39ade9f214e42d830c2fcf9a441a4ee49a222e7a17fe39c4e71a1bc1bc28",
      "captured_at": "2026-10-19T02:44:04.625611Z"
    },
    {
      "role": "38873978927-same-sha-runs",
      "pages": 1,
      "reported_total": 2,
      "returned": 2,
      "response_sha256": "98077fc79233f5981efc55399213ee5a9735768954c692604d6411632dcac837",
      "captured_at": "2026-10-19T02:44:04.820443Z"
    },
    {
      "role": "38878691465-attempt1-jobs",
      "pages": 1,
      "reported_total": 59,
      "returned": 59,
      "response_sha256": "83e935791f34378de2c3f90225e2b6125e00f3e394642cf46acb3d2caba9e5cb",
      "captured_at": "2026-10-19T02:44:04.401963Z"
    },
    {
      "role": "38878691465-artifacts",
      "pages": 1,
      "reported_total": 34,
      "returned": 34,
      "response_sha256": "6ebb4b5f438538c48552876f6df757338fd6dfd339da7546ba59600c166e1a21",
      "captured_at": "2026-10-19T02:43:59.615530Z"
    },
    {
      "role": "38878691465-same-sha-runs",
      "pages": 1,
      "reported_total": 17,
      "returned": 17,
      "response_sha256": "3debb92aacf5098912bacf0d548a311853f28d519f2c53dbc4f611e2b852f537",
      "captured_at": "2026-10-19T02:44:00.288501Z"
    },
    {
      "role": "39132650260-attempt1-jobs",
      "pages": 1,
      "reported_total": 61,
      "returned": 61,
      "response_sha256": "fc11496c942e6ded99dc0c7ba9d8da2da142554299c4948fd207253fbafc93c1",
      "captured_at": "2026-10-19T02:44:04.307392Z"
    },
    {
      "role": "39132650260-artifacts",
      "pages": 1,
      "reported_total": 46,
      "returned": 46,
      "response_sha256": "d18c33dcde213fe2dc71544f50e1f27ff789e378cdfdc5c5bfffa10339edf77e",
      "captured_at": "2026-10-19T02:43:58.935498Z"
    },
    {
      "role": "39132650260-same-sha-runs",
      "pages": 1,
      "reported_total": 12,
      "returned": 12,
      "response_sha256": "1bd92f22b650d7f78f79b09299ec25dbc299b0c7ff19196d504c63f0ad878ee5",
      "captured_at": "2026-10-19T02:43:59.271020Z"
    }
  ],
  "runs": [
    {
      "id": 38833746063,
      "run_attempt": 1,
      "event": "push",
      "head_sha": "5467c02d11f74efc1a85c833024efb654bbf98b5",
      "head_branch": "master",
      "status": "completed",
      "conclusion": "failure",
      "created_at": "2026-10-16T01:11:15Z",
      "run_started_at": "2026-10-16T01:11:15Z",
      "updated_at": "2026-10-16T01:56:42Z",
      "html_url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063",
      "workflow_id": 386168204,
      "path": ".github/workflows/dependency-tests.yml",
      "pull_request_numbers": [],
      "qualification": {
        "pr_comparison": "ineligible_event",
        "pr_reason": "No pull_request event/PR identity or reviewed legacy/selective pair; pushes/manual runs are not PR comparisons.",
        "broad_integration": "failed",
        "broad_reason": "Nested required Broad family accounting failed.",
        "accepted_pr_numbers": [],
        "accepted_broad": false,
        "required_workload_union_review": "not_accepted; accounting failure/absence is already disqualifying"
      },
      "job_count": 56,
      "job_conclusion_counts": {
        "success": 37,
        "failure": 18,
        "skipped": 1
      },
      "selection_and_worker_jobs": [
        {
          "id": 116758091484,
          "name": "select",
          "conclusion": "success",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758091484"
        },
        {
          "id": 116758466591,
          "name": "groundhog / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758466591"
        },
        {
          "id": 116758466628,
          "name": "core / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758466628"
        },
        {
          "id": 116758466655,
          "name": "grotto / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758466655"
        },
        {
          "id": 116758466682,
          "name": "mdk08 / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758466682"
        },
        {
          "id": 116758466683,
          "name": "policy / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758466683"
        },
        {
          "id": 116758466736,
          "name": "libgo / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758466736"
        },
        {
          "id": 116758466751,
          "name": "libgo_native / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758466751"
        },
        {
          "id": 116770728893,
          "name": "Dependency gate",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116770728893"
        }
      ],
      "gate_job": {
        "id": 116770728893,
        "name": "Dependency gate",
        "conclusion": "failure",
        "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116770728893"
      },
      "plan": {
        "plan_id": "13177f5cf5ac03e21ad9c92f4c0508112c9739a82aa39fca0a9cee9cc4071d39",
        "inventory_hash": "acab1855f888236d1681f3b217a75a6bf89ba43a00dd157fef2e3b0bed783c9f",
        "context": "ci",
        "effective_mode": "broad",
        "source_identity": {
          "kind": "committed",
          "sha": "5467c02d11f74efc1a85c833024efb654bbf98b5"
        },
        "expected_lane_ids": [
          "core.linux",
          "grotto.linux",
          "groundhog.linux",
          "groundhog.mdk08",
          "libgo.linux.asan",
          "libgo.macos.asan",
          "static.policy"
        ],
        "partial_profiles": [],
        "artifact_id": 12018358112
      },
      "validation": {
        "artifact_id": 12020390181,
        "member": "validation.json",
        "tested_sha": "5467c02d11f74efc1a85c833024efb654bbf98b5",
        "passed": false,
        "complete_requested": true,
        "complete_gate_eligible": null,
        "error_count": 18,
        "first_errors": [
          "selected worker job conclusions missing/skipped/failed/cancelled",
          "duplicate/unexpected lane: None",
          "missing lane: core.linux"
        ]
      },
      "lane_receipts": [
        {
          "artifact_id": 12018835234,
          "member": "libgo.linux.asan/receipt.json",
          "lane_id": null,
          "profile": null,
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "5467c02d11f74efc1a85c833024efb654bbf98b5"
          },
          "job_conclusion": null,
          "registration_audit": null,
          "input_audit": null,
          "complete_gate_eligible": true,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 0,
          "check_outcome_counts": {},
          "observed_execution_count": 0,
          "positive_checks": {}
        }
      ],
      "artifacts": [
        {
          "artifact_id": 12020390181,
          "name": "dependency-validation-13177f5cf5ac03e21ad9c92f4c0508112c9739a82aa39fca0a9cee9cc4071d39-1",
          "expired": false,
          "metadata_size_bytes": 1300,
          "metadata_digest": "sha256:b8ac2ab8a59fa709281b663779193a950017cbb6ac8b8db12639b979effb067a",
          "download_sha256": "b8ac2ab8a59fa709281b663779193a950017cbb6ac8b8db12639b979effb067a",
          "download_size_bytes": 1300,
          "download_exit_code": 0,
          "role": "validation",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12020390181",
          "member_sha256": {
            "validation.json": "0b5d56b39400d6e75ae18db0d049051ddba2d7949b79b59349e15b40bd2e47a3"
          }
        },
        {
          "artifact_id": 12018835234,
          "name": "dependency-receipt-13177f5cf5ac03e21ad9c92f4c0508112c9739a82aa39fca0a9cee9cc4071d39-libgo.linux.asan-1",
          "expired": false,
          "metadata_size_bytes": 4321,
          "metadata_digest": "sha256:954effad9454a1caab4b1cd019bd9c91a4cdf6005bf35628e69c3000b02dfbe5",
          "download_sha256": "954effad9454a1caab4b1cd019bd9c91a4cdf6005bf35628e69c3000b02dfbe5",
          "download_size_bytes": 4321,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12018835234",
          "member_sha256": {
            "libgo.linux.asan/receipt.json": "91fb0f5eb2486fa40c34b68993469f85cfff44cb11c3681d06b93a3f7b611c2c",
            "libgo.linux.asan/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12018750668,
          "name": "dependency-receipt-13177f5cf5ac03e21ad9c92f4c0508112c9739a82aa39fca0a9cee9cc4071d39-static.policy-1",
          "expired": false,
          "metadata_size_bytes": 1961,
          "metadata_digest": "sha256:3be995285c5a9b5c2e91aeca7bcd1acaecbb0b6a3a7f5cf728627cd028ee32c9",
          "download_sha256": "3be995285c5a9b5c2e91aeca7bcd1acaecbb0b6a3a7f5cf728627cd028ee32c9",
          "download_size_bytes": 1961,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12018750668",
          "member_sha256": {
            "static.policy/worker.log": "4a7cf28857d07d9437cc8577973453f800cd0d1c09c08f87c42a663fbab379fc"
          }
        },
        {
          "artifact_id": 12018681047,
          "name": "dependency-receipt-13177f5cf5ac03e21ad9c92f4c0508112c9739a82aa39fca0a9cee9cc4071d39-core.linux-1",
          "expired": false,
          "metadata_size_bytes": 1979,
          "metadata_digest": "sha256:ddec85e715e67d4c50eeb44c3da2fbdbf9b2e6df1ae2f36d9caae8a69e9a2efa",
          "download_sha256": "ddec85e715e67d4c50eeb44c3da2fbdbf9b2e6df1ae2f36d9caae8a69e9a2efa",
          "download_size_bytes": 1979,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12018681047",
          "member_sha256": {
            "core.linux/worker.log": "40636bf31d482282f16239aa9b28a066fd325262743aaaa10e8efc838203f8fc"
          }
        },
        {
          "artifact_id": 12018632028,
          "name": "dependency-receipt-13177f5cf5ac03e21ad9c92f4c0508112c9739a82aa39fca0a9cee9cc4071d39-grotto.linux-1",
          "expired": false,
          "metadata_size_bytes": 1988,
          "metadata_digest": "sha256:46dee673c27ceab99aa4cd9d450258ad23ebdd6db7a3a41e24a73fe08cf7c23e",
          "download_sha256": "46dee673c27ceab99aa4cd9d450258ad23ebdd6db7a3a41e24a73fe08cf7c23e",
          "download_size_bytes": 1988,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12018632028",
          "member_sha256": {
            "grotto.linux/worker.log": "357760b953ed9794d43776201c4b37dae44cf756fc4f1c0ca6775a24774ffbdf"
          }
        },
        {
          "artifact_id": 12018551806,
          "name": "dependency-receipt-13177f5cf5ac03e21ad9c92f4c0508112c9739a82aa39fca0a9cee9cc4071d39-groundhog.linux-1",
          "expired": false,
          "metadata_size_bytes": 1776,
          "metadata_digest": "sha256:64a3f2cf381d3ed092b24c8e9340e574254870ef193c5b2f6d4d3252e16e7ea1",
          "download_sha256": "64a3f2cf381d3ed092b24c8e9340e574254870ef193c5b2f6d4d3252e16e7ea1",
          "download_size_bytes": 1776,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12018551806",
          "member_sha256": {
            "groundhog.linux/worker.log": "4a7dd6bca1438e9d2a4849654c7b0d15971fd6ef8ee129e5b73a122e4a7cf184"
          }
        },
        {
          "artifact_id": 12018358112,
          "name": "dependency-plan-13177f5cf5ac03e21ad9c92f4c0508112c9739a82aa39fca0a9cee9cc4071d39-1",
          "expired": false,
          "metadata_size_bytes": 5604,
          "metadata_digest": "sha256:07e3fca2cda270976b550693ce50ab8cfc4b5c6558da0e5e4fd5a0f6306b8703",
          "download_sha256": "07e3fca2cda270976b550693ce50ab8cfc4b5c6558da0e5e4fd5a0f6306b8703",
          "download_size_bytes": 5604,
          "download_exit_code": 0,
          "role": "plan",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12018358112",
          "member_sha256": {
            "plan.json": "5f70644abba0d049808df49ae391a549a3abb47ba51bab31ea633bf171973066"
          }
        },
        {
          "artifact_id": 12017873407,
          "name": "dependency-receipt-13177f5cf5ac03e21ad9c92f4c0508112c9739a82aa39fca0a9cee9cc4071d39-groundhog.mdk08-1",
          "expired": false,
          "metadata_size_bytes": 2012,
          "metadata_digest": "sha256:809d325a1f9986216fc73d666bf70f4f71c9ec077be7f2d800ddbaf2a565b8bd",
          "download_sha256": "809d325a1f9986216fc73d666bf70f4f71c9ec077be7f2d800ddbaf2a565b8bd",
          "download_size_bytes": 2012,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12017873407",
          "member_sha256": {
            "groundhog.mdk08/worker.log": "5da03945ef4d668e8ebfd523630fd8a6f5b2f0c866356f61f19eb957b10b2c63"
          }
        },
        {
          "artifact_id": 12017618868,
          "name": "dependency-receipt-13177f5cf5ac03e21ad9c92f4c0508112c9739a82aa39fca0a9cee9cc4071d39-libgo.macos.asan-1",
          "expired": false,
          "metadata_size_bytes": 170,
          "metadata_digest": "sha256:ce50a2393029a64e9195f4bfe850d3eb70fef725c2f70168ca7e1a76f099e3f6",
          "download_sha256": "ce50a2393029a64e9195f4bfe850d3eb70fef725c2f70168ca7e1a76f099e3f6",
          "download_size_bytes": 170,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12017618868",
          "member_sha256": {
            "libgo.macos.asan/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        }
      ],
      "broad": {
        "nested_job_count": 47,
        "job_conclusion_counts": {
          "success": 36,
          "failure": 10,
          "skipped": 1
        },
        "accounting_job": {
          "id": 116770701220,
          "name": "broad / Broad family accounting",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116770701220"
        },
        "failed_jobs": [
          {
            "id": 116758467389,
            "name": "broad / bp_extended / nightly-matrix",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758467389"
          },
          {
            "id": 116758467415,
            "name": "broad / homed / build-and-test (debian-bookworm, debian:bookworm, apt-get update, apt-get install -y build-essent...",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758467415"
          },
          {
            "id": 116758467440,
            "name": "broad / homed / build-and-test (fedora-40, fedora:40, dnf -y update, dnf -y install @development-tools cmake ninj...",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758467440"
          },
          {
            "id": 116758467468,
            "name": "broad / homed / build-and-test (ubuntu-24.04, ubuntu:24.04, apt-get update, apt-get install -y build-essential cm...",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758467468"
          },
          {
            "id": 116758467475,
            "name": "broad / homed / build-and-test (archlinux, archlinux:latest, pacman -Syu --noconfirm, pacman -S --noconfirm base-...",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758467475"
          },
          {
            "id": 116758467498,
            "name": "broad / bp / bp-matrix",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758467498"
          },
          {
            "id": 116758736579,
            "name": "broad / fuzz / Fuzz json-event",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758736579"
          },
          {
            "id": 116758736596,
            "name": "broad / fuzz / Fuzz nip49",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758736596"
          },
          {
            "id": 116758736663,
            "name": "broad / fuzz / Fuzz bip39",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116758736663"
          },
          {
            "id": 116770701220,
            "name": "broad / Broad family accounting",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116770701220"
          }
        ],
        "non_success_jobs": [
          {
            "id": 116760531207,
            "name": "broad / fuzz / Generate coverage report",
            "conclusion": "skipped",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38833746063/job/116760531207"
          }
        ]
      },
      "same_sha_repository_runs": {
        "query_returned": 13,
        "other_run_ids": [
          38833745853,
          38833745877,
          38833745791,
          38833745813,
          38833745982,
          38833745874,
          38833745941,
          38833745916,
          38833745807,
          38833746195,
          38833745867,
          38833745789
        ],
        "conclusion_counts": {
          "success": 10,
          "failure": 3
        },
        "pairing_review": "Not a PR comparison; same SHA alone does not prove matching ranges/profiles/required coverage."
      },
      "workflow_file_sha256": {
        ".github/workflows/dependency-tests.yml": "d5b47588bd665c9dd14f2074ebfa071831e16a6cefb39743668eed070cd80be0",
        ".github/workflows/dependency-broad.yml": "79d5ce5af5f517d037b83fc5d4c92d3d31172a5ef059202e0a4b8ffe00d383b6"
      }
    },
    {
      "id": 38845886339,
      "run_attempt": 1,
      "event": "push",
      "head_sha": "33a1012767c9db5a46057280178134a544b86af7",
      "head_branch": "master",
      "status": "completed",
      "conclusion": "failure",
      "created_at": "2026-10-16T03:30:47Z",
      "run_started_at": "2026-10-16T03:30:47Z",
      "updated_at": "2026-10-16T03:32:15Z",
      "html_url": "https://github.com/chebizarro/nostrc/actions/runs/38845886339",
      "workflow_id": 386168204,
      "path": ".github/workflows/dependency-tests.yml",
      "pull_request_numbers": [],
      "qualification": {
        "pr_comparison": "ineligible_event",
        "pr_reason": "No pull_request event/PR identity or reviewed legacy/selective pair; pushes/manual runs are not PR comparisons.",
        "broad_integration": "blocked",
        "broad_reason": "Selection failed before broad children were launched.",
        "accepted_pr_numbers": [],
        "accepted_broad": false,
        "required_workload_union_review": "not_accepted; accounting failure/absence is already disqualifying"
      },
      "job_count": 10,
      "job_conclusion_counts": {
        "failure": 2,
        "skipped": 8
      },
      "selection_and_worker_jobs": [
        {
          "id": 116796201556,
          "name": "select",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38845886339/job/116796201556"
        },
        {
          "id": 116796544583,
          "name": "Dependency gate",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38845886339/job/116796544583"
        },
        {
          "id": 116796545238,
          "name": "groundhog",
          "conclusion": "skipped",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38845886339/job/116796545238"
        },
        {
          "id": 116796545266,
          "name": "libgo_native",
          "conclusion": "skipped",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38845886339/job/116796545266"
        },
        {
          "id": 116796545336,
          "name": "policy",
          "conclusion": "skipped",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38845886339/job/116796545336"
        },
        {
          "id": 116796545385,
          "name": "core",
          "conclusion": "skipped",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38845886339/job/116796545385"
        },
        {
          "id": 116796545512,
          "name": "broad",
          "conclusion": "skipped",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38845886339/job/116796545512"
        },
        {
          "id": 116796545607,
          "name": "mdk08",
          "conclusion": "skipped",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38845886339/job/116796545607"
        },
        {
          "id": 116796545608,
          "name": "libgo",
          "conclusion": "skipped",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38845886339/job/116796545608"
        },
        {
          "id": 116796545857,
          "name": "grotto",
          "conclusion": "skipped",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38845886339/job/116796545857"
        }
      ],
      "gate_job": {
        "id": 116796544583,
        "name": "Dependency gate",
        "conclusion": "failure",
        "url": "https://github.com/chebizarro/nostrc/actions/runs/38845886339/job/116796544583"
      },
      "plan": null,
      "validation": null,
      "lane_receipts": [],
      "artifacts": [],
      "broad": {
        "nested_job_count": 0,
        "job_conclusion_counts": {},
        "accounting_job": null,
        "failed_jobs": [],
        "non_success_jobs": []
      },
      "same_sha_repository_runs": {
        "query_returned": 12,
        "other_run_ids": [
          38845885901,
          38845886139,
          38845886143,
          38845886047,
          38845885985,
          38845885972,
          38845886109,
          38845886048,
          38845886024,
          38845886270,
          38845886016
        ],
        "conclusion_counts": {
          "success": 10,
          "failure": 2
        },
        "pairing_review": "Not a PR comparison; same SHA alone does not prove matching ranges/profiles/required coverage."
      },
      "workflow_file_sha256": {
        ".github/workflows/dependency-tests.yml": "d5b47588bd665c9dd14f2074ebfa071831e16a6cefb39743668eed070cd80be0",
        ".github/workflows/dependency-broad.yml": "79d5ce5af5f517d037b83fc5d4c92d3d31172a5ef059202e0a4b8ffe00d383b6"
      }
    },
    {
      "id": 38856570439,
      "run_attempt": 1,
      "event": "push",
      "head_sha": "8a031f443ac84850345a4cf0fb6ab5a8c3b32814",
      "head_branch": "master",
      "status": "completed",
      "conclusion": "failure",
      "created_at": "2026-10-16T05:35:29Z",
      "run_started_at": "2026-10-16T05:35:29Z",
      "updated_at": "2026-10-16T06:18:19Z",
      "html_url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439",
      "workflow_id": 386168204,
      "path": ".github/workflows/dependency-tests.yml",
      "pull_request_numbers": [],
      "qualification": {
        "pr_comparison": "ineligible_event",
        "pr_reason": "No pull_request event/PR identity or reviewed legacy/selective pair; pushes/manual runs are not PR comparisons.",
        "broad_integration": "failed",
        "broad_reason": "Nested required Broad family accounting failed.",
        "accepted_pr_numbers": [],
        "accepted_broad": false,
        "required_workload_union_review": "not_accepted; accounting failure/absence is already disqualifying"
      },
      "job_count": 59,
      "job_conclusion_counts": {
        "success": 39,
        "failure": 19,
        "skipped": 1
      },
      "selection_and_worker_jobs": [
        {
          "id": 116829255485,
          "name": "select",
          "conclusion": "success",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829255485"
        },
        {
          "id": 116829330340,
          "name": "grotto / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330340"
        },
        {
          "id": 116829330347,
          "name": "groundhog_sanitizers / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330347"
        },
        {
          "id": 116829330369,
          "name": "core / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330369"
        },
        {
          "id": 116829330383,
          "name": "groundhog / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330383"
        },
        {
          "id": 116829330398,
          "name": "mdk08 / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330398"
        },
        {
          "id": 116829330419,
          "name": "policy / execute",
          "conclusion": "success",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330419"
        },
        {
          "id": 116829330482,
          "name": "core_asan / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330482"
        },
        {
          "id": 116829330520,
          "name": "libgo / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330520"
        },
        {
          "id": 116829330534,
          "name": "libgo_native / execute",
          "conclusion": "success",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330534"
        },
        {
          "id": 116829330573,
          "name": "core_native / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330573"
        },
        {
          "id": 116841190994,
          "name": "Dependency gate",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116841190994"
        }
      ],
      "gate_job": {
        "id": 116841190994,
        "name": "Dependency gate",
        "conclusion": "failure",
        "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116841190994"
      },
      "plan": {
        "plan_id": "3b0264bf4f094946e192da6f7db6340e25e16cc6af97084c0253017426e321df",
        "inventory_hash": "00045270c2a511ce96c674fe4a835842ed27719fa3cbcf6a4dc7a3cbd70fca5d",
        "context": "ci",
        "effective_mode": "broad",
        "source_identity": {
          "kind": "committed",
          "sha": "8a031f443ac84850345a4cf0fb6ab5a8c3b32814"
        },
        "expected_lane_ids": [
          "core.linux",
          "core.linux.asan",
          "core.macos.asan",
          "grotto.linux",
          "groundhog.linux",
          "groundhog.linux.sanitizers",
          "groundhog.mdk08",
          "libgo.linux.asan",
          "libgo.macos.asan",
          "static.policy"
        ],
        "partial_profiles": [],
        "artifact_id": 12027855083
      },
      "validation": {
        "artifact_id": 12028398157,
        "member": "validation.json",
        "tested_sha": "8a031f443ac84850345a4cf0fb6ab5a8c3b32814",
        "passed": false,
        "complete_requested": true,
        "complete_gate_eligible": false,
        "error_count": 25,
        "first_errors": [
          "inventory coverage/enforcement readiness cannot satisfy complete gate",
          "selected worker job conclusions missing/skipped/failed/cancelled",
          "duplicate/unexpected lane: None"
        ]
      },
      "lane_receipts": [
        {
          "artifact_id": 12027775498,
          "member": "libgo.linux.asan/receipt.json",
          "lane_id": null,
          "profile": null,
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "8a031f443ac84850345a4cf0fb6ab5a8c3b32814"
          },
          "job_conclusion": null,
          "registration_audit": null,
          "input_audit": null,
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 0,
          "check_outcome_counts": {},
          "observed_execution_count": 0,
          "positive_checks": {}
        },
        {
          "artifact_id": 12027745788,
          "member": "static.policy/receipt.json",
          "lane_id": "static.policy",
          "profile": "static.policy",
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "8a031f443ac84850345a4cf0fb6ab5a8c3b32814"
          },
          "job_conclusion": "success",
          "registration_audit": "passed",
          "input_audit": "passed",
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 1,
          "check_outcome_counts": {
            "passed": 1
          },
          "observed_execution_count": 1,
          "positive_checks": {
            "selection.contract": {
              "outcome": "passed",
              "observed_execution_count": 1
            }
          }
        },
        {
          "artifact_id": 12027323348,
          "member": "libgo.macos.asan/receipt.json",
          "lane_id": "libgo.macos.asan",
          "profile": "libgo.macos.asan",
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "8a031f443ac84850345a4cf0fb6ab5a8c3b32814"
          },
          "job_conclusion": "success",
          "registration_audit": "passed",
          "input_audit": "passed",
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 2,
          "check_outcome_counts": {
            "passed": 2
          },
          "observed_execution_count": 32,
          "positive_checks": {
            "libgo.channel": {
              "outcome": "passed",
              "observed_execution_count": 1
            },
            "libgo.native_standalone": {
              "outcome": "passed",
              "observed_execution_count": 31
            }
          }
        },
        {
          "artifact_id": 12027248137,
          "member": "core.linux.asan/receipt.json",
          "lane_id": null,
          "profile": null,
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "8a031f443ac84850345a4cf0fb6ab5a8c3b32814"
          },
          "job_conclusion": null,
          "registration_audit": null,
          "input_audit": null,
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 0,
          "check_outcome_counts": {},
          "observed_execution_count": 0,
          "positive_checks": {}
        },
        {
          "artifact_id": 12026609380,
          "member": "core.macos.asan/receipt.json",
          "lane_id": null,
          "profile": null,
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "8a031f443ac84850345a4cf0fb6ab5a8c3b32814"
          },
          "job_conclusion": null,
          "registration_audit": null,
          "input_audit": null,
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 0,
          "check_outcome_counts": {},
          "observed_execution_count": 0,
          "positive_checks": {}
        }
      ],
      "artifacts": [
        {
          "artifact_id": 12028398157,
          "name": "dependency-validation-3b0264bf4f094946e192da6f7db6340e25e16cc6af97084c0253017426e321df-1",
          "expired": false,
          "metadata_size_bytes": 2080,
          "metadata_digest": "sha256:da8f733bfbeb5447b9c73ab387d4c8482230f1c6aa86c6fbe26c1f2abfd29171",
          "download_sha256": "da8f733bfbeb5447b9c73ab387d4c8482230f1c6aa86c6fbe26c1f2abfd29171",
          "download_size_bytes": 2080,
          "download_exit_code": 0,
          "role": "validation",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12028398157",
          "member_sha256": {
            "validation.json": "3277f02015f88e019ced874f11591e49aaebf5b3e499798d91526b7de264cfd7"
          }
        },
        {
          "artifact_id": 12027925197,
          "name": "dependency-receipt-3b0264bf4f094946e192da6f7db6340e25e16cc6af97084c0253017426e321df-groundhog.linux-1",
          "expired": false,
          "metadata_size_bytes": 1775,
          "metadata_digest": "sha256:d5f9c550fb5f4e4306ffc712b944fd4b6d219e2bb6e61acc7cf74b4ec5075cdc",
          "download_sha256": "d5f9c550fb5f4e4306ffc712b944fd4b6d219e2bb6e61acc7cf74b4ec5075cdc",
          "download_size_bytes": 1775,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12027925197",
          "member_sha256": {
            "groundhog.linux/worker.log": "edc07daa0324b386c10c5caafba13252e8094183aba0fecd224152bbf7125aca"
          }
        },
        {
          "artifact_id": 12027870329,
          "name": "dependency-receipt-3b0264bf4f094946e192da6f7db6340e25e16cc6af97084c0253017426e321df-groundhog.linux.sanitizers-1",
          "expired": false,
          "metadata_size_bytes": 1767,
          "metadata_digest": "sha256:dfedda03e0fa0b2f8b23fa988b67f35feb377b0917bcba0da667e0bc16dbd576",
          "download_sha256": "dfedda03e0fa0b2f8b23fa988b67f35feb377b0917bcba0da667e0bc16dbd576",
          "download_size_bytes": 1767,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12027870329",
          "member_sha256": {
            "groundhog.linux.sanitizers/worker.log": "868e377727999df01fab488474c4bde7ac9afab47ff625a838c9e2202744259a"
          }
        },
        {
          "artifact_id": 12027860350,
          "name": "dependency-receipt-3b0264bf4f094946e192da6f7db6340e25e16cc6af97084c0253017426e321df-core.linux-1",
          "expired": false,
          "metadata_size_bytes": 1977,
          "metadata_digest": "sha256:333a0918aa837fae0281061f7ab4b2a584326583886bc830fcc3542aa7646373",
          "download_sha256": "333a0918aa837fae0281061f7ab4b2a584326583886bc830fcc3542aa7646373",
          "download_size_bytes": 1977,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12027860350",
          "member_sha256": {
            "core.linux/worker.log": "4446cc84df12ad917964d4f3c0755e5c9e960d11725d83d17114ca94211fce32"
          }
        },
        {
          "artifact_id": 12027855083,
          "name": "dependency-plan-3b0264bf4f094946e192da6f7db6340e25e16cc6af97084c0253017426e321df-1",
          "expired": false,
          "metadata_size_bytes": 5261,
          "metadata_digest": "sha256:bd8753986fd193769a5576d1aae7af9d98b05e3a8787ae81ca641770428e6ba0",
          "download_sha256": "bd8753986fd193769a5576d1aae7af9d98b05e3a8787ae81ca641770428e6ba0",
          "download_size_bytes": 5261,
          "download_exit_code": 0,
          "role": "plan",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12027855083",
          "member_sha256": {
            "plan.json": "44a9071a16871d05c386b0aa0d697eb857a0cf62f746951789d60d60009ba3d4"
          }
        },
        {
          "artifact_id": 12027775498,
          "name": "dependency-receipt-3b0264bf4f094946e192da6f7db6340e25e16cc6af97084c0253017426e321df-libgo.linux.asan-1",
          "expired": false,
          "metadata_size_bytes": 5424,
          "metadata_digest": "sha256:50572effd00ef3b53d51f5629edd3745d87484a71678bf7e58b8c5bb705e2384",
          "download_sha256": "50572effd00ef3b53d51f5629edd3745d87484a71678bf7e58b8c5bb705e2384",
          "download_size_bytes": 5424,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12027775498",
          "member_sha256": {
            "libgo.linux.asan/receipt.json": "93f550bd53d009ae2994bd824aaa51f0f7258cd4a37b077990e9246d4f5b22d8",
            "libgo.linux.asan/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12027745788,
          "name": "dependency-receipt-3b0264bf4f094946e192da6f7db6340e25e16cc6af97084c0253017426e321df-static.policy-1",
          "expired": false,
          "metadata_size_bytes": 8661,
          "metadata_digest": "sha256:7080b2730d648ee04d49ba790c3e452902859722a51b4d3515e40d7fd834ac3a",
          "download_sha256": "7080b2730d648ee04d49ba790c3e452902859722a51b4d3515e40d7fd834ac3a",
          "download_size_bytes": 8661,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12027745788",
          "member_sha256": {
            "static.policy/receipt.json": "c6fdeffd240ba8b138c3c4d0afde99334ad1dcc543b7dc821576812a1503d1a4",
            "static.policy/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12027457503,
          "name": "dependency-receipt-3b0264bf4f094946e192da6f7db6340e25e16cc6af97084c0253017426e321df-groundhog.mdk08-1",
          "expired": false,
          "metadata_size_bytes": 2012,
          "metadata_digest": "sha256:7f7f63c38e4f3e974beba0c4a558eff86ddb58c6ce9224561d5ee515aefdb0d5",
          "download_sha256": "7f7f63c38e4f3e974beba0c4a558eff86ddb58c6ce9224561d5ee515aefdb0d5",
          "download_size_bytes": 2012,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12027457503",
          "member_sha256": {
            "groundhog.mdk08/worker.log": "53767ea03c688d0ee5ba59e52eeb1186aae9d724f22f4e6593619c6116664748"
          }
        },
        {
          "artifact_id": 12027452461,
          "name": "dependency-receipt-3b0264bf4f094946e192da6f7db6340e25e16cc6af97084c0253017426e321df-grotto.linux-1",
          "expired": false,
          "metadata_size_bytes": 1988,
          "metadata_digest": "sha256:4b5d22686effd46db2c43fdf191cd3179bb8417c18d433bc41c843cd3d86e2d4",
          "download_sha256": "4b5d22686effd46db2c43fdf191cd3179bb8417c18d433bc41c843cd3d86e2d4",
          "download_size_bytes": 1988,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12027452461",
          "member_sha256": {
            "grotto.linux/worker.log": "2851323a45aca37bd15887212124609e18e71f2ec07b6c5aa0c3ab9a6f11d655"
          }
        },
        {
          "artifact_id": 12027323348,
          "name": "dependency-receipt-3b0264bf4f094946e192da6f7db6340e25e16cc6af97084c0253017426e321df-libgo.macos.asan-1",
          "expired": false,
          "metadata_size_bytes": 20323,
          "metadata_digest": "sha256:5a74861f3df21dcb03668e867de5efe363e2f96a4a179b2c4cc53722f6ffb14a",
          "download_sha256": "5a74861f3df21dcb03668e867de5efe363e2f96a4a179b2c4cc53722f6ffb14a",
          "download_size_bytes": 20323,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12027323348",
          "member_sha256": {
            "libgo.macos.asan/receipt.json": "c5fbe6b294507260c5230e99a21b6eefee40de48168f3de78185b8ab3899205a",
            "libgo.macos.asan/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12027248137,
          "name": "dependency-receipt-3b0264bf4f094946e192da6f7db6340e25e16cc6af97084c0253017426e321df-core.linux.asan-1",
          "expired": false,
          "metadata_size_bytes": 80343,
          "metadata_digest": "sha256:360863bb953b128bb875b71a8ec292051cb7ac45db9f18a79851f4eeb31f740e",
          "download_sha256": "360863bb953b128bb875b71a8ec292051cb7ac45db9f18a79851f4eeb31f740e",
          "download_size_bytes": 80343,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12027248137",
          "member_sha256": {
            "core.linux.asan/receipt.json": "d367c0655464f578f0f51d5306099a2a749de3518e1723f3134616a40ed0d60f",
            "core.linux.asan/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12026609380,
          "name": "dependency-receipt-3b0264bf4f094946e192da6f7db6340e25e16cc6af97084c0253017426e321df-core.macos.asan-1",
          "expired": false,
          "metadata_size_bytes": 73562,
          "metadata_digest": "sha256:a6b7d7373d1fe5a86daf32a85e721733503b6f43670d26f73d7b54f273d5bb34",
          "download_sha256": "a6b7d7373d1fe5a86daf32a85e721733503b6f43670d26f73d7b54f273d5bb34",
          "download_size_bytes": 73562,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12026609380",
          "member_sha256": {
            "core.macos.asan/receipt.json": "48309a0192fb93454baaad6b04783e018c06eaa944ff008bd2d66797d6e71b03",
            "core.macos.asan/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        }
      ],
      "broad": {
        "nested_job_count": 47,
        "job_conclusion_counts": {
          "success": 36,
          "failure": 10,
          "skipped": 1
        },
        "accounting_job": {
          "id": 116841163905,
          "name": "broad / Broad family accounting",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116841163905"
        },
        "failed_jobs": [
          {
            "id": 116829330879,
            "name": "broad / homed / build-and-test (fedora-40, fedora:40, dnf -y update, dnf -y install @development-tools cmake ninj...",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330879"
          },
          {
            "id": 116829330888,
            "name": "broad / bp_extended / nightly-matrix",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330888"
          },
          {
            "id": 116829330914,
            "name": "broad / homed / build-and-test (debian-bookworm, debian:bookworm, apt-get update, apt-get install -y build-essent...",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330914"
          },
          {
            "id": 116829330915,
            "name": "broad / homed / build-and-test (ubuntu-24.04, ubuntu:24.04, apt-get update, apt-get install -y build-essential cm...",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330915"
          },
          {
            "id": 116829330967,
            "name": "broad / homed / build-and-test (archlinux, archlinux:latest, pacman -Syu --noconfirm, pacman -S --noconfirm base-...",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829330967"
          },
          {
            "id": 116829331088,
            "name": "broad / bp / bp-matrix",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116829331088"
          },
          {
            "id": 116831017659,
            "name": "broad / fuzz / Fuzz bip39",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116831017659"
          },
          {
            "id": 116831017669,
            "name": "broad / fuzz / Fuzz json-event",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116831017669"
          },
          {
            "id": 116831017860,
            "name": "broad / fuzz / Fuzz nip49",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116831017860"
          },
          {
            "id": 116841163905,
            "name": "broad / Broad family accounting",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116841163905"
          }
        ],
        "non_success_jobs": [
          {
            "id": 116831289438,
            "name": "broad / fuzz / Generate coverage report",
            "conclusion": "skipped",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38856570439/job/116831289438"
          }
        ]
      },
      "same_sha_repository_runs": {
        "query_returned": 8,
        "other_run_ids": [
          38874039865,
          38871961965,
          38856570251,
          38856570237,
          38856570261,
          38856570245,
          38856570188
        ],
        "conclusion_counts": {
          "success": 6,
          "failure": 2
        },
        "pairing_review": "Not a PR comparison; same SHA alone does not prove matching ranges/profiles/required coverage."
      },
      "workflow_file_sha256": {
        ".github/workflows/dependency-tests.yml": "215dd788507eb2fff04dd3ed70193126006ec642bf949b3689fc8b1415e486e2",
        ".github/workflows/dependency-broad.yml": "79d5ce5af5f517d037b83fc5d4c92d3d31172a5ef059202e0a4b8ffe00d383b6"
      }
    },
    {
      "id": 38873978927,
      "run_attempt": 1,
      "event": "workflow_dispatch",
      "head_sha": "a478070509569c26917dd5fb13a548b1f68eade9",
      "head_branch": "ci/testing-final-integration-20261016",
      "status": "completed",
      "conclusion": "cancelled",
      "created_at": "2026-10-16T08:29:01Z",
      "run_started_at": "2026-10-16T08:29:01Z",
      "updated_at": "2026-10-16T09:19:49Z",
      "html_url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927",
      "workflow_id": 386168204,
      "path": ".github/workflows/dependency-tests.yml",
      "pull_request_numbers": [],
      "qualification": {
        "pr_comparison": "ineligible_event",
        "pr_reason": "No pull_request event/PR identity or reviewed legacy/selective pair; pushes/manual runs are not PR comparisons.",
        "broad_integration": "failed",
        "broad_reason": "Nested required Broad family accounting failed.",
        "accepted_pr_numbers": [],
        "accepted_broad": false,
        "required_workload_union_review": "not_accepted; accounting failure/absence is already disqualifying"
      },
      "job_count": 59,
      "job_conclusion_counts": {
        "success": 45,
        "failure": 13,
        "skipped": 1
      },
      "selection_and_worker_jobs": [
        {
          "id": 116884499254,
          "name": "select",
          "conclusion": "success",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116884499254"
        },
        {
          "id": 116884595889,
          "name": "groundhog_sanitizers / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116884595889"
        },
        {
          "id": 116884595890,
          "name": "groundhog / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116884595890"
        },
        {
          "id": 116884595942,
          "name": "core_native / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116884595942"
        },
        {
          "id": 116884595995,
          "name": "grotto / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116884595995"
        },
        {
          "id": 116884596024,
          "name": "core_asan / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116884596024"
        },
        {
          "id": 116884596077,
          "name": "libgo_native / execute",
          "conclusion": "success",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116884596077"
        },
        {
          "id": 116884596080,
          "name": "policy / execute",
          "conclusion": "success",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116884596080"
        },
        {
          "id": 116884596182,
          "name": "mdk08 / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116884596182"
        },
        {
          "id": 116884596184,
          "name": "core / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116884596184"
        },
        {
          "id": 116884596359,
          "name": "libgo / execute",
          "conclusion": "success",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116884596359"
        },
        {
          "id": 116900256052,
          "name": "Dependency gate",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116900256052"
        }
      ],
      "gate_job": {
        "id": 116900256052,
        "name": "Dependency gate",
        "conclusion": "failure",
        "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116900256052"
      },
      "plan": {
        "plan_id": "3aba1a4e60bdf008fe6079e12d9baeaa67bd06f29517b4d319ee0a6e57112744",
        "inventory_hash": "6b272061d8c62dec6cc67813576d3d5f0c6923a859fba672971c66a213e0edbc",
        "context": "ci",
        "effective_mode": "broad",
        "source_identity": {
          "kind": "committed",
          "sha": "a478070509569c26917dd5fb13a548b1f68eade9"
        },
        "expected_lane_ids": [
          "core.linux",
          "core.linux.asan",
          "core.macos.asan",
          "grotto.linux",
          "groundhog.linux",
          "groundhog.linux.sanitizers",
          "groundhog.mdk08",
          "libgo.linux.asan",
          "libgo.macos.asan",
          "static.policy"
        ],
        "partial_profiles": [],
        "artifact_id": 12034472127
      },
      "validation": {
        "artifact_id": 12036857589,
        "member": "validation.json",
        "tested_sha": "a478070509569c26917dd5fb13a548b1f68eade9",
        "passed": false,
        "complete_requested": true,
        "complete_gate_eligible": false,
        "error_count": 24,
        "first_errors": [
          "inventory coverage/enforcement readiness cannot satisfy complete gate",
          "selected worker job conclusions missing/skipped/failed/cancelled",
          "duplicate/unexpected lane: None"
        ]
      },
      "lane_receipts": [
        {
          "artifact_id": 12034791870,
          "member": "groundhog.linux.sanitizers/receipt.json",
          "lane_id": null,
          "profile": null,
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "a478070509569c26917dd5fb13a548b1f68eade9"
          },
          "job_conclusion": null,
          "registration_audit": null,
          "input_audit": null,
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 0,
          "check_outcome_counts": {},
          "observed_execution_count": 0,
          "positive_checks": {}
        },
        {
          "artifact_id": 12034733294,
          "member": "core.linux.asan/receipt.json",
          "lane_id": null,
          "profile": null,
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "a478070509569c26917dd5fb13a548b1f68eade9"
          },
          "job_conclusion": null,
          "registration_audit": null,
          "input_audit": null,
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 0,
          "check_outcome_counts": {},
          "observed_execution_count": 0,
          "positive_checks": {}
        },
        {
          "artifact_id": 12034417836,
          "member": "libgo.linux.asan/receipt.json",
          "lane_id": "libgo.linux.asan",
          "profile": "libgo.linux.asan",
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "a478070509569c26917dd5fb13a548b1f68eade9"
          },
          "job_conclusion": "success",
          "registration_audit": "passed",
          "input_audit": "passed",
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 2,
          "check_outcome_counts": {
            "passed": 2
          },
          "observed_execution_count": 33,
          "positive_checks": {
            "libgo.channel": {
              "outcome": "passed",
              "observed_execution_count": 1
            },
            "libgo.native_standalone": {
              "outcome": "passed",
              "observed_execution_count": 32
            }
          }
        },
        {
          "artifact_id": 12034264843,
          "member": "core.macos.asan/receipt.json",
          "lane_id": null,
          "profile": null,
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "a478070509569c26917dd5fb13a548b1f68eade9"
          },
          "job_conclusion": null,
          "registration_audit": null,
          "input_audit": null,
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 0,
          "check_outcome_counts": {},
          "observed_execution_count": 0,
          "positive_checks": {}
        },
        {
          "artifact_id": 12034124206,
          "member": "libgo.macos.asan/receipt.json",
          "lane_id": "libgo.macos.asan",
          "profile": "libgo.macos.asan",
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "a478070509569c26917dd5fb13a548b1f68eade9"
          },
          "job_conclusion": "success",
          "registration_audit": "passed",
          "input_audit": "passed",
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 2,
          "check_outcome_counts": {
            "passed": 2
          },
          "observed_execution_count": 32,
          "positive_checks": {
            "libgo.channel": {
              "outcome": "passed",
              "observed_execution_count": 1
            },
            "libgo.native_standalone": {
              "outcome": "passed",
              "observed_execution_count": 31
            }
          }
        },
        {
          "artifact_id": 12033829723,
          "member": "static.policy/receipt.json",
          "lane_id": "static.policy",
          "profile": "static.policy",
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "a478070509569c26917dd5fb13a548b1f68eade9"
          },
          "job_conclusion": "success",
          "registration_audit": "passed",
          "input_audit": "passed",
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 1,
          "check_outcome_counts": {
            "passed": 1
          },
          "observed_execution_count": 1,
          "positive_checks": {
            "selection.contract": {
              "outcome": "passed",
              "observed_execution_count": 1
            }
          }
        }
      ],
      "artifacts": [
        {
          "artifact_id": 12036857589,
          "name": "dependency-validation-3aba1a4e60bdf008fe6079e12d9baeaa67bd06f29517b4d319ee0a6e57112744-1",
          "expired": false,
          "metadata_size_bytes": 4894,
          "metadata_digest": "sha256:63f3e13b30c9537282b954bd02f8a3bb384853c696c904f47b5743d412c23bd5",
          "download_sha256": "63f3e13b30c9537282b954bd02f8a3bb384853c696c904f47b5743d412c23bd5",
          "download_size_bytes": 4894,
          "download_exit_code": 0,
          "role": "validation",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12036857589",
          "member_sha256": {
            "validation.json": "99e22ac4dde85a7a3efb7c68c5074f85be8e8ebc2f0f195f582ba76e2662956c"
          }
        },
        {
          "artifact_id": 12035130067,
          "name": "dependency-receipt-3aba1a4e60bdf008fe6079e12d9baeaa67bd06f29517b4d319ee0a6e57112744-groundhog.mdk08-1",
          "expired": false,
          "metadata_size_bytes": 2109,
          "metadata_digest": "sha256:3fe33129e0f5c864c4cdcb1518c06d748182841ac720871e467c919a33547299",
          "download_sha256": "3fe33129e0f5c864c4cdcb1518c06d748182841ac720871e467c919a33547299",
          "download_size_bytes": 2109,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12035130067",
          "member_sha256": {
            "groundhog.mdk08/worker.log": "ac96f17d194a78de4f445d277556bcffaff7dbfbc5071bec4c3168ff782bd6da"
          }
        },
        {
          "artifact_id": 12034842125,
          "name": "dependency-receipt-3aba1a4e60bdf008fe6079e12d9baeaa67bd06f29517b4d319ee0a6e57112744-core.linux-1",
          "expired": false,
          "metadata_size_bytes": 2074,
          "metadata_digest": "sha256:6b10f50468d43a7f3e779056548b85ae1334c7b3bcb9463b124a5e664ca9b2f1",
          "download_sha256": "6b10f50468d43a7f3e779056548b85ae1334c7b3bcb9463b124a5e664ca9b2f1",
          "download_size_bytes": 2074,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12034842125",
          "member_sha256": {
            "core.linux/worker.log": "c8d5bc89c749148810cbbc4cabc615f81f41bca3e1e939c7a778dd8d890892eb"
          }
        },
        {
          "artifact_id": 12034791870,
          "name": "dependency-receipt-3aba1a4e60bdf008fe6079e12d9baeaa67bd06f29517b4d319ee0a6e57112744-groundhog.linux.sanitizers-1",
          "expired": false,
          "metadata_size_bytes": 68668,
          "metadata_digest": "sha256:beda93545659efc8f976960edbc00dc078c48290bbbac4cc3f497b20fc9d177a",
          "download_sha256": "beda93545659efc8f976960edbc00dc078c48290bbbac4cc3f497b20fc9d177a",
          "download_size_bytes": 68668,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12034791870",
          "member_sha256": {
            "groundhog.linux.sanitizers/receipt.json": "7244df156e6a17bcfb4d8878607a42745aba1f93666cad955bba80fca1f3036e",
            "groundhog.linux.sanitizers/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12034733294,
          "name": "dependency-receipt-3aba1a4e60bdf008fe6079e12d9baeaa67bd06f29517b4d319ee0a6e57112744-core.linux.asan-1",
          "expired": false,
          "metadata_size_bytes": 34640,
          "metadata_digest": "sha256:4171015a677e4f7e7c8b65d2690a4e490c89d4332636a4d54b43f71a4236cc64",
          "download_sha256": "4171015a677e4f7e7c8b65d2690a4e490c89d4332636a4d54b43f71a4236cc64",
          "download_size_bytes": 34640,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12034733294",
          "member_sha256": {
            "core.linux.asan/receipt.json": "5495484822f6e16f6b625ddc6aa2d948060686655593f191252aeb3dbad5557d",
            "core.linux.asan/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12034472127,
          "name": "dependency-plan-3aba1a4e60bdf008fe6079e12d9baeaa67bd06f29517b4d319ee0a6e57112744-1",
          "expired": false,
          "metadata_size_bytes": 6776,
          "metadata_digest": "sha256:c216c2bb4dff51a9a1e4603b228dcb8240966e339159f42e18f9a9d2fd5c67b7",
          "download_sha256": "c216c2bb4dff51a9a1e4603b228dcb8240966e339159f42e18f9a9d2fd5c67b7",
          "download_size_bytes": 6776,
          "download_exit_code": 0,
          "role": "plan",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12034472127",
          "member_sha256": {
            "plan.json": "408d0ea27eb604eb6bc954747afe26c01e99360a4ff5859a41b08fe3ec0d1fc0"
          }
        },
        {
          "artifact_id": 12034417836,
          "name": "dependency-receipt-3aba1a4e60bdf008fe6079e12d9baeaa67bd06f29517b4d319ee0a6e57112744-libgo.linux.asan-1",
          "expired": false,
          "metadata_size_bytes": 24305,
          "metadata_digest": "sha256:cd9df3e930dab5d193a4b099950d3587851365d61d030bc6c39ed92a4046b92c",
          "download_sha256": "cd9df3e930dab5d193a4b099950d3587851365d61d030bc6c39ed92a4046b92c",
          "download_size_bytes": 24305,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12034417836",
          "member_sha256": {
            "libgo.linux.asan/receipt.json": "94488c8891032cf926de8a0a60e5133ffb37b6d47eb47cba7249dc0eaad992d5",
            "libgo.linux.asan/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12034264843,
          "name": "dependency-receipt-3aba1a4e60bdf008fe6079e12d9baeaa67bd06f29517b4d319ee0a6e57112744-core.macos.asan-1",
          "expired": false,
          "metadata_size_bytes": 34601,
          "metadata_digest": "sha256:7d3c9506cdad652ef213d10b1f5c79d245edea889fdc1a1438b1d8d738887a09",
          "download_sha256": "7d3c9506cdad652ef213d10b1f5c79d245edea889fdc1a1438b1d8d738887a09",
          "download_size_bytes": 34601,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12034264843",
          "member_sha256": {
            "core.macos.asan/receipt.json": "ea07f17ac8975583d02e2d7fb5578adf35bebf32f8da653dc684734b360355c6",
            "core.macos.asan/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12034124206,
          "name": "dependency-receipt-3aba1a4e60bdf008fe6079e12d9baeaa67bd06f29517b4d319ee0a6e57112744-libgo.macos.asan-1",
          "expired": false,
          "metadata_size_bytes": 27466,
          "metadata_digest": "sha256:0274f5bd9fc93411a2746fa9e0bc8d5d5e04607c6c10f43751502ad875176817",
          "download_sha256": "0274f5bd9fc93411a2746fa9e0bc8d5d5e04607c6c10f43751502ad875176817",
          "download_size_bytes": 27466,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12034124206",
          "member_sha256": {
            "libgo.macos.asan/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            "libgo.macos.asan/receipt.json": "325b24a9b926cfa69bebbc2c177244ebf43d737c832d570f0c33504d2c659348"
          }
        },
        {
          "artifact_id": 12033953011,
          "name": "dependency-receipt-3aba1a4e60bdf008fe6079e12d9baeaa67bd06f29517b4d319ee0a6e57112744-groundhog.linux-1",
          "expired": false,
          "metadata_size_bytes": 234,
          "metadata_digest": "sha256:aede9821e2d1ac46165ff539a58ec07357d62234830b002b948c0453c92d3fcf",
          "download_sha256": "aede9821e2d1ac46165ff539a58ec07357d62234830b002b948c0453c92d3fcf",
          "download_size_bytes": 234,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12033953011",
          "member_sha256": {
            "groundhog.linux/worker.log": "c3248197fc46c39acc93fb321661c79fa213f9f3ab8d6d7c86ed0ff9fb1d050e"
          }
        },
        {
          "artifact_id": 12033829723,
          "name": "dependency-receipt-3aba1a4e60bdf008fe6079e12d9baeaa67bd06f29517b4d319ee0a6e57112744-static.policy-1",
          "expired": false,
          "metadata_size_bytes": 20630,
          "metadata_digest": "sha256:dc9d81720c68c9bee696dcc4a1a04160ac5e49b90a8f2af626eae063bff88dac",
          "download_sha256": "dc9d81720c68c9bee696dcc4a1a04160ac5e49b90a8f2af626eae063bff88dac",
          "download_size_bytes": 20630,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12033829723",
          "member_sha256": {
            "static.policy/receipt.json": "8182e76c2295c5cf4b9a0fb82fe0a2166dc25c5d55f7b17f91fd78dd03848cc9",
            "static.policy/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12033788282,
          "name": "dependency-receipt-3aba1a4e60bdf008fe6079e12d9baeaa67bd06f29517b4d319ee0a6e57112744-grotto.linux-1",
          "expired": false,
          "metadata_size_bytes": 2085,
          "metadata_digest": "sha256:7f7e0e4991ec61ae0253ea4fbab0a99dd52c78c56999662d6b1014330bd3cac6",
          "download_sha256": "7f7e0e4991ec61ae0253ea4fbab0a99dd52c78c56999662d6b1014330bd3cac6",
          "download_size_bytes": 2085,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12033788282",
          "member_sha256": {
            "grotto.linux/worker.log": "08b424f999d5834b8de7e169b4c51c66500943b1e03f61e97833e0f1008a2038"
          }
        }
      ],
      "broad": {
        "nested_job_count": 47,
        "job_conclusion_counts": {
          "success": 41,
          "failure": 5,
          "skipped": 1
        },
        "accounting_job": {
          "id": 116900093654,
          "name": "broad / Broad family accounting",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116900093654"
        },
        "failed_jobs": [
          {
            "id": 116884596656,
            "name": "broad / groundhog / groundhog-sanitizers",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116884596656"
          },
          {
            "id": 116884597017,
            "name": "broad / bp / bp-matrix",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116884597017"
          },
          {
            "id": 116884597175,
            "name": "broad / bp_extended / nightly-matrix",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116884597175"
          },
          {
            "id": 116885248265,
            "name": "broad / fuzz / Fuzz json-event",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116885248265"
          },
          {
            "id": 116900093654,
            "name": "broad / Broad family accounting",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116900093654"
          }
        ],
        "non_success_jobs": [
          {
            "id": 116889115593,
            "name": "broad / fuzz / Generate coverage report",
            "conclusion": "skipped",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38873978927/job/116889115593"
          }
        ]
      },
      "same_sha_repository_runs": {
        "query_returned": 2,
        "other_run_ids": [
          38873970340
        ],
        "conclusion_counts": {
          "cancelled": 1,
          "success": 1
        },
        "pairing_review": "Not a PR comparison; same SHA alone does not prove matching ranges/profiles/required coverage."
      },
      "workflow_file_sha256": {
        ".github/workflows/dependency-tests.yml": "215dd788507eb2fff04dd3ed70193126006ec642bf949b3689fc8b1415e486e2",
        ".github/workflows/dependency-broad.yml": "79d5ce5af5f517d037b83fc5d4c92d3d31172a5ef059202e0a4b8ffe00d383b6"
      }
    },
    {
      "id": 38878691465,
      "run_attempt": 1,
      "event": "push",
      "head_sha": "249f9657fb230947ba486fb3cd92894316f9acc3",
      "head_branch": "master",
      "status": "completed",
      "conclusion": "failure",
      "created_at": "2026-10-16T09:10:58Z",
      "run_started_at": "2026-10-16T09:10:58Z",
      "updated_at": "2026-10-16T09:53:52Z",
      "html_url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465",
      "workflow_id": 386168204,
      "path": ".github/workflows/dependency-tests.yml",
      "pull_request_numbers": [],
      "qualification": {
        "pr_comparison": "ineligible_event",
        "pr_reason": "No pull_request event/PR identity or reviewed legacy/selective pair; pushes/manual runs are not PR comparisons.",
        "broad_integration": "failed",
        "broad_reason": "Nested required Broad family accounting failed.",
        "accepted_pr_numbers": [],
        "accepted_broad": false,
        "required_workload_union_review": "not_accepted; accounting failure/absence is already disqualifying"
      },
      "job_count": 59,
      "job_conclusion_counts": {
        "success": 45,
        "failure": 13,
        "skipped": 1
      },
      "selection_and_worker_jobs": [
        {
          "id": 116899872177,
          "name": "select",
          "conclusion": "success",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116899872177"
        },
        {
          "id": 116899991146,
          "name": "groundhog_sanitizers / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116899991146"
        },
        {
          "id": 116899991148,
          "name": "grotto / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116899991148"
        },
        {
          "id": 116899991244,
          "name": "core / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116899991244"
        },
        {
          "id": 116899991264,
          "name": "groundhog / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116899991264"
        },
        {
          "id": 116899991307,
          "name": "core_asan / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116899991307"
        },
        {
          "id": 116899991318,
          "name": "mdk08 / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116899991318"
        },
        {
          "id": 116899991384,
          "name": "libgo / execute",
          "conclusion": "success",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116899991384"
        },
        {
          "id": 116899991387,
          "name": "policy / execute",
          "conclusion": "success",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116899991387"
        },
        {
          "id": 116899991439,
          "name": "libgo_native / execute",
          "conclusion": "success",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116899991439"
        },
        {
          "id": 116899992047,
          "name": "core_native / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116899992047"
        },
        {
          "id": 116915750038,
          "name": "Dependency gate",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116915750038"
        }
      ],
      "gate_job": {
        "id": 116915750038,
        "name": "Dependency gate",
        "conclusion": "failure",
        "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116915750038"
      },
      "plan": {
        "plan_id": "ef5fe08b9b15da727973302b5e13b6c79376062eba33b579d6d0b81da195425c",
        "inventory_hash": "e074ff04674faa90f3335b9a5177a5c236ebf5d74f5e8ab81cf4e8943c73a95a",
        "context": "ci",
        "effective_mode": "broad",
        "source_identity": {
          "kind": "committed",
          "sha": "249f9657fb230947ba486fb3cd92894316f9acc3"
        },
        "expected_lane_ids": [
          "core.linux",
          "core.linux.asan",
          "core.macos.asan",
          "grotto.linux",
          "groundhog.linux",
          "groundhog.linux.sanitizers",
          "groundhog.mdk08",
          "libgo.linux.asan",
          "libgo.macos.asan",
          "static.policy"
        ],
        "partial_profiles": [],
        "artifact_id": 12036446528
      },
      "validation": {
        "artifact_id": 12038423327,
        "member": "validation.json",
        "tested_sha": "249f9657fb230947ba486fb3cd92894316f9acc3",
        "passed": false,
        "complete_requested": true,
        "complete_gate_eligible": false,
        "error_count": 23,
        "first_errors": [
          "inventory coverage/enforcement readiness cannot satisfy complete gate",
          "selected worker job conclusions missing/skipped/failed/cancelled",
          "duplicate/unexpected lane: None"
        ]
      },
      "lane_receipts": [
        {
          "artifact_id": 12037146583,
          "member": "static.policy/receipt.json",
          "lane_id": "static.policy",
          "profile": "static.policy",
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "249f9657fb230947ba486fb3cd92894316f9acc3"
          },
          "job_conclusion": "success",
          "registration_audit": "passed",
          "input_audit": "passed",
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 1,
          "check_outcome_counts": {
            "passed": 1
          },
          "observed_execution_count": 1,
          "positive_checks": {
            "selection.contract": {
              "outcome": "passed",
              "observed_execution_count": 1
            }
          }
        },
        {
          "artifact_id": 12036653196,
          "member": "core.linux.asan/receipt.json",
          "lane_id": null,
          "profile": null,
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "249f9657fb230947ba486fb3cd92894316f9acc3"
          },
          "job_conclusion": null,
          "registration_audit": null,
          "input_audit": null,
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 0,
          "check_outcome_counts": {},
          "observed_execution_count": 0,
          "positive_checks": {}
        },
        {
          "artifact_id": 12036349109,
          "member": "core.macos.asan/receipt.json",
          "lane_id": null,
          "profile": null,
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "249f9657fb230947ba486fb3cd92894316f9acc3"
          },
          "job_conclusion": null,
          "registration_audit": null,
          "input_audit": null,
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 0,
          "check_outcome_counts": {},
          "observed_execution_count": 0,
          "positive_checks": {}
        },
        {
          "artifact_id": 12036264453,
          "member": "libgo.linux.asan/receipt.json",
          "lane_id": "libgo.linux.asan",
          "profile": "libgo.linux.asan",
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "249f9657fb230947ba486fb3cd92894316f9acc3"
          },
          "job_conclusion": "success",
          "registration_audit": "passed",
          "input_audit": "passed",
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 2,
          "check_outcome_counts": {
            "passed": 2
          },
          "observed_execution_count": 33,
          "positive_checks": {
            "libgo.channel": {
              "outcome": "passed",
              "observed_execution_count": 1
            },
            "libgo.native_standalone": {
              "outcome": "passed",
              "observed_execution_count": 32
            }
          }
        },
        {
          "artifact_id": 12035779772,
          "member": "libgo.macos.asan/receipt.json",
          "lane_id": "libgo.macos.asan",
          "profile": "libgo.macos.asan",
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "249f9657fb230947ba486fb3cd92894316f9acc3"
          },
          "job_conclusion": "success",
          "registration_audit": "passed",
          "input_audit": "passed",
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 2,
          "check_outcome_counts": {
            "passed": 2
          },
          "observed_execution_count": 32,
          "positive_checks": {
            "libgo.channel": {
              "outcome": "passed",
              "observed_execution_count": 1
            },
            "libgo.native_standalone": {
              "outcome": "passed",
              "observed_execution_count": 31
            }
          }
        }
      ],
      "artifacts": [
        {
          "artifact_id": 12038423327,
          "name": "dependency-validation-ef5fe08b9b15da727973302b5e13b6c79376062eba33b579d6d0b81da195425c-1",
          "expired": false,
          "metadata_size_bytes": 4894,
          "metadata_digest": "sha256:c09d2bf2bfc586edae4c43f0f24e3abab5445f1840d4aa1541b8c4e249a53a6e",
          "download_sha256": "c09d2bf2bfc586edae4c43f0f24e3abab5445f1840d4aa1541b8c4e249a53a6e",
          "download_size_bytes": 4894,
          "download_exit_code": 0,
          "role": "validation",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12038423327",
          "member_sha256": {
            "validation.json": "150999c9d3b3cb35679a3639cf7b97a47c3786cc096fe6b9bc6d7bb771922a3c"
          }
        },
        {
          "artifact_id": 12037146583,
          "name": "dependency-receipt-ef5fe08b9b15da727973302b5e13b6c79376062eba33b579d6d0b81da195425c-static.policy-1",
          "expired": false,
          "metadata_size_bytes": 20996,
          "metadata_digest": "sha256:3f01782ae7ea8c72a75af3869b1ae80e65eab7f947d31edfc612c2951dbb55d8",
          "download_sha256": "3f01782ae7ea8c72a75af3869b1ae80e65eab7f947d31edfc612c2951dbb55d8",
          "download_size_bytes": 20996,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12037146583",
          "member_sha256": {
            "static.policy/receipt.json": "62164fd907dd307e073170de6afb482a5db7799c67b24c7a907d54bb6e77a7eb",
            "static.policy/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12037010902,
          "name": "dependency-receipt-ef5fe08b9b15da727973302b5e13b6c79376062eba33b579d6d0b81da195425c-grotto.linux-1",
          "expired": false,
          "metadata_size_bytes": 2086,
          "metadata_digest": "sha256:364ce819dfe917d4c2adc4a36b18c312ee9d2e16ce129f05de80d0b56234d339",
          "download_sha256": "364ce819dfe917d4c2adc4a36b18c312ee9d2e16ce129f05de80d0b56234d339",
          "download_size_bytes": 2086,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12037010902",
          "member_sha256": {
            "grotto.linux/worker.log": "5f7348cbc88e7e0dcb44c275c7084fc300473f2e160c6aa9820743e807cdca65"
          }
        },
        {
          "artifact_id": 12036853940,
          "name": "dependency-receipt-ef5fe08b9b15da727973302b5e13b6c79376062eba33b579d6d0b81da195425c-groundhog.linux.sanitizers-1",
          "expired": false,
          "metadata_size_bytes": 244,
          "metadata_digest": "sha256:9514bfb9b49af672b8d5116d7107f41b9cba66ec53c0abd54c18b42de7533ec7",
          "download_sha256": "9514bfb9b49af672b8d5116d7107f41b9cba66ec53c0abd54c18b42de7533ec7",
          "download_size_bytes": 244,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12036853940",
          "member_sha256": {
            "groundhog.linux.sanitizers/worker.log": "e6d6319e1f3904807c413aec43dddcd5a935f6ec040b05d95fe02eb5e2a7582f"
          }
        },
        {
          "artifact_id": 12036847585,
          "name": "dependency-receipt-ef5fe08b9b15da727973302b5e13b6c79376062eba33b579d6d0b81da195425c-groundhog.mdk08-1",
          "expired": false,
          "metadata_size_bytes": 2111,
          "metadata_digest": "sha256:04fda5fab6ef6499c9d09525a58efb1dac10aae06ce7516c5724e8607dedf49d",
          "download_sha256": "04fda5fab6ef6499c9d09525a58efb1dac10aae06ce7516c5724e8607dedf49d",
          "download_size_bytes": 2111,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12036847585",
          "member_sha256": {
            "groundhog.mdk08/worker.log": "9b7df63ce591094a4d57151aec289394e68f59a778bd74003d21d2705d526898"
          }
        },
        {
          "artifact_id": 12036762746,
          "name": "dependency-receipt-ef5fe08b9b15da727973302b5e13b6c79376062eba33b579d6d0b81da195425c-core.linux-1",
          "expired": false,
          "metadata_size_bytes": 2076,
          "metadata_digest": "sha256:c37db5c991690742e643ab37dde00df0a61503c5315d4f99e0cd813fc628b9c1",
          "download_sha256": "c37db5c991690742e643ab37dde00df0a61503c5315d4f99e0cd813fc628b9c1",
          "download_size_bytes": 2076,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12036762746",
          "member_sha256": {
            "core.linux/worker.log": "f647884ad3f253de75cc3d4596809d501ed43d368edd514842168d95b85f890b"
          }
        },
        {
          "artifact_id": 12036653196,
          "name": "dependency-receipt-ef5fe08b9b15da727973302b5e13b6c79376062eba33b579d6d0b81da195425c-core.linux.asan-1",
          "expired": false,
          "metadata_size_bytes": 34641,
          "metadata_digest": "sha256:2c13708682ddefab42c9169de3cc334d19116bfd6279c37dc578a8abf91dcdc0",
          "download_sha256": "2c13708682ddefab42c9169de3cc334d19116bfd6279c37dc578a8abf91dcdc0",
          "download_size_bytes": 34641,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12036653196",
          "member_sha256": {
            "core.linux.asan/receipt.json": "4de4b3c65f559818d1afa46a5fa8c41662e4049f927b549ef6ce5557c0623202",
            "core.linux.asan/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12036638304,
          "name": "dependency-receipt-ef5fe08b9b15da727973302b5e13b6c79376062eba33b579d6d0b81da195425c-groundhog.linux-1",
          "expired": false,
          "metadata_size_bytes": 234,
          "metadata_digest": "sha256:5a8ca7e5958f48bf133b4436254a7a006c60fc9fed9b170d4d1eb5ba35cef176",
          "download_sha256": "5a8ca7e5958f48bf133b4436254a7a006c60fc9fed9b170d4d1eb5ba35cef176",
          "download_size_bytes": 234,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12036638304",
          "member_sha256": {
            "groundhog.linux/worker.log": "c3248197fc46c39acc93fb321661c79fa213f9f3ab8d6d7c86ed0ff9fb1d050e"
          }
        },
        {
          "artifact_id": 12036446528,
          "name": "dependency-plan-ef5fe08b9b15da727973302b5e13b6c79376062eba33b579d6d0b81da195425c-1",
          "expired": false,
          "metadata_size_bytes": 10774,
          "metadata_digest": "sha256:ed53641475f0bd33b30d52e2a25e49d8c6f42027d890f5e76d47697c3cc64285",
          "download_sha256": "ed53641475f0bd33b30d52e2a25e49d8c6f42027d890f5e76d47697c3cc64285",
          "download_size_bytes": 10774,
          "download_exit_code": 0,
          "role": "plan",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12036446528",
          "member_sha256": {
            "plan.json": "25aabe135c71bb69c86e16c1f28f441cdb2e489f21dec969cae158b866aa3c1a"
          }
        },
        {
          "artifact_id": 12036349109,
          "name": "dependency-receipt-ef5fe08b9b15da727973302b5e13b6c79376062eba33b579d6d0b81da195425c-core.macos.asan-1",
          "expired": false,
          "metadata_size_bytes": 34594,
          "metadata_digest": "sha256:59de5695381658d0f39da2accb1e6334ab20c22e684cb130eeb4a0c0315dc6dd",
          "download_sha256": "59de5695381658d0f39da2accb1e6334ab20c22e684cb130eeb4a0c0315dc6dd",
          "download_size_bytes": 34594,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12036349109",
          "member_sha256": {
            "core.macos.asan/receipt.json": "517976102c3525162898a9965c12bdcb3187b9e77771944d7ab2096d49f08a9a",
            "core.macos.asan/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12036264453,
          "name": "dependency-receipt-ef5fe08b9b15da727973302b5e13b6c79376062eba33b579d6d0b81da195425c-libgo.linux.asan-1",
          "expired": false,
          "metadata_size_bytes": 24300,
          "metadata_digest": "sha256:509b6f70a372c9f208d88c31e39607b1da260b28a4d0765be8604d3353a1d05e",
          "download_sha256": "509b6f70a372c9f208d88c31e39607b1da260b28a4d0765be8604d3353a1d05e",
          "download_size_bytes": 24300,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12036264453",
          "member_sha256": {
            "libgo.linux.asan/receipt.json": "2ac043618f0a468b08d1108927b9c4298994e12a2567bc8dd963b3122e823dd8",
            "libgo.linux.asan/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12035779772,
          "name": "dependency-receipt-ef5fe08b9b15da727973302b5e13b6c79376062eba33b579d6d0b81da195425c-libgo.macos.asan-1",
          "expired": false,
          "metadata_size_bytes": 27472,
          "metadata_digest": "sha256:a63a73ab8e0325c57d6393ac42b6ee0a0935417a2e518b14d2ba8faee33f4768",
          "download_sha256": "a63a73ab8e0325c57d6393ac42b6ee0a0935417a2e518b14d2ba8faee33f4768",
          "download_size_bytes": 27472,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12035779772",
          "member_sha256": {
            "libgo.macos.asan/receipt.json": "7b17538a1d3a4bcded8272d951e0e9a3823798b40093d20b13b6b2ea5e9cf2df",
            "libgo.macos.asan/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        }
      ],
      "broad": {
        "nested_job_count": 47,
        "job_conclusion_counts": {
          "success": 41,
          "failure": 5,
          "skipped": 1
        },
        "accounting_job": {
          "id": 116915712972,
          "name": "broad / Broad family accounting",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116915712972"
        },
        "failed_jobs": [
          {
            "id": 116899992180,
            "name": "broad / bp_extended / nightly-matrix",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116899992180"
          },
          {
            "id": 116899992250,
            "name": "broad / bp / bp-matrix",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116899992250"
          },
          {
            "id": 116899992256,
            "name": "broad / groundhog / groundhog-sanitizers",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116899992256"
          },
          {
            "id": 116902162597,
            "name": "broad / fuzz / Fuzz json-event",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116902162597"
          },
          {
            "id": 116915712972,
            "name": "broad / Broad family accounting",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116915712972"
          }
        ],
        "non_success_jobs": [
          {
            "id": 116905942425,
            "name": "broad / fuzz / Generate coverage report",
            "conclusion": "skipped",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/38878691465/job/116905942425"
          }
        ]
      },
      "same_sha_repository_runs": {
        "query_returned": 17,
        "other_run_ids": [
          39111022109,
          39109895428,
          39109224867,
          39015855938,
          39014974619,
          38878691249,
          38878691293,
          38878691258,
          38878691277,
          38878691312,
          38878691304,
          38878691362,
          38878691863,
          38878691317,
          38878691295,
          38878691261
        ],
        "conclusion_counts": {
          "success": 15,
          "failure": 2
        },
        "pairing_review": "Not a PR comparison; same SHA alone does not prove matching ranges/profiles/required coverage."
      },
      "workflow_file_sha256": {
        ".github/workflows/dependency-tests.yml": "215dd788507eb2fff04dd3ed70193126006ec642bf949b3689fc8b1415e486e2",
        ".github/workflows/dependency-broad.yml": "79d5ce5af5f517d037b83fc5d4c92d3d31172a5ef059202e0a4b8ffe00d383b6"
      }
    },
    {
      "id": 39132650260,
      "run_attempt": 1,
      "event": "push",
      "head_sha": "2743f17354ac13527d8d8919f81abc711396c959",
      "head_branch": "master",
      "status": "completed",
      "conclusion": "failure",
      "created_at": "2026-10-18T13:41:27Z",
      "run_started_at": "2026-10-18T13:41:27Z",
      "updated_at": "2026-10-18T14:24:47Z",
      "html_url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260",
      "workflow_id": 386168204,
      "path": ".github/workflows/dependency-tests.yml",
      "pull_request_numbers": [],
      "qualification": {
        "pr_comparison": "ineligible_event",
        "pr_reason": "No pull_request event/PR identity or reviewed legacy/selective pair; pushes/manual runs are not PR comparisons.",
        "broad_integration": "failed",
        "broad_reason": "Nested required Broad family accounting failed.",
        "accepted_pr_numbers": [],
        "accepted_broad": false,
        "required_workload_union_review": "not_accepted; accounting failure/absence is already disqualifying"
      },
      "job_count": 61,
      "job_conclusion_counts": {
        "success": 48,
        "failure": 12,
        "skipped": 1
      },
      "selection_and_worker_jobs": [
        {
          "id": 117690793121,
          "name": "select",
          "conclusion": "success",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117690793121"
        },
        {
          "id": 117691042406,
          "name": "grotto / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117691042406"
        },
        {
          "id": 117691042416,
          "name": "core_native / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117691042416"
        },
        {
          "id": 117691042489,
          "name": "groundhog / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117691042489"
        },
        {
          "id": 117691042494,
          "name": "mdk08 / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117691042494"
        },
        {
          "id": 117691042501,
          "name": "groundhog_sanitizers / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117691042501"
        },
        {
          "id": 117691042514,
          "name": "core / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117691042514"
        },
        {
          "id": 117691042549,
          "name": "grotto_asan / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117691042549"
        },
        {
          "id": 117691042569,
          "name": "policy / execute",
          "conclusion": "success",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117691042569"
        },
        {
          "id": 117691042625,
          "name": "core_asan / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117691042625"
        },
        {
          "id": 117691042628,
          "name": "libgo_native / execute",
          "conclusion": "success",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117691042628"
        },
        {
          "id": 117691042638,
          "name": "mdk011 / execute",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117691042638"
        },
        {
          "id": 117691042658,
          "name": "libgo / execute",
          "conclusion": "success",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117691042658"
        },
        {
          "id": 117700054727,
          "name": "Dependency gate",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117700054727"
        }
      ],
      "gate_job": {
        "id": 117700054727,
        "name": "Dependency gate",
        "conclusion": "failure",
        "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117700054727"
      },
      "plan": {
        "plan_id": "3590a9cf34ef97302aaf6d04f2f65efbe66b884a0e5092f4f8ad606a7234485c",
        "inventory_hash": "c2ef7ee5281253cae2df2da3fb85a2ef83b8d4f7bb21c1ce20abf8d986bcd9ab",
        "context": "ci",
        "effective_mode": "broad",
        "source_identity": {
          "kind": "committed",
          "sha": "2743f17354ac13527d8d8919f81abc711396c959"
        },
        "expected_lane_ids": [
          "core.linux",
          "core.linux.asan",
          "core.macos.asan",
          "grotto.linux",
          "grotto.linux.asan_ubsan",
          "groundhog.linux",
          "groundhog.linux.sanitizers",
          "groundhog.mdk011",
          "groundhog.mdk08",
          "libgo.linux.asan",
          "libgo.macos.asan",
          "static.policy"
        ],
        "partial_profiles": [],
        "artifact_id": 12138098321
      },
      "validation": {
        "artifact_id": 12140195214,
        "member": "validation.json",
        "tested_sha": "2743f17354ac13527d8d8919f81abc711396c959",
        "passed": false,
        "complete_requested": true,
        "complete_gate_eligible": false,
        "error_count": 467,
        "first_errors": [
          "inventory coverage/enforcement readiness cannot satisfy complete gate",
          "selected worker job conclusions missing/skipped/failed/cancelled",
          "core.linux: lane failure at configure"
        ]
      },
      "lane_receipts": [
        {
          "artifact_id": 12139515154,
          "member": "static.policy/receipt.json",
          "lane_id": "static.policy",
          "profile": "static.policy",
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "2743f17354ac13527d8d8919f81abc711396c959"
          },
          "job_conclusion": "success",
          "registration_audit": "passed",
          "input_audit": "passed",
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 1,
          "check_outcome_counts": {
            "passed": 1
          },
          "observed_execution_count": 1,
          "positive_checks": {
            "selection.contract": {
              "outcome": "passed",
              "observed_execution_count": 1
            }
          }
        },
        {
          "artifact_id": 12139196110,
          "member": "core.linux/receipt.json",
          "lane_id": "core.linux",
          "profile": "core.linux",
          "receipt_kind": "lane_failure_v1",
          "source_identity": {
            "kind": "committed",
            "sha": "2743f17354ac13527d8d8919f81abc711396c959"
          },
          "job_conclusion": "failure",
          "registration_audit": "not_performed",
          "input_audit": "not_completed",
          "complete_gate_eligible": false,
          "failure_stage": "configure",
          "failure_errors": [
            {
              "message": "profile build type Debug differs from realized ''",
              "type": "RealizationError"
            }
          ],
          "input_audit_gap_counts": {},
          "check_count": 3,
          "check_outcome_counts": {
            "missing": 3
          },
          "observed_execution_count": 0,
          "positive_checks": {}
        },
        {
          "artifact_id": 12139046572,
          "member": "groundhog.linux.sanitizers/receipt.json",
          "lane_id": "groundhog.linux.sanitizers",
          "profile": "groundhog.linux.sanitizers",
          "receipt_kind": "lane_failure_v1",
          "source_identity": {
            "kind": "committed",
            "sha": "2743f17354ac13527d8d8919f81abc711396c959"
          },
          "job_conclusion": "failure",
          "registration_audit": "not_performed",
          "input_audit": "not_completed",
          "complete_gate_eligible": false,
          "failure_stage": "resolve",
          "failure_errors": [
            {
              "message": "diagnostic nonmember fresh registration partition differs",
              "type": "RealizationError"
            }
          ],
          "input_audit_gap_counts": {},
          "check_count": 62,
          "check_outcome_counts": {
            "missing": 62
          },
          "observed_execution_count": 0,
          "positive_checks": {}
        },
        {
          "artifact_id": 12138776972,
          "member": "libgo.macos.asan/receipt.json",
          "lane_id": "libgo.macos.asan",
          "profile": "libgo.macos.asan",
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "2743f17354ac13527d8d8919f81abc711396c959"
          },
          "job_conclusion": "success",
          "registration_audit": "passed",
          "input_audit": "passed",
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 2,
          "check_outcome_counts": {
            "passed": 2
          },
          "observed_execution_count": 32,
          "positive_checks": {
            "libgo.channel": {
              "outcome": "passed",
              "observed_execution_count": 1
            },
            "libgo.native_standalone": {
              "outcome": "passed",
              "observed_execution_count": 31
            }
          }
        },
        {
          "artifact_id": 12138657091,
          "member": "groundhog.mdk011/receipt.json",
          "lane_id": "groundhog.mdk011",
          "profile": "groundhog.mdk011",
          "receipt_kind": "lane_failure_v1",
          "source_identity": {
            "kind": "committed",
            "sha": "2743f17354ac13527d8d8919f81abc711396c959"
          },
          "job_conclusion": "failure",
          "registration_audit": "not_performed",
          "input_audit": "not_completed",
          "complete_gate_eligible": false,
          "failure_stage": "capability",
          "failure_errors": [
            {
              "message": "live peer/image lifecycle requires the existing guarded adapter",
              "type": "CheckFailure"
            }
          ],
          "input_audit_gap_counts": {},
          "check_count": 16,
          "check_outcome_counts": {
            "missing": 16
          },
          "observed_execution_count": 0,
          "positive_checks": {}
        },
        {
          "artifact_id": 12138638626,
          "member": "core.linux.asan/receipt.json",
          "lane_id": "core.linux.asan",
          "profile": "core.linux.asan",
          "receipt_kind": "lane_failure_v1",
          "source_identity": {
            "kind": "committed",
            "sha": "2743f17354ac13527d8d8919f81abc711396c959"
          },
          "job_conclusion": "failure",
          "registration_audit": "failed",
          "input_audit": "failed",
          "complete_gate_eligible": false,
          "failure_stage": "preaudit",
          "failure_errors": [
            {
              "message": "pre-build input audit failed; original findings retained",
              "type": "CheckFailure"
            }
          ],
          "input_audit_gap_counts": {
            "unclaimed_registration": 3
          },
          "check_count": 48,
          "check_outcome_counts": {
            "missing": 48
          },
          "observed_execution_count": 0,
          "positive_checks": {}
        },
        {
          "artifact_id": 12138633221,
          "member": "groundhog.linux/receipt.json",
          "lane_id": "groundhog.linux",
          "profile": "groundhog.linux",
          "receipt_kind": "lane_failure_v1",
          "source_identity": {
            "kind": "committed",
            "sha": "2743f17354ac13527d8d8919f81abc711396c959"
          },
          "job_conclusion": "failure",
          "registration_audit": "failed",
          "input_audit": "failed",
          "complete_gate_eligible": false,
          "failure_stage": "preaudit",
          "failure_errors": [
            {
              "message": "pre-build input audit failed; original findings retained",
              "type": "CheckFailure"
            }
          ],
          "input_audit_gap_counts": {
            "unclaimed_registration": 408,
            "missing_impact_edge": 9473,
            "check_descriptor_owner_disagreement": 1,
            "target_mapping": 1
          },
          "check_count": 10,
          "check_outcome_counts": {
            "missing": 10
          },
          "observed_execution_count": 0,
          "positive_checks": {}
        },
        {
          "artifact_id": 12138577637,
          "member": "grotto.linux.asan_ubsan/receipt.json",
          "lane_id": "grotto.linux.asan_ubsan",
          "profile": "grotto.linux.asan_ubsan",
          "receipt_kind": "lane_failure_v1",
          "source_identity": {
            "kind": "committed",
            "sha": "2743f17354ac13527d8d8919f81abc711396c959"
          },
          "job_conclusion": "failure",
          "registration_audit": "failed",
          "input_audit": "failed",
          "complete_gate_eligible": false,
          "failure_stage": "preaudit",
          "failure_errors": [
            {
              "message": "pre-build input audit failed; original findings retained",
              "type": "CheckFailure"
            }
          ],
          "input_audit_gap_counts": {
            "unclaimed_registration": 322,
            "target_mapping": 1,
            "missing_impact_edge": 330,
            "generated_runtime_without_target": 2
          },
          "check_count": 4,
          "check_outcome_counts": {
            "missing": 4
          },
          "observed_execution_count": 0,
          "positive_checks": {}
        },
        {
          "artifact_id": 12138467933,
          "member": "groundhog.mdk08/receipt.json",
          "lane_id": "groundhog.mdk08",
          "profile": "groundhog.mdk08",
          "receipt_kind": "lane_failure_v1",
          "source_identity": {
            "kind": "committed",
            "sha": "2743f17354ac13527d8d8919f81abc711396c959"
          },
          "job_conclusion": "failure",
          "registration_audit": "not_performed",
          "input_audit": "not_completed",
          "complete_gate_eligible": false,
          "failure_stage": "configure",
          "failure_errors": [
            {
              "message": "profile build type Debug differs from realized ''",
              "type": "RealizationError"
            }
          ],
          "input_audit_gap_counts": {},
          "check_count": 1,
          "check_outcome_counts": {
            "missing": 1
          },
          "observed_execution_count": 0,
          "positive_checks": {}
        },
        {
          "artifact_id": 12138424589,
          "member": "grotto.linux/receipt.json",
          "lane_id": "grotto.linux",
          "profile": "grotto.linux",
          "receipt_kind": "lane_failure_v1",
          "source_identity": {
            "kind": "committed",
            "sha": "2743f17354ac13527d8d8919f81abc711396c959"
          },
          "job_conclusion": "failure",
          "registration_audit": "failed",
          "input_audit": "failed",
          "complete_gate_eligible": false,
          "failure_stage": "preaudit",
          "failure_errors": [
            {
              "message": "pre-build input audit failed; original findings retained",
              "type": "CheckFailure"
            }
          ],
          "input_audit_gap_counts": {
            "unclaimed_registration": 402,
            "missing_impact_edge": 9104,
            "generated_runtime_without_target": 2
          },
          "check_count": 4,
          "check_outcome_counts": {
            "missing": 4
          },
          "observed_execution_count": 0,
          "positive_checks": {}
        },
        {
          "artifact_id": 12138414686,
          "member": "libgo.linux.asan/receipt.json",
          "lane_id": "libgo.linux.asan",
          "profile": "libgo.linux.asan",
          "receipt_kind": null,
          "source_identity": {
            "kind": "committed",
            "sha": "2743f17354ac13527d8d8919f81abc711396c959"
          },
          "job_conclusion": "success",
          "registration_audit": "passed",
          "input_audit": "passed",
          "complete_gate_eligible": false,
          "failure_stage": null,
          "failure_errors": [],
          "input_audit_gap_counts": {},
          "check_count": 2,
          "check_outcome_counts": {
            "passed": 2
          },
          "observed_execution_count": 33,
          "positive_checks": {
            "libgo.channel": {
              "outcome": "passed",
              "observed_execution_count": 1
            },
            "libgo.native_standalone": {
              "outcome": "passed",
              "observed_execution_count": 32
            }
          }
        },
        {
          "artifact_id": 12138179654,
          "member": "core.macos.asan/receipt.json",
          "lane_id": "core.macos.asan",
          "profile": "core.macos.asan",
          "receipt_kind": "lane_failure_v1",
          "source_identity": {
            "kind": "committed",
            "sha": "2743f17354ac13527d8d8919f81abc711396c959"
          },
          "job_conclusion": "failure",
          "registration_audit": "failed",
          "input_audit": "failed",
          "complete_gate_eligible": false,
          "failure_stage": "preaudit",
          "failure_errors": [
            {
              "message": "pre-build input audit failed; original findings retained",
              "type": "CheckFailure"
            }
          ],
          "input_audit_gap_counts": {
            "unclaimed_registration": 3
          },
          "check_count": 48,
          "check_outcome_counts": {
            "missing": 48
          },
          "observed_execution_count": 0,
          "positive_checks": {}
        }
      ],
      "artifacts": [
        {
          "artifact_id": 12140195214,
          "name": "dependency-validation-3590a9cf34ef97302aaf6d04f2f65efbe66b884a0e5092f4f8ad606a7234485c-1",
          "expired": false,
          "metadata_size_bytes": 510512,
          "metadata_digest": "sha256:a190e29a0e3e242e9125db7d8a905c13c1bd325e3a84c1b28d697e988ae10f4a",
          "download_sha256": "a190e29a0e3e242e9125db7d8a905c13c1bd325e3a84c1b28d697e988ae10f4a",
          "download_size_bytes": 510512,
          "download_exit_code": 0,
          "role": "validation",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12140195214",
          "member_sha256": {
            "validation.json": "fb2a85ee6e84a1242ed87a7b89c5a4d8aac31833000a31235661516cd237ab05"
          }
        },
        {
          "artifact_id": 12139515154,
          "name": "dependency-receipt-3590a9cf34ef97302aaf6d04f2f65efbe66b884a0e5092f4f8ad606a7234485c-static.policy-1",
          "expired": false,
          "metadata_size_bytes": 22105,
          "metadata_digest": "sha256:763fb1481bbfe61469699a0578260dd23f8dad63470a80680058081e430d71c2",
          "download_sha256": "763fb1481bbfe61469699a0578260dd23f8dad63470a80680058081e430d71c2",
          "download_size_bytes": 22105,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12139515154",
          "member_sha256": {
            "static.policy/receipt.json": "6979128aa356ff102413ef0385e1703904f23d54e8cd91d1f5c5ee82091518a6",
            "static.policy/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12139196110,
          "name": "dependency-receipt-3590a9cf34ef97302aaf6d04f2f65efbe66b884a0e5092f4f8ad606a7234485c-core.linux-1",
          "expired": false,
          "metadata_size_bytes": 5628,
          "metadata_digest": "sha256:2320fef83a4af6543ac7074d5b1fa118917434bb19f42d822f672210770ec938",
          "download_sha256": "2320fef83a4af6543ac7074d5b1fa118917434bb19f42d822f672210770ec938",
          "download_size_bytes": 5628,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12139196110",
          "member_sha256": {
            "core.linux/receipt.json": "a08b10d71c8e83de07d668962a72cbe7e2718bdd1c8d7d3b4bda38c7d4ff1ddf",
            "core.linux/worker.log": "2709cb6f0bb486e513f02d6fb2b573e9f9885baa3268e8daf66e72594d85fe5f"
          }
        },
        {
          "artifact_id": 12139046572,
          "name": "dependency-receipt-3590a9cf34ef97302aaf6d04f2f65efbe66b884a0e5092f4f8ad606a7234485c-groundhog.linux.sanitizers-1",
          "expired": false,
          "metadata_size_bytes": 7379,
          "metadata_digest": "sha256:9d827bacb3ca61d0d5dc518c0330c59a9e0f0f2ca738404ba9ad33d492e94dfe",
          "download_sha256": "9d827bacb3ca61d0d5dc518c0330c59a9e0f0f2ca738404ba9ad33d492e94dfe",
          "download_size_bytes": 7379,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12139046572",
          "member_sha256": {
            "groundhog.linux.sanitizers/receipt.json": "47dd5a471b5a2fcaee9906e74145f6714c491b4294c3fe14067a1ebdeaaac11d",
            "groundhog.linux.sanitizers/worker.log": "389e76c63f8828b5a7a5f6faf9587d06aff79d72a44ecab7151c0eaac496496c"
          }
        },
        {
          "artifact_id": 12138776972,
          "name": "dependency-receipt-3590a9cf34ef97302aaf6d04f2f65efbe66b884a0e5092f4f8ad606a7234485c-libgo.macos.asan-1",
          "expired": false,
          "metadata_size_bytes": 27753,
          "metadata_digest": "sha256:e47302b122455ecd016df8ede806437758edf4e6799657c7297430d397647c51",
          "download_sha256": "e47302b122455ecd016df8ede806437758edf4e6799657c7297430d397647c51",
          "download_size_bytes": 27753,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12138776972",
          "member_sha256": {
            "libgo.macos.asan/receipt.json": "fcca8e3d3c6bda0a7aad8ae354a687efc6b654bcdea6f29d60f9338c94d76ae5",
            "libgo.macos.asan/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12138657091,
          "name": "dependency-receipt-3590a9cf34ef97302aaf6d04f2f65efbe66b884a0e5092f4f8ad606a7234485c-groundhog.mdk011-1",
          "expired": false,
          "metadata_size_bytes": 16372,
          "metadata_digest": "sha256:47281329eeb1a3f4843234f43b30e650bb77da6493f196d6b4cc8b5190ffbf30",
          "download_sha256": "47281329eeb1a3f4843234f43b30e650bb77da6493f196d6b4cc8b5190ffbf30",
          "download_size_bytes": 16372,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12138657091",
          "member_sha256": {
            "groundhog.mdk011/receipt.json": "72c417b73ca074e05cb1ed9d63912be2623c9fa9c8c9c2dcff59f4e057d69274",
            "groundhog.mdk011/worker.log": "c3248197fc46c39acc93fb321661c79fa213f9f3ab8d6d7c86ed0ff9fb1d050e"
          }
        },
        {
          "artifact_id": 12138638626,
          "name": "dependency-receipt-3590a9cf34ef97302aaf6d04f2f65efbe66b884a0e5092f4f8ad606a7234485c-core.linux.asan-1",
          "expired": false,
          "metadata_size_bytes": 57851,
          "metadata_digest": "sha256:43cb9d13ea132282b4a2933daa5282406f0901b1e37203574cd2dac0f1a8091c",
          "download_sha256": "43cb9d13ea132282b4a2933daa5282406f0901b1e37203574cd2dac0f1a8091c",
          "download_size_bytes": 57851,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12138638626",
          "member_sha256": {
            "core.linux.asan/receipt.json": "63f8a77e5c63d61cdc6bca17fe5b1e97ce3448af10cacfba58e40cef6ea05672",
            "core.linux.asan/worker.log": "173430c24f2ae508d76f8d596ffe08f3a12825f6f07420e0018ddb2d32374e15"
          }
        },
        {
          "artifact_id": 12138633221,
          "name": "dependency-receipt-3590a9cf34ef97302aaf6d04f2f65efbe66b884a0e5092f4f8ad606a7234485c-groundhog.linux-1",
          "expired": false,
          "metadata_size_bytes": 176345,
          "metadata_digest": "sha256:3764ed320d7ceb848ff59b66762996de548a7105844e91793071e5502ba90517",
          "download_sha256": "3764ed320d7ceb848ff59b66762996de548a7105844e91793071e5502ba90517",
          "download_size_bytes": 176345,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12138633221",
          "member_sha256": {
            "groundhog.linux/receipt.json": "3cc5042f5821ce8db35bcc0e7d156acb7b47112be26f2e80bd924e1c9c90d1e5",
            "groundhog.linux/worker.log": "173430c24f2ae508d76f8d596ffe08f3a12825f6f07420e0018ddb2d32374e15"
          }
        },
        {
          "artifact_id": 12138577637,
          "name": "dependency-receipt-3590a9cf34ef97302aaf6d04f2f65efbe66b884a0e5092f4f8ad606a7234485c-grotto.linux.asan_ubsan-1",
          "expired": false,
          "metadata_size_bytes": 96535,
          "metadata_digest": "sha256:190646958bf1e1a1d77e0d8dc07cdca8583c49d0b13a2693a14723f544aa7565",
          "download_sha256": "190646958bf1e1a1d77e0d8dc07cdca8583c49d0b13a2693a14723f544aa7565",
          "download_size_bytes": 96535,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12138577637",
          "member_sha256": {
            "grotto.linux.asan_ubsan/receipt.json": "4d04dca81a81f990d3607449c63a41ccb9d2f146d421d2ed93b6ce81f4c96e22",
            "grotto.linux.asan_ubsan/worker.log": "173430c24f2ae508d76f8d596ffe08f3a12825f6f07420e0018ddb2d32374e15"
          }
        },
        {
          "artifact_id": 12138467933,
          "name": "dependency-receipt-3590a9cf34ef97302aaf6d04f2f65efbe66b884a0e5092f4f8ad606a7234485c-groundhog.mdk08-1",
          "expired": false,
          "metadata_size_bytes": 5588,
          "metadata_digest": "sha256:87797abab8f814def91e83cfe376fbfc30fd7daa2ddb1e52814a5df76b599b0b",
          "download_sha256": "87797abab8f814def91e83cfe376fbfc30fd7daa2ddb1e52814a5df76b599b0b",
          "download_size_bytes": 5588,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12138467933",
          "member_sha256": {
            "groundhog.mdk08/receipt.json": "d8b494cd11f97f3a0b4063ec4888a8b63c48c4a4299475c4e494e70b437e9a67",
            "groundhog.mdk08/worker.log": "2709cb6f0bb486e513f02d6fb2b573e9f9885baa3268e8daf66e72594d85fe5f"
          }
        },
        {
          "artifact_id": 12138424589,
          "name": "dependency-receipt-3590a9cf34ef97302aaf6d04f2f65efbe66b884a0e5092f4f8ad606a7234485c-grotto.linux-1",
          "expired": false,
          "metadata_size_bytes": 192926,
          "metadata_digest": "sha256:1d16caad632dc3fa33a2719d34b78357f98026007dbc43d15e45f6ff8d3957d9",
          "download_sha256": "1d16caad632dc3fa33a2719d34b78357f98026007dbc43d15e45f6ff8d3957d9",
          "download_size_bytes": 192926,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12138424589",
          "member_sha256": {
            "grotto.linux/receipt.json": "07b0649aba3cd9d0039b1473cfa0882b3a88250596f33a8e68455dacd12c95e2",
            "grotto.linux/worker.log": "173430c24f2ae508d76f8d596ffe08f3a12825f6f07420e0018ddb2d32374e15"
          }
        },
        {
          "artifact_id": 12138414686,
          "name": "dependency-receipt-3590a9cf34ef97302aaf6d04f2f65efbe66b884a0e5092f4f8ad606a7234485c-libgo.linux.asan-1",
          "expired": false,
          "metadata_size_bytes": 24613,
          "metadata_digest": "sha256:206cf7385d089a737bf6f583d0de7eb862af46b068e0a0c82d96c22ba695adac",
          "download_sha256": "206cf7385d089a737bf6f583d0de7eb862af46b068e0a0c82d96c22ba695adac",
          "download_size_bytes": 24613,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12138414686",
          "member_sha256": {
            "libgo.linux.asan/receipt.json": "82a68ee215f72015ef1e2900904eccc23e88b3c5ea37aaaa0fefee59d8c584b8",
            "libgo.linux.asan/worker.log": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
          }
        },
        {
          "artifact_id": 12138179654,
          "name": "dependency-receipt-3590a9cf34ef97302aaf6d04f2f65efbe66b884a0e5092f4f8ad606a7234485c-core.macos.asan-1",
          "expired": false,
          "metadata_size_bytes": 57895,
          "metadata_digest": "sha256:3d82cbe3544f555a0bc4117ceab170a7077f1320b522bbf2bdedb18762d673e2",
          "download_sha256": "3d82cbe3544f555a0bc4117ceab170a7077f1320b522bbf2bdedb18762d673e2",
          "download_size_bytes": 57895,
          "download_exit_code": 0,
          "role": "lane_receipt",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12138179654",
          "member_sha256": {
            "core.macos.asan/receipt.json": "624a133e74ccb1aab18940a4c2c1036757dad9c134d5c4ae6e331c61af82b513",
            "core.macos.asan/worker.log": "173430c24f2ae508d76f8d596ffe08f3a12825f6f07420e0018ddb2d32374e15"
          }
        },
        {
          "artifact_id": 12138098321,
          "name": "dependency-plan-3590a9cf34ef97302aaf6d04f2f65efbe66b884a0e5092f4f8ad606a7234485c-1",
          "expired": false,
          "metadata_size_bytes": 9147,
          "metadata_digest": "sha256:1d0e7ad8d510aaa5f105d61b2c8992691240d52961715b1f3829383e99628703",
          "download_sha256": "1d0e7ad8d510aaa5f105d61b2c8992691240d52961715b1f3829383e99628703",
          "download_size_bytes": 9147,
          "download_exit_code": 0,
          "role": "plan",
          "api_url": "https://api.github.com/repos/chebizarro/nostrc/actions/artifacts/12138098321",
          "member_sha256": {
            "plan.json": "b147c6654944cca6c7ee85d2a9383695199aecacd628f5933ef287619b7a185b"
          }
        }
      ],
      "broad": {
        "nested_job_count": 47,
        "job_conclusion_counts": {
          "success": 44,
          "failure": 2,
          "skipped": 1
        },
        "accounting_job": {
          "id": 117700034296,
          "name": "broad / Broad family accounting",
          "conclusion": "failure",
          "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117700034296"
        },
        "failed_jobs": [
          {
            "id": 117691683008,
            "name": "broad / fuzz / Fuzz json-event",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117691683008"
          },
          {
            "id": 117700034296,
            "name": "broad / Broad family accounting",
            "conclusion": "failure",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117700034296"
          }
        ],
        "non_success_jobs": [
          {
            "id": 117694708765,
            "name": "broad / fuzz / Generate coverage report",
            "conclusion": "skipped",
            "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117694708765"
          }
        ]
      },
      "same_sha_repository_runs": {
        "query_returned": 12,
        "other_run_ids": [
          39132650008,
          39132649987,
          39132650081,
          39132650000,
          39132649950,
          39132650144,
          39132650026,
          39132650266,
          39132650074,
          39132650119,
          39132650079
        ],
        "conclusion_counts": {
          "success": 11,
          "failure": 1
        },
        "pairing_review": "Not a PR comparison; same SHA alone does not prove matching ranges/profiles/required coverage."
      },
      "workflow_file_sha256": {
        ".github/workflows/dependency-tests.yml": "a18010ceab7be2d96632ef8266c24fcb2e71e970dbdb4c09905ad17265f45f15",
        ".github/workflows/dependency-broad.yml": "79d5ce5af5f517d037b83fc5d4c92d3d31172a5ef059202e0a4b8ffe00d383b6"
      }
    }
  ],
  "artifact_integrity": {
    "archives_checked": 59,
    "published_digests_matching": 59,
    "crc_failures": 0
  },
  "latest_broad_failure_log": {
    "run_id": 39132650260,
    "job_id": 117691683008,
    "url": "https://github.com/chebizarro/nostrc/actions/runs/39132650260/job/117691683008",
    "raw_log_sha256": "ce24a08fc9b1b4fef01091d8ce4f9878f44fca74c46c86681e36f04bc2f89d7f",
    "observed": "LeakSanitizer514 bytes/15 allocations through json_parser_load_from_data and fuzz_json_event.c:55; exit77. Input leak-7396472d70f69483739531904dd1db7271706023.",
    "cause_limit": "Actual observed call path, not a newly established upstream root cause or remedy.",
    "coverage_report_job": {
      "id": 117694708765,
      "conclusion": "skipped",
      "not_a_pass": true
    }
  }
}
```
<!-- acceptance-ledger-json:end -->
