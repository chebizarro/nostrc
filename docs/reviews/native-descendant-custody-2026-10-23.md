# Native descendant custody: explicit kernel backend (.23)

Base: `0ce430299060a09e4a2db250be1a2cac4df2c212`. Internal tooling/tests only;
**no component version bump**, default cutover, hosted/release acceptance, or MDK
cleanup change. Parent owns inventory/core realization and landing. This note is
not a replacement for the criterion register or W11 ledger.

## Implemented Linux boundary

`scripts/test-gate.sh --native-custody pidns` (`--linux-custody pidns` alias)
selects `scripts/adapter_namespace_custody.py`. `legacy` remains the default;
existing ptrace is unchanged, opt-in, non-instrumented and unwrapped only.

The new helper creates a user namespace mapping only the caller's own UID/GID,
then private mount and PID namespaces. It never acquires host CAP_SYS_ADMIN.
Mount propagation is private before mounting procfs; the requested program is
not executed until setup, init identity, proc view and pidfd signaling succeed.
Policy denying unprivileged user namespaces or proc mounts is an explicit
prelaunch failure, not permission to escalate or fall back.

An outer supervisor holds its direct, unreaped namespace-init child by pidfd.
Init holds the direct root by pidfd and adopts/reaps double-fork orphans. After
normal/nonzero root exit, or cancellation grace expiry, init exits. Linux's
[`zap_pid_ns_processes`](https://github.com/torvalds/linux/blob/v6.12/kernel/pid_namespace.c#L170-L260)
first disables further PID allocation, kills remaining tasks and waits for the
namespace to empty before init is reaped. That is the cleanup barrier, not an
inference from an arbitrary direct-child exit. Nested PID namespaces, setsid,
closed inherited descriptors and CLONE_UNTRACED cannot leave this domain.
See also [PID namespace semantics](https://man7.org/linux/man-pages/man7/pid_namespaces.7.html).

Cancellation signals the reserved root with TERM so it can finalize backend
receipts/raw output. Remaining descendants are bound to pidfds using the retained
private procfs directory and killed through those handles. This is **not** host
ancestry sampling: every PID visible to this fresh namespace is owned, even if
reused before pidfd acquisition. PID 1 is excluded and the root identity remains
reserved until reaped. No cached positive-PID/PGID signaling, process-name match,
retry, sleep, or inherited-control-FD assumption is used. Parent-death signals
plus retained parent pidfds cover death before/after arming; credential changes
re-arm adapter parent-death protection. Fatal setup/collector paths exit init and
wait for the same kernel barrier. An uninterruptible kernel task may delay that
barrier; it cannot become successful cleanup by timeout.

The backend uses **no ptrace**. Real LSan positive/leak-negative tests keep leak
detection enabled and require the leak to fail. This does not change the ptrace
instrumentation refusal or establish sanitizer-profile parity. UID/GID maps,
procfs, namespaces and no_new_privs are material environment differences, so
`complete=false`, `complete_gate_eligible=false`, `untraced_profile_parity=false`
remain explicit even for successful diagnostics. No filesystem/network sandbox
or work requested through an external daemon is claimed.

## Exact macOS constraint, not a blanket impossibility claim

On macOS 27/arm64, an actual compiled call to
[`es_new_descendants_client`](https://developer.apple.com/documentation/endpointsecurity/es_new_descendants_client(_:_:))
returns `ES_NEW_CLIENT_RESULT_ERR_NOT_ENTITLED` (3). This modern API is explicitly
recursive and descendant-scoped. It needs
[`com.apple.developer.endpoint-security.client`](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.developer.endpoint-security.client),
which Apple requires developers to request. Unlike the system-wide ES client,
the descendant API does **not** require root or TCC approval. Neither root nor
Full Disk Access is the missing requirement for this candidate. No entitlement,
SIP, signing policy or developer permissions were modified.

Alternative primitives were investigated, not assumed equivalent:

* Actual `EVFILT_PROC` + `NOTE_TRACK` registration returns `EV_ERROR/ENOTSUP`.
  XNU's [`filt_procattach`](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/kern_event.c#L1030-L1040)
  explicitly rejects recursive tracking flags. NOTE_FORK notification alone is
  not a recursive ownership/exit barrier.
* Actual `coalition_create` at UID 501 returns -1/EPERM and no coalition ID.
  XNU's [coalition syscall authorization](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/sys_coalition.c#L208-L222)
  requires membership in a privileged coalition. Mere existence of the syscall
  is not authority to create a dedicated domain. No existing coalition was
  terminated or altered.
* [`proc_signal_with_audittoken`](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/proc_info.c#L3464-L3483)
  supplies identity-bound signaling, but does not itself discover/reserve an
  entire recursively forking tree. Pairing it with a ps ancestry snapshot would
  leave the ownership gap, not solve it.

Therefore the explicit `pidns` route refuses on macOS **before checkout/target
launch** with the entitlement boundary stated. There is no advertised macOS
safe-cleanup implementation. A provisioned, appropriately entitled descendant
broker would still need lifecycle, dropped-event, authorization, death, exit
barrier and raw-finalization proofs before admission. The legacy macOS path is
retained byte-for-byte, not relabelled safe. `.23` cannot be closed as full
cross-platform custody from the Linux implementation or macOS refusal tests.

## Parent integration interfaces and remaining scope

Enroll exact new runtime input `scripts/adapter_namespace_custody.py` and test
input `tests/selection/test_namespace_custody.py` in the parent's authoritative
inventory/realization contract. Existing changed custody/fixture/adapter paths
must keep their source bindings. No competing core or inventory edits are made
here. This backend is not a prepared-peer route, Docker lifecycle receipt or
selector/profile admission.

Both kernel backends still refuse display/private-bus wrappers before a lane
launch; non-CTest/mixed wrapper refusal and native keyring-service refusal remain.
The wrapper may be the root rather than the backend finalizer. Safe admission
needs the parent's explicit protected-finalizer/root lifetime interface and real
wrapped cancellation/failure/receipt proofs, not just descendant kill success.
This is **internal remaining engineering for parent I66FCFE65 in this batch**,
not an external/platform blocker or another deferred acceptance cell. Exact
interface/ownership requirements and the required real wrapped matrix are in
`/tmp/nostrc-native-custody-I66FCFE65-interface.json` and the `.23` bead comments.
The minimal seam keeps `run_api` as the direct reserved finalizer and moves live
bus/display resource setup behind it, with every service inside the namespace
before resolve/run. Retaining nested wrappers instead requires an authenticated,
gated finalizer reservation, never an arbitrary PID declaration. Capabilities
are never silently dropped. Existing legacy CTest wrapper behavior is untouched.
MDK/signer/workflow/ledger and sanitizer/dependency lane files are not modified.

## Reproduction and evidence boundaries

Use `/usr/bin/env XPC_SERVICE_NAME=0 PYTHONDONTWRITEBYTECODE=1` for every test.
Native macOS tests prove entitlement/tracking/refusal and legacy compatibility;
they do not claim positive native arbitrary-tree cleanup. Linux positives run
on the actual ARM64 Linux kernel without emulation, in a disposable nonroot
UID/GID1001 Docker test environment. Docker only provisions the test host; the
helper itself invokes kernel syscalls, not Docker or the contained adapter.
This is not authored Linux x86, hosted, whole-profile or bare-metal acceptance.
The test host must permit user/mount/PID namespaces and private proc mounts.
Default Docker masked proc paths deny the latter: retain that failure, or
explicitly provision a test container with `seccomp=unconfined` and
`systempaths=unconfined`; do not weaken the helper or silently skip positives.
No host capabilities or privileged container are needed.

Retained evidence directory: `/tmp/nostrc-native-custody-20261023` (including first
failed attempts, primary-source downloads, source hashes, actual raw bytes and
final run logs). New tests cover command/CTest success, nonzero, cancellation,
escaped TERM-ignoring descendants, double fork, nested namespaces, supervisor and
adapter-parent SIGKILL, startup EOF/pending cancellation, denied setup/signaling,
LSan pass/leak failure and unrelated sibling roundtrips. Readiness and death are
socket/pipe/pidfd events; timeouts are failure guards, never synchronization.

Safe fresh native default build used Ninja/Debug and mandatory libsecret and
secret-service backend flags, with read-only keychain guards before and after.
Groundhog is OFF in this root default; CMake reports its backend flag unused.
This is the safe native **default build**, not all-components/profile execution.
Final counts, review disposition and immutable commit IDs are recorded in the
bead handoff; earlier failed logs remain separate and are not erased.
