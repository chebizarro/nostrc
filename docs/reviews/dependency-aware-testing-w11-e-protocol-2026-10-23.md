# E: physical execution and collection interface — 2026-10-23

Finite `nostrc-0cfsg.36.6`, base `44044fefe9c40ac095044e9c22eb06f2678aab1f`.
Internal tooling/tests/evidence only: **no component bump**. Parent lands local
commits. **W11 NOT MET; W12 NOT RUN.** No active hook, trigger, schedule,
protection or publisher setting is changed by this batch.

## What runs now, and what does not

`w11-evidence.py census` and `collect` actually ran against GitHub; the dated
ledger records their results and original ZIP proofs. The physical `execute`
path is exercised by small local process/Git fixtures, not a product build or
acceptance comparison. Host has approximately 5 GiB free and Docker read-only
ping times out: **no heavy local build, Docker, restart, prune or source copy**.

Real full-profile execution still depends on B's fresh normal full-Go source
contract (not fixed historical product hashes), A's complete Linux runtime and
scoped ephemeral hosted AppArmor provisioning/cleanup, and accepted native
custody/profile evidence. C's `cbf3d14a32204f647e2a7157b8cbea3f42c128b5` is a
frozen diagnostic, not accepted native custody. E does not alter those files or
route around those blockers. No extra manual workflow is added before a real
reviewed complete recipe exists; existing broad/fuzz dispatches already run.

## Bounded hosted collector — runnable now

```sh
python3 scripts/w11-evidence.py census --output /tmp/w11-roots-NEW
python3 scripts/w11-evidence.py collect --run 39922309846 --attempt 1 \
  --artifact 12493552566 --artifact 12488212907 --output /tmp/w11-broad-NEW
python3 scripts/w11-evidence.py collect --run 39922316626 --attempt 1 \
  --output /tmp/w11-fuzz-NEW
```

Every output directory must be new. Collection makes one bounded pass, never
waits/retries/dispatched work. Jobs use `/runs/ID/attempts/N/jobs`, complete
pagination and duplicate/run/attempt checks; root metadata is reread to disclose
drift. Artifact IDs are explicitly requested; default is metadata only. Original
ZIPs require API SHA256 and size, CRC-checked member reads, unique members and an
8 MiB compressed/expanded budget per archive. No extraction, large installer
fetches or redownload of all 54 historical archives. Root artifact ownership is
**not** silently converted into attempt ownership. Raw API bytes, physical `gh`
argv/time/exit/streams, original ZIP and member digests are retained, including
failures. `--job-log ID` additionally retains explicit completed job logs from the
same attempt. Their API supplies no content digest: locally captured log SHA256
is distinguished from the artifacts' authoritative API checksum. Runner script/Python fingerprints and start time are in `runner.json`.

Job receipts retain runner IDs/names/labels, every step and observed job duration.
Observed allocated job-time sums are not billing records, phase decomposition,
DAG critical paths, successful integrations or reviewed PR comparisons. The API
head for a PR is not its tested merge: inspect original plan/checkout evidence.

## Physical execution protocol — native runner after parent review

Both routes execute **the same genuine original PR source**, not a current
engine against historical product files and not a transplanted change. Before
qualifying any historical case, parent must accept this method and each original
legacy-triggered workload/selection recipe. PR-head or canonical-merge execution
is explicitly distinguished from the original Actions-tested merge. The runner
fetches the real PR and commit APIs, verifies local commit/tree/parents, retains
original binary diff (PR-head uses retained merge-base; canonical merge uses its
first-parent integration diff) and rejects unrelated/transplanted candidate SHAs. This
provides provenance, not a claim that historical broken recipes will succeed.

For each accepted case, use already provisioned clean checkouts at the identical
candidate SHA/submodule identities. Do not modify product sources or substitute
current policy. Use separate fresh output directories per route/sample/attempt.
Prepare public `contract.json` objects with:

- `kind=w11_physical_contract_v1`, `candidate_sha`, `condition`, `cache_state`,
  `route`, positive `sample`, shared `pair_id`, and exact `profile`;
- `environment`: explicit public PATH/HOME/TMPDIR and authored compiler/sanitizer
  settings; no inherited environment, Git routing, skip controls or credentials;
- `input_bindings`: repository-relative committed recipe/profile/capability/input
  paths mapped to actual SHA256 bytes (full source identity is also retained);
- `tool_probes`: actual compiler/CMake/Ninja/etc version `argv` and executable
  SHA256, not claimed versions inferred from profile names;
- `phases`: exact reviewed setup/configure/build/test command arrays, each with
  `phase`, `argv`, `executable_sha256`, positive `timeout_seconds`; **or one intact
  `workload` command** where the production adapter owns phase ordering. Never
  split a `--phase all` contract or insert no-ops to invent phase timings;
- `cache_evidence`: independently retained initial clean/incremental cache
  identity and producer evidence; labels alone are not verified cache state;
- optional `retained_outputs`: exact relative original result/plan/audit files,
  retained byte-for-byte within an 8 MiB total budget. Missing files are visible.

```sh
python3 scripts/w11-evidence.py execute --repo "$EXISTING_NATIVE_CHECKOUT" \
  --pull "$REAL_PR_NUMBER" --contract "$REVIEWED_LEGACY_CONTRACT" \
  --output "$NEW_LEGACY_EVIDENCE"
python3 scripts/w11-evidence.py execute --repo "$EXISTING_NATIVE_CHECKOUT" \
  --pull "$REAL_PR_NUMBER" --contract "$REVIEWED_SELECTED_CONTRACT" \
  --output "$NEW_SELECTED_EVIDENCE"
python3 scripts/w11-evidence.py measurements \
  --sample "$NEW_LEGACY_EVIDENCE/result.json" \
  --sample "$NEW_SELECTED_EVIDENCE/result.json" --output "$NEW_REPORT"
```

Execution holds a nonblocking repository-common lock. It fingerprints physical
executables and probes versions, checks actual committed source bytes/modes/submodules
before/after each command (independent of index assume-unchanged flags), retains exact public environment, argv, exit, original
stdout/stderr and measured monotonic phase/wall time. A failed phase stops the
remaining phases, records `not_run`, and returns nonzero. Timeout/cancellation
retains failures and kills the owned process group; **this is not kernel
custody or descendant-cleanup acceptance**. Existing inner custody must retain
its own original receipt. Source checks are **endpoint-only**: a command that
modifies and restores bytes within its phase is not detected; receipts explicitly
set `source_immutability_verified=false`. Immutable execution/within-phase drift
proof must come from the separately accepted inner custody, not this runner.
Retries require new evidence; they do not erase failed attempts. The runner makes no source copies and provisions no service itself.

Reporting accounts all **14 cells**: seven representative conditions, each clean
and incremental. At least five successful distinct paired samples are needed for
median, min/max range, linear `(n-1)` IQR and per-phase descriptive statistics.
Pairs must match exact source/submodules, profile, native runner and PR provenance;
duplicate/retry sample identities refuse and failed/unpaired samples stay visible.
Also pass each preflight failure's `failure.json` via `--sample`; the report retains
its error and contract hash separately rather than converting it into a timing.
Single intact workloads disclose missing phase decomposition. Serial wall savings
are **not** the required aggregate runner-minute savings or critical-path proof.

The material remaining acceptance work is real, not a documentation quota:
20 reviewed distinct representative PR executions, two complete broad integrations,
all differences classified (legacy-only failures require regression fixtures),
reviewed complete trigger/job/coverage accounting, cache-independent samples,
>=30% Grotto / >=25% isolated non-MLS Groundhog aggregate runner-minute reduction,
zero unrelated live MDK, <=10% critical-path regression and broad median <=10%
regression or explicit measured coverage justification. Selected skip/cancel,
fork/PR, enabled merge-group and release dryrun proof remain required. This tool
never promotes `complete=true`, local fixtures, planner cases or job colours.
If faithful original-source cases cannot satisfy the contract, genuine future
PRs are required; do not manufacture PRs or waive the criteria.

## Exact conditional W12 support — not applied

```sh
python3 scripts/w11-evidence.py prepare-cutover --repo . \
  --sha 44044fefe9c40ac095044e9c22eb06f2678aab1f \
  --protection /tmp/nostrc-w11-e-discovery-20261023/protection.stdout \
  --output /tmp/w12-draft-NEW
git apply --check /tmp/w12-draft-NEW/w12-forward.patch
```

The checked-in forward/reverse patch and manifest are reproducible draft bytes
for that source, **not permission to apply**. Regenerate/review against the final
accepted source; compare every source binding before application. One repository
patch changes hook + AGENTS + TESTING together, disables only duplicate legacy
validation push/PR triggers and the three legacy schedules **only after** their
full workloads/variants are proven in existing master/release/manual broad calls
(W9). Owner policy has **no scheduled tier**: no replacement cron or schedule
route is added. Reusable/manual entry points, Windows tag publication and all
release workflows remain. The full broad local
override remains `NOSTRC_FULL_GATE=1 git push`; `NOSTRC_SELECTIVE_GATE=0` cannot
silently disable the new default. No release-mode flip is included.

At the retained read-only discovery snapshot, rulesets were `[]`, master branch
protection returned **404 Branch not protected**, caller API permissions included
admin/push, and actual PR21 check-run name/app was `Dependency gate` / GitHub
Actions **15368** (failed). No existing protected-context set or enabled merge
queue is inferred. Current absence of protection is not an approved new policy.
Thus **server transition payload generation is blocked** until the owner supplies
an approved protected-branch baseline and actual obsolete validation contexts.
Do not invent review/admin settings or install branch protection from defaults.

With that baseline, `--protection GET.json --app-id 15368
--obsolete-context 'OBSERVED REVIEWED LEGACY CONTEXT'` generates exact PATCH
bodies for `/repos/chebizarro/nostrc/branches/master/protection/required_status_checks`.
Only explicitly named old contexts from that observed app can be removed;
unrelated checks/apps and `strict` are preserved. No HTTP write is performed.
After original W11 + parent/operator acceptance: refresh rulesets/protection,
add new status **alongside old** (`required-dual.json`), GET-verify union, verify
real PR/fork and enabled merge-group behavior, then remove only obsolete contexts
(`required-selected.json`) and land the reviewed atomic repository patch.
Never remove legacy triggers before their statuses are no longer required.
Rollback first restores old broad requirements alongside new
(`required-rollback-dual.json`), restores the repository patch/triggers, verifies
real broad jobs/manual override, then removes new
(`required-rollback-legacy.json`). Re-read live protection before every write;
any drift, unknown ruleset, absent baseline or failed observation stops transition.

One substantive bounded Oracle source-capsule review found three actionable
issues: overclaimed within-phase source immutability, diverged PR diff provenance,
and loss of later cleanup output. Receipts/docs now disclose endpoint-only source
verification; PR-head diffs use retained merge-base; timeout/interruption retains
the second communication timeout's cumulative streams. Dedicated regressions
cover these cases. The first Oracle request received no selected files (exporter
pending/0 tokens), so no whole-project review is claimed.

Tests cover real small physical processes and negative source/tool/pair/lock/API
cases plus unapplied patch applicability and preservation of unrelated statuses.
They are not the required product/profile, broad-override or hosted rollback tests.
