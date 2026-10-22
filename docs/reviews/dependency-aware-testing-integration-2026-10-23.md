# Remaining acceptance: bounded integration, 2026-10-23

## Source and disposition

Base `0ce430299060a09e4a2db250be1a2cac4df2c212`; preserving merges of approved
A `9bb00d07a08e2f80202b66b63123c09a382159c8` and C
`cceaddb38b5b97e7cda80819123d58328415e703`, including TCP
`dad32d7976e8921093f61099acfb41bacd727b02`. Tested integration code is
`df8fb86ff329fffc72064a4ea255322ef3e955a0`. Later documentation/tracker export
is not additional executed-source credit. Internal tooling/tests/docs only:
**no independently released component version bump**. Parent owns landing/push.

This finishes the internal wrapped-finalizer seam, not all acceptance criteria.
`.35` is officially closed at its finite C commit. Root/`.8`/`.11`/`.23`/`.29`
remain open. W11 **NOT MET (0/20,0/2)**; W12 **NOT RUN**. Default custody stays
`legacy`; no enforcement, protection, publisher, trigger or schedule cutover.

## Whole outer-wrapper custody

Explicit `scripts/test-gate.sh --native-custody pidns` (alias
`--linux-custody pidns`) keeps the backend finalizer as the namespace's direct
reserved root. Actual display/private-bus setup runs inside that root, spanning
resolve/run; no `xvfb-run`/`dbus-run-session` wrapper sits above its finalizer.
Command, CTest, build-contract and mixed lanes share the kernel-owned domain.
Legacy non-CTest wrapper refusals and ptrace wrapper/instrumentation refusals
remain; capabilities are never dropped to obtain a receipt.

* The helper prepares a caller-owned private tmpfs HOME/XDG tree before target
  execution, and binds its socket directory over an existing canonical
  `/tmp/.X11-unix`. Missing/unsafe mountpoints refuse; host directories are not
  created or numerically cleaned up. Namespace destruction removes private bytes.
* Direct bus/display services have reserved pidfds, actual executable hashes,
  bounded readiness and wait/reap proof. D-Bus answers a real `ListNames` call;
  Xvfb authenticates a real X11 handshake and rejects a wrong cookie. Its
  caller-only nested UID/GID mapping lets `-nolock` work without host root or
  shared X lock creation. The protected finalizer stays nondumpable; the service
  child closes its protected descriptor before its own mapping setup.
* The trusted root compares filesystem receipts with original in-memory CLI
  results, seals retained bytes using an in-memory digest, and sends that digest
  through a private pipe. Supervisors close unused copies; workloads cannot
  reopen the finalizer's descriptors through procfs. The outer adapter verifies
  the seal only after the helper/kernel namespace-exit barrier. A separate
  `native-receipt.json` carries the verified proof; the original child receipt
  is not rewritten. Raw child receipts alone cannot establish aggregation.
* Exact claimed source bytes are retained and sealed, so partial verification
  works after checkout cleanup; tampering refuses. Real escaped late-writer,
  cancellation, nonzero/raw-byte, mixed-lane, adapter-death, sibling and LSan
  pass/leak-negative cases preserve failures rather than promoting child success.

Resource setup is not executable capability, Secret Service provisioning, or
profile parity. `gnome_keyring` commands still refuse; existing keyring CTests
keep their own HOME/bus/service-initially-absent fixtures. `signer_endpoint` is
not admitted. Namespace UID/proc/environment differences stay diagnostic:
`complete=false`, `complete_gate_eligible=false`, `untraced_profile_parity=false`.
No filesystem/network sandbox or custody of external-daemon work is claimed.

## C enrollment and cache-export repair

Exact runtime diagnostic sources, approver C, Secret Service/schema, fetch recipe,
original Docker/Cargo/toolchain/driver inputs and standalone negative runners are
enrolled. Six exact source-owner rules preserve existing owners and add only the
contract consumer; unrelated Grotto GUI changes do not acquire that consumer.
The old policy projection reverses only these exact additions. The 193 review
keys, exact33 omissions/29 required closures, generation/default-build contracts
and full audits are preserved, not renewed or replaced with selected-only audits.

Enrollment does **not** run standalone runtime suites during ordinary unittest
discovery, admit a signer capability/profile, alias prepared011, or authenticate
raw runtime receipts as selector execution. Actual TCP still needs the approved
TCP/libsecret/test-trust build, isolated identity/token and explicit unidentified-
caller approval. Four automatic source/admission checks include actual CLI
missing-approval, invalid producer and changed-recipe refusals before resources.

The single integration review found four concrete blockers: mutable-file-derived
seal digest, procfs access to the seal pipe, deleted source inputs preventing
retained verification, and an unowned cache-export create/cancel window. All were
fixed with focused regressions; **no second review** or broader framework was added.
The new cache helper journals a unique name/ownership token before `docker create`,
defers cancellation through acquisition/cleanup, reconciles indeterminate replies
by exact image/name/label/CID, and retains unverified cleanup negatively. No global
prune, default builder, unreserved host PID signal or name-only deletion is used.

C's original08/09 builds/hello/scoped cleanup and original011 source-only-cache
rebuild/five original tests/hello/scoped cleanup remain PASS **at C's exact source**,
complete=false. Original011 unchanged cold-recipe completion is not claimed.
The changed helper's fresh proof is three real export/cleanup cases (including
injected reply loss after actual daemon creation and actual SIGTERM) plus five
admission negatives, not a rerun of the original MDK matrix. Its SHA256 is
`baa41cb745311acf07877113ae7ca3fcd0409e047c20dc6646b76f82a69a0b39`.

## Executed evidence and limits

Evidence root: `/tmp/nostrc-remaining-integration-20261023`.
`final-code-source.json` SHA256
`f877341f6746631fb68da5efb3436bd43be1e5e125d1ec5773cab47ae4028e42`
binds the unchanged code/fixture bytes across the final split invocations.
`verification-summary.json` records the exact dispositions.

| Invocation | Actual result |
|---|---|
| `final-selector-corrected/command.log` | Reported777 cases:733 executed cases pass,44 explicit Linux-only skips; one class-setup error prevented six Docker cases because the immutable BusyBox fixture lacked its required musl loader. **Not a single full-green invocation.** |
| `final-six-docker/command.log` | The six prevented real Docker lifecycle cases PASS, zero skips, separately on cached ARM64 Alpine `sha256:71e9b7bae160ba167fa17414401b94cbcdaf3c32feeab9298bd794a2a0614c35`. Same code/source bindings; no success credited to the setup error. |
| `final-linux-corrected.log` |79/79 PASS, zero skips:40 whole-wrapper cases,10 namespace cases,20 existing adapter custody cases,9 service/contracts. Provisioned ARM64/nonroot Docker test host, not authored x86, hosted or bare metal; contract cases are not all kernel positives. |
| `enrollment-final-focused.log` |27/27 source/enrollment/input/golden checks PASS, zero skips. Nested pinned secp source ownership check separately PASS. |
| `cache-export-review-regressions/summary.json`; `final-cache-admission.log` |3/3 actual cache-export ownership cases and5/5 admission negatives PASS. |
| `final-native/result.json` | Fresh safe Ninja/Debug default build PASS; actual private HOME/bus/Secret Service cleanup and read-only developer keychain guards PASS. Root default has Groundhog OFF; unused backend-option warning retained. Not a fresh434-case native CTest claim. |
| `final-*.sh.log`; `final-statics.log` | Six shell suites/four statics PASS. Affected `test-test-gate.sh` rechecked after the final wrapper fix, PASS. No repeated production cold/full-audit matrices. |

The supported Linux host is immutable image
`sha256:e965b342b0aa8c9c2fb9b1cd5b96230e711f630ac5bae90923df4619fa30ca5c`,
UID/GID1001, network disabled, explicitly provisioned user/mount/PID policy
(`seccomp=unconfined`, `systempaths=unconfined`, private X11 mountpoint).
Docker provisions the test host; the native helper itself owns kernel namespaces.
The macOS selector harness retains a private HOME/XDG/bus/Secret Service. Nested
keychain guards execute only actual read-only `security` queries in the original
developer context, never mutations or mock-success responses. Legacy `ps`
fixtures run without the optional network sandbox that denied their process query.
The real Buildx executable is fingerprinted and linked into private client config.
Exact commands/tool identities/cleanup are in the retained per-invocation receipts.

Earlier failures remain: the initial777-case sandbox/private-HOME invocation;
79-case nondumpable-Xvfb mapping regression plus obsolete refusal assertions;
pre-enrollment/missing submodule/impact-edge errors; developmental source-copy
and test-target mistakes; original C cancellations and B's provider failure.
They are not retries relabelled as green or erased from the evidence directory.
The old725-selector/434-native-five-skips/three full-production-zero-gap baseline
keeps its own source; it was neither rerun nor promoted to this source.

## Still unfulfilled

* Native macOS positive custody: A's five Mac cases are compatibility/refusal,
  not cleanup success. The actual entitlement probe and Apple primary sources
  are in [the native custody report](native-descendant-custody-2026-10-23.md).
  `.23` stays open; Linux wrapper integration is no longer an external deferral.
* C's six actual ARM64 TCP cases and hosted native-x86 NIP55L two-case proof at
  base0ce430299 satisfy finite `.35`, not new shared capability/profile acceptance.
  C's1294-file raw manifest SHA256 is
  `68e827af62a3efdcb2dc0e8e11ee7977a73d72d385eecd2018ee8386f512b0ea`;
  source manifest `466a5b543f856f16fb77364f8cd1867d4a6637860d6514f7ab79b7b7ba85d97a`.
  Parent independently verified them; retained verification is
  `/tmp/nostrc-remaining-acceptance-20261023/parent-c-verification.json`.
* `.11` heartbeat causality and `.29` JSON-GLib remedy remain technically unresolved.
  B's provider security-filter termination while retrieving history is separate:
  `/tmp/nostrc-remaining-acceptance-20261023/provider-blocked-sanitizer.xml`, SHA256
  `4780d5c74e3c169775968d18eb68b9551c2aa4461eaa34edcc603b6bd5986f36`.
  Original extraction only; no task retry/reroute or claimed dependency fix.
* Broader `.8`, remaining authored x86/hosted/whole-job cells and operator release
  acceptance remain explicit in the [criterion register](dependency-aware-testing-ci-2026-10-22-closeout.md).
  The22:15:24.573824Z source-linked bounded snapshot is48 roots:45 failures,
  one cancellation, one queued, one in progress;0/20 qualifying comparisons and
  0/2 complete broad integrations. Historical46 identities were matched, not
  freshly refetched. It is a dated observation, not a live counter.

## Tracker and landing handoff

Official all-record export at2026-10-22T23:24:58.217225Z preserves1,553 records
(1,516 issues +37 memories),599 comments and1,161 interactions, including closed
`.35` and the parent provider/root comments. Compared root HEAD/index/worktree
and initial exports lose no identities/comments or memory bytes. Official export
ordering can differ; records are semantically identical. Events were copied
whole, byte-for-byte, with the prior stream an exact prefix. Root staged issues
and modified events remain parent's landing work, not reset or semantically edited.
Details/hashes: `tracker-final/preservation.json` under the evidence root. Final
handoff includes a full derived criterion register and scoped evidence manifest.
`bd dolt pull` failed `Error1105: no remote`; retained without retry. Only local
commits are produced here; parent owns push, merges and cleanup.
