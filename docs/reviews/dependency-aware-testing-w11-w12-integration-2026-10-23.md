# W11/W12 and heartbeat: bounded integration (2026-10-23)

## Exact sources and scope

Baseline master: `587ebea254fb3b7c19c8f91a42b23a8a4d950db3`.
Approved ancestry is preserved (not squashed):

| Component | Exact approved tip | Limit |
|---|---|---|
| W12 readiness | `ca41abf6a660110e732c29194833aca9e97c1319` | Test-only legacy-default/full-override/rollback and simultaneous-switch regressions; `.37` closed, actual W12 NOT RUN. |
| Heartbeat | `3d7b1ce5e16eceb9bdbf284d449060cfcc98e635` (code `1f42636f066e8c4d6d21759eb3235295087a67cc`) | Diagnostic counters/message, boundary regression and causal report; `.11` unresolved/open. |
| W11 | `c1a14433aa81bae4f3e28cf15561498deab5362f` (code `fc5333d58447110fff59f3da1741f193e098227d`) | Actual pip CMake binary PATH, process-scoped Windows LF checkout, physical setup regressions and dated evidence; `.36` in progress. |

Enrollment/correction source: `942128955a726536b334bb6a2bcb2d6af278666e`.
Initial enrollment `2c43b7eb3021457a1050a4339df9cbfabc50a9b5` and its failed
786-case invocation remain retained, not credited to the corrected source.
Evidence root: `/tmp/nostrc-w11-w12-integration-20261023`.
The W11 test and exact pinned NIP44 vector read are enrolled in `selection.contract`;
the vector retains `nip44.test_inputs` ownership plus only its consuming contract
edge. The non-executable W11 evidence JSON has an exact explicit broad rule,
not a weakened prose allowlist. Exactly two workflow fingerprint records are refreshed. Immutable historical
fixtures remain unchanged: a SHA-pinned test-only witness reverses only reviewed
setup bytes/hash fields and rejects arbitrary source/fingerprint mutations. No
engine, old source/nonmember authority, profile capability or rollout change follows.
The one bounded review of this novel enrollment seam found no blockers; it did not
re-review approved sibling code or rerun tests.

## Hosted observation and unresolved criteria

The [W11 receipt](dependency-aware-testing-ci-2026-10-23-w11.md),
[raw index](dependency-aware-testing-ci-2026-10-23-w11-evidence.json),
[audit](dependency-aware-testing-ci-2026-10-18-acceptance-audit.md) and
[ledger](dependency-aware-testing-ci-2026-10-18-run-ledger.md) retain snapshot
**2026-10-23T00:48:54Z**: **50 roots,47 failed,1 cancelled,2 pending**;
**0/20** qualifying reviewed distinct PR comparisons, **0/2** successful complete
broad integrations. These are dated counts, not live counters. The hosted policy
suite reported808 tests with44 failures,15 errors,6 skips, including actual
setgroups denial and sanitizer partition/refusal evidence, not merely timeout.
Corrected-source hosted execution is NOT RUN here. W11 **NOT MET**, W12 **NOT RUN**.
The bounded hosted-pidns probe found no justified helper repair. NEWUSER succeeds
then opening setgroups fails EACCES before mappings/mount/PID/workload launch.
Same-UID NEWUSER does not inherently reset dumpability; namespace-local capability/
LSM policy, plausibly AppArmor, remains an inference without hosted readback.
Do not label the underlying cause confirmed or bypass it. Source/status/probe and
failed attempts: `/tmp/nostrc-w11-pidns-20261023`; `causal-status.md` SHA256
`876444895773671f60a8aa9777bfeb185ca3b6e5e17c2cbc02435103ac9b14d4`.
Its default-policy nonroot ARM64 Docker probe refuses NEWUSER (EPERM), with no
workload marker: actual prelaunch refusal, not hosted EACCES reproduction or
positive custody. No helper edit/commit, policy/sysctl change or new positive
custody acceptance; `.23` remains in progress with its new official comment.

The [heartbeat report](heartbeat-causal-diagnosis-2026-10-23.md) confirms post-burst
idle-wake attribution for five current recurrences with bound job/run heads.
Original `5467c02d11f74efc1a85c833024efb654bbf98b5` attribution is only plausible;
underlying hosted wake-latency cause and safe remedy remain unknown. Post-burst gaps
still count; strict >50ms, <=2 misses, >5 heartbeats, TIMEOUT90 and sanitizer policy
are unchanged. `post_burst_max_gap_us` covers all post-burst gaps, not only misses.
No exclusion, repeat-pass causal inference or `.11` closure. `.29` remains
provider-blocked/out of scope; no reroute or claimed remedy.

## Source-bound integration gates

Final gates bind clean source `c6fc26a858d2fcdfd8d456969c1ae0a2aa0f4508`.
`tested-source-final.json` / `tested-source-post.json` independently verify4868
tracked root entries and pinned nostrdb/secp256k1/nsync bytes against committed
objects, fingerprint tools, and prove identical pre/post source/submodules/tools.
Initial invocations retain `2c43b7eb3021457a1050a4339df9cbfabc50a9b5` and their
separate4867-entry `tested-source.json`; none is relabelled as final-source proof.
Unused documentation submodule `docs/nips` is uninitialized; no fetched-source or
execution credit is claimed for it.

| Evidence under integration root | Actual result |
|---|---|
| `pre-enrollment-focused.log` |27 tests: two failures/one error from stale accounting/historical assertions; retained. |
| `enrollment-focused.log` |28 tests: one historical-projection idempotence error; retained. |
| `enrollment-focused-corrected.log` |28/28 PASS after the finite projection correction. |
| `selector/result.json`, `selector/command.log` |786 tests in590.712s: one unclassified evidence-JSON failure, four exact-vector support-input errors,44 explicit Linux-only skips. Failed invocation retained. |
| `enrollment-complete-focused.log` |59/59 PASS after adding only the missing exact support path and an explicit broad rule for evidence JSON. No engine/authority relaxation. |
| `selector-final/result.json`, `selector-final/command.log` |786 tests in580.461s:742 PASS,44 explicit Linux-only skips, zero failures/errors. All six actual Docker fixture cases execute against immutable cached ARM64 Alpine. No Linux/hosted/complete-profile acceptance is inferred from skips. |
| `native-final/result.json`, `native-final/heartbeat-proof.json` | Fresh safe Ninja/Debug default build PASS; exact heartbeat registration1/1 PASS,4.32s, all8 internal cases including post-burst boundary/negative controls. TIMEOUT90. Raw build/focused test, not attested selector/default-all/full CTest acceptance. |
| `shells-statics-final/dispositions.txt` | All seven listed shells, four statics, full actionlint including ShellCheck and range whitespace PASS on the same clean final source. |
| Initial `native/result.json`, `native/command.log`, `native/heartbeat-registration.json` | Earlier2c43 source build/heartbeat1/1 PASS,4.38s,TIMEOUT90; not substituted for the final invocation. Both builds leave Groundhog OFF and retain the unused backend warning. Not full CTest/hosted/sanitizer acceptance. |
| `shells-statics/dispositions.txt` | Six established shell suites plus `test-install-hooks.sh`, four statics, full actionlint including ShellCheck, range whitespace PASS. |
| `enrollment-review.md` | One bounded review of the initial2c43 enrollment seam, no blockers; reviewer did not rerun tests. Subsequent exact support-path/broad-ownership omissions were diagnosed and corrected by real gates; no second review or widened authority. |

Harnesses use private HOME/XDG and actual private bus/Secret Service with read-only
developer keychain guards in the original context. Final selector/native/shell
receipts all prove service/bus reaped and socket absent; pre-run guard uses
`check=True`, post-run exit0. No keychain mutation, login-keychain test storage, full old cold matrix or
full native CTest is claimed.

## Component evidence integrity

* W12 `/tmp/nostrc-w12-native-20261023T004512Z`: six original manifest entries
  verified; seven exact committed blob bindings verified. Corrected
  `source-manifest.txt` SHA256 `b4a693a9a3cfa082e73e717d8b8c6040b2ab83f6371f31649c40444a7676a201`
  is added by `SHA256SUMS.manifest`, derived evidence only, not a new gate/log.
* Heartbeat `/tmp/nostrc-heartbeat-claude-20261023`: parent verified132 entries,
  four source blobs and five job/run-head bindings. `SHA256SUMS` SHA256
  `a02a700ebcc4ca5c74abb13406d457ba57732bcdcb5f8d9ea16f02e6bc793fc2`;
  `MANIFEST.json` SHA256 `c7f896c5f3e504d09f40a275030c9886c1069f3efd20525b98b492a06b1fde4a`.
  Integrated test SHA256 `05074fa2a888228c991b0d5d165caaae37e0487902a72609e57c178bde54b3f3`.
* W11 `/tmp/nostrc-w11-20261023`: old505-entry `SHA256SUMS` has504 current
  matches and one retained stale `setup-tests.log` entry; not wholly verified.
  `SHA256SUMS.final` verifies8/8 including current PASS setup log SHA256
  `606ceac95a54d805c8e6181b47f0bd0a369a5bf0a2504e03799db1ff8c85a0d9`.
  Final supersedes that entry without erasing the old list. Parent verification
  JSONs remain in `/tmp/nostrc-w11-w12-20261023`.

## Tracker, version and landing

Finite local integration `.38` is CLOSED; `.36` remains in progress and `.5`
remains the already-closed baseline criterion, not W11 acceptance. Official
`bd export --all` at2026-10-23T01:24:51.212483Z preserves1556 records (1519 issues,
37 memories),610 comments and1165 interactions. Every initial/root HEAD/index/
worktree identity/comment and complete memory record survives; interaction streams
are copied whole with every prior stream an exact prefix. Parent PNGs/investigation
and dirty nostrdb HEAD/status/diff match initial preservation; no root reset or
semantic JSONL editing. Export SHA256
`f27c52dbf3c55a24c7643b868068be35b65d7c5492c49e31b42282cc98de3ba3`;
interaction SHA256 `77d5968d1ddcfd3045b68fffc17153cbb6e5e37b31f42b0f3057adeefce65d37`.
`tracker-final/preservation.json`, `component-evidence-index.json`, final source
continuity and evidence manifest retain exact snapshot scope/hashes. The earlier
`bd dolt pull` Error1105/no remote is retained; no direct database editing.
Internal CI/tooling, tests and docs only: **no component version bump**;
`VERSION_MANIFEST.md` unchanged. No defaults/protections/publisher/trigger/schedule
cutover. Local commits only; parent owns push, merge-back, corrected-source hosted
inspection and worktree cleanup.
