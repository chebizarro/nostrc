# FIPS C port security review — 2026-11-15

Task: `nostrc-f66sr.5`. Baseline: `2534b158535265b8ebb6c279766c928da43cf33a`
(`feature/fips-security-review`). **Verdict: changes required before a networked
first release.** No critical finding; one high latent protocol finding and an ACL policy gap
remain in protected host files. Local medium fixes below have regressions. This is a source review plus
local macOS arm64 sanitizer/fuzz evidence, not deployment or hosted-CI acceptance.

## Scope, method and comparison

Reviewed `libfips-core/{src,include}` codec, identity, utils, cache,
proto/{stp,bloom,routing,lookup,fmp,fsp,mmp,path,probe,rekey}, node/session and Noise
profile integration, and `fipsd` CLI/log/runtime/privilege/config/peer/control,
node lifecycle/workers/netmon/handlers/dataplane and host OS adapters. The prior
[Noise audit](libfips-core-noise-audit-2026-11-15.md) is the primitive baseline:
this review does **not** re-audit the cryptographic constructions or claim to
close its upstream issues. It checks their daemon call sites and mitigations.

Upstream comparisons use jmcorgan/fips commit
`6aa80504baa0fa312b392009eea0e3e59fb6fe4c`, as pinned by `libfips-core/UPSTREAM.md`.
The requested upstream security/design/control-socket/unprivileged-user documents
were read from a disposable checkout **inside this worktree**, since those paths
are not vendored here. Upstream paths below are relative to that revision, not
to the main nostrc checkout. Reviewed the current tree, not future sibling work.
No edits under `fipsd/src/node/`, `fipsd/src/peer/`, or
`fipsd/src/control/snapshot.h`. Follow-ups there include implementation locations
and regression expectations below. No Beads commands, pre-push wrapper, push or
merge were run. References in this report are post-fix line numbers.

## Findings

| ID | Severity | File:line | Issue and impact | Upstream comparison | Disposition |
|---|---|---|---|---|---|
| H1 | **high** | `libfips-core/src/proto/fmp/core.c:46-61`; `fipsd/src/node/handlers/handshake.c:281-335` | An authenticated but **replayed old IK msg1** with a different startup epoch can evict the current live peer before new-session liveness is proved. | Inherited: `core/src/proto/fmp/core.rs` also maps different epoch to `RestartThenPromote`; rate limiting does not establish freshness. | Protected host follow-up; high latent risk, gate before enabling network transports. |
| A1 | medium | `fipsd/src/node/handlers/handshake.c:283-335,377-397`; `fipsd/src/node/handlers/provider.c:402` | The reviewed host has **no ACL admission enforcement**; status explicitly says disabled/default allow. This is missing policy integration, not evidence that an enabled deny policy was bypassed. ACL-dependent deployments must not assume enforcement. | Upstream security/design docs require an authenticated-identity ACL check before peer allocation/msg2, and outbound checks. | Sibling ACL work must close this; release blocker for ACL-dependent deployments. |
| M1 | medium | `fipsd/src/control/control.c:139-145,277-299`; `control.h:10` | Unlimited accepted clients previously allocated two fibers each, despite per-request deadlines. A socket-authorized local user could exhaust memory/fds. | Upstream `src/control/mod.rs:199-212` likewise spawns per accept without a concurrent-client semaphore. | **Fixed:** 32 live connection slots, refused before channel/fiber allocation, returned only when both owners exit. |
| M2 | medium | `fipsd/src/control/protocol.c:103-128`; `protocol.h:6` | A 4096-byte bound alone permits deeply recursive JSON parsing on a fiber stack. | Rust parser/runtime stack behavior is not a C/Jansson fiber-stack guarantee. | **Fixed:** lexical 64-container preflight outside strings before Jansson; Jansson remains the syntax authority. |
| M3 | medium | `fipsd/src/config/value.c:111-177`; `config/loadlog.c:16-20` | YAML aliases multiply large scalar copies despite input/node caps; many warnings also accumulate allocated diagnostic strings. | Port-specific explicit ownership/expansion model; Rust serde behavior is not evidence of a bound for this libyaml conversion. | **Fixed:** 8 MiB total expanded scalar/key bytes including terminators; 4096 diagnostics, fail config on overflow. Existing depth/node/input bounds retained. |
| M4 | medium | `fipsd/src/log.c:237-255`; `main.c:85-98` | Pre-config log open could block on a FIFO or append to a nonregular/multiply-linked target, including with launch privileges. | Upstream log opening is not a substitute for C descriptor type/alias validation. CLI and writable log-directory control remain trusted. | **Fixed:** nonblocking open, descriptor regular-file and single-link checks; existing final-leaf `O_NOFOLLOW` retained. |
| M5 | medium | `fipsd/src/log.c:295-311` | Failed rotation raised the effective cap and continued appending indefinitely: configured disk bound failed open. | Inherited from `src/utils/logfile.rs:153-164` (raises limit after failed roll). | **Fixed:** refuse at the cap on rotation failure, retry later; count each successful partial append even when the subsequent write fails. No unbounded fallback. |
| M6 | medium | `libfips-core/src/proto/routing/wire.c:68-80` | PathBroken's u16 body length silently truncated a large coordinate encoding, producing malformed frames. No buffer overrun demonstrated. | Upstream encoder also narrows the body length to u16. The C maximum-coordinate test previously pinned the lossy cast. | **Fixed:** reject body > UINT16_MAX before any writer/output mutation; valid wire unchanged. |
| M7 | medium | `fipsd/src/node/handlers/handlers.c:376-396,460-480`; `libgo/src/channel.c:1129-1140` | Timed-out control command remains queued and may mutate state later. A reply sent to its now-unobserved capacity-one channel is not decref'd by generic channel destruction. | Host-specific command/reply lifetime; socket deadlines do not cancel queued state changes. | Protected host follow-up; no claim the timeout cancels work. |
| L1 | low | `fipsd/src/config/config.c:106-126` | YAML loading follows symlinks and enforces regular-file/size, not owner or mode. A readable nsec-bearing file can be exposed by deployment mistakes. | Upstream YAML read is not owner/mode enforced either; upstream key-file warnings in `src/config/mod.rs:690-733` concern its persistent identity key file, not YAML. | Operational follow-up; no incompatible config-path policy silently introduced. |
| L2 | low | `fipsd/src/privilege.c:64-108`; `control/control.c:75-89,104-125`; `log.c:204-255` | Final-leaf checks do not secure arbitrary attacker-writable ancestor directories. Socket chmod is by name after lstat; custom/shared parents are intentionally not chmod'd. | Same filesystem/group trust boundary in control-socket reference; C stale-socket removal is stricter than upstream. | Use service-owned private parents. Custom-path race hardening requires descriptor-relative path design, not a local cosmetic check. |
| L3 | low | `libfips-core/src/proto/rate_limit.c:67-68`; `proto/lookup/limits.c:92-108`; `proto/lookup/core.c:71-93` | Bounded caches/rate-key tables are **not uniformly fail-closed**: unseen routing-error keys can be admitted uncached; lookup forwarding/recent records evict and admit. Churn can defeat remembered per-key suppression. | Deliberate upstream availability/eviction semantics, not a newly unbounded allocation. Link-authenticated traffic still consumes processing. | Aggregate authenticated-peer work-budget follow-up; do not describe these as strict admission caps. |
| L4 | low | `fipsd/src/node/handlers/session.c:97-129,184-190,407-411`; `handlers.c:130-132,370-374` | Pending/held/queued user plaintext is freed or length-reset without comprehensive wiping. | Host buffer ownership, separate from Noise key destruction. | Protected host follow-up; matters for allocator remnants/core dumps, not demonstrated remote readout. |
| I1 | info | `fipsd/src/main.c:150-176` | Production entry point registers no Tier B2 transport drivers; STUN/Nostr/native socket/TUN integration is not qualified by these tests. | Upstream has those transports and their own admission limits. | No claim of complete exposure or rate-limit equivalence for absent adapters. |
| I2 | info | `fipsd/src/config/value.c:17-25,178-192`; `node/context.c:310-327` | Owned config strings and identity material are wiped, but libyaml internal buffers/error paths, process dumps and same-uid debugging are not comprehensively protected. | Core zeroization alone is not a host-memory secrecy guarantee. | Deployment/dump/ptrace caveat; details below. |

### File-ready protected-path follow-ups

**H1 — restart replay.** Source-traced attack, not a live-network exploit test:
record a legitimate peer's IK msg1 in epoch A; after it reconnects in epoch B,
replay A. IK decryption authenticates the recorded static identity but does not
prove current possession/liveness. `fips_fmp_establish_inbound` checks inequality,
not freshness. `remove_peer_carry` at handshake.c:314 tears down B before msg2 is
answered. Captured messages from two epochs can alternate this transition.
The global msg1 token bucket only bounds frequency. Epochs are opaque random
8-byte values: **do not fix this with numeric greater-than ordering**.

Owner should modify `node/handlers/handshake.c` and its peer/rekey integration to
preserve the live link until fresh key confirmation. Use bounded retired-epoch/
msg1 history only as supplementary protection. History
alone cannot reject previously unseen historical messages or entries forgotten
on eviction; fresh possession must be established before destructive replacement. Preserve legitimate restart and retransmission
semantics. Add an in-process transport regression: establish A, restart to B,
replay captured A, assert B's peer/index/key/replay state survives and no duplicate
promotion occurs; exercise replay across history saturation. Core decision-only
tests cannot prove the host lifecycle fix.

**A1 — ACL policy gap, insertion and reload ordering.** Sibling ACL code must check the
normalized authenticated identity immediately after handshake.c:283, **before**
reserve, restart removal, receive-index allocation, msg2 generation/send or
`establish`. Recheck outbound authenticated replies before the transition around
:386-397 and before promotion; also check configured/manual dials. Immutable
policy reload needs a retained ref and a defined linearization point at dispatcher
admission. Denied input must leave peer counts, indices, endpoints and packet
send state unchanged. Add denied new-peer, denied restart, denied rebind, outbound
deny, reload-during-handshake and resource-count regressions. This review does
not certify the sibling's not-yet-present ACL/metrics/stats/diag implementation.

### Disposition — A1 (`nostrc-f66sr.8`)

The ACL added by `4aeec42da` had a late inbound gate: it followed reservation,
restart eviction and the same-epoch rekey path. Moved it immediately after IK
identity normalization, before all three paths. Indexed outbound rekey replies
now also check the link's pinned identity before entering the machine; pending
outbound IK already rechecked before transition/promotion. Manual/configured
dials keep their pre-reservation gate. The retained GoSwap config load in
`hs_acl_authorize` is the admission linearization point: an immutable generation
owns that decision, and subsequent admissions load anew. Reload does not revoke
already established traffic.

`FipsdNodeAclAdmissionTest` drives production IK/AEAD through an in-process
transport and real node dispatcher/workers. It asserts denied-new-peer,
denied-restart and denied-rebind peer/index/binding/endpoint/key/replay/send-state
invariance; manual/configured outbound denial; and allow-to-deny reload between
msg1 and authenticated msg2. A denied reply reclaims only the pending outbound
leg/lease, with no promotion or send. `FipsdAclTest` retains old/new immutable
policies and covers npub/alias/ALL, allow-first/default-allow semantics.
Focused ASan/UBSan: these two tests plus `FipsdNodeAclTest` **3/3 passed**.
Full-suite sanitizer/static evidence is recorded with H1 below. This is local
host admission evidence, not deployed transport or hosted-CI qualification.

**M7 — command cancellation/reply ownership.** In `node/handlers/handlers.c`,
make the execution-deadline contract explicit. A waiting timeout alone does not
necessarily promise cancellation; if it is an execution deadline, carry a retained
cancellation/deadline token into `handler_command` and check it on the dispatcher
**before** state mutations. The abandoned-response leak is a defect independently
of that contract. Tie response ownership to
a live receiver (closing/draining or an explicit owned reply object), rather than
assuming `try_send` fails after the waiting fiber unrefs its own channel reference.
The command still owns a channel ref; the empty channel remains sendable and its
buffer destructor frees pointers, not JSON objects. Regression: block dispatcher,
queue a mutating request, expire/cancel it, resume dispatcher, assert no mutation
and balanced response destruction, including the timeout/completion race. Avoid
adding a polling cancellation loop.

**L4 — queued plaintext.** Wipe used pending/held slots on consume, drop-oldest,
expiry and teardown; wipe `HCMD_SEND` flexible payload and delivery-notice payload
on all exits. Do not wipe borrowed buffers still owned by worker/snapshot refs.
Add deterministic destructor/slot-reuse tests, not a heap scan or timing test.

## (a) DoS bounds and admission inventory

Values below are daemon defaults or fixed limits, not promises that trusted
embedding code cannot request larger bounded allocations. A resource cap is not
itself a packet-rate limit. **The requested universal fail-closed checklist is
not fully satisfied:** L3 evicts/admits; pending datagrams drop oldest; H1/A1/M7
remain. No unchecked unbounded growth was found in the reviewed fixed tables.

| Resource | Cap / full policy | Who can reach it and evidence |
|---|---|---|
| Peer table / pending IK | 256 host slots; reservation/insertion refuse at capacity. Separate pending limit defaults to 64. | Raw msg1 hits global stranger/established buckets before Noise (`handlers/handshake.c:272-281`); identity verified before insertion. Defaults stranger burst 100/refill 10/s, established derived separately (`include/fips/node/rate_limit.h:8-14`; host `handlers.c:289-295`). Source endpoint selects class, not proof of identity. ACL gap A1. |
| Session table / XK setup | 256 slots; no vacancy refuses. Setup limiter entries bounded by peer capacity and refuse a new key when full. | FSP arrives over an authenticated link. The originating end-to-end identity is not authenticated by XK msg1; per-carrier-peer setup bucket is applied **before** responder work (`handlers/session.c:193-202`; core `node/rate_limit.c:170-200`). It bounds each carrier, not all origins globally. |
| Receive-index allocator + host map | Four times peer capacity (1024 default), bounded random allocation attempts; refuse full rather than overwrite. | Dispatcher-owned; reservation after admission (`handlers.c:176,288`; `utils/index.c`; host `handlers/rekey.c:43`). Restart lifecycle H1 remains. |
| Lookup ladder, pending/backoff | 256 pending/backoff entries; pending refuses full, backoff evicts. Finite 1/2/4/8-second retry ladder; no indefinite request loop. | Local sends or authenticated transit. Recent requests 128, forwarding pairs 64 (`handlers/lookup.c:7-12`); dedup/eviction is not aggregate flood control (L3). |
| Lookup response signing | Per-carrier-peer keys <=256, full key table refuses; default burst 256, refill 32/s. | `handlers/lookup.c:83`, `handlers.c:295`, core `node/rate_limit.c:205-239`. Raw transport lookup is not dispatched before link authentication. These limits match the imported upstream limiter model; do not interpret it as end-to-end request authentication. |
| Coordinate / identity caches | Coordinate soft default 1024 with bounded probation/slack storage (`FIPS_COORD_CACHE_SLOTS + 8`); eviction/expiry instead of growth. Cached coordinate max 80 entries. Identity table default 256. | Verified lookup response paths plus bounded probation; defaults/allocation `handlers.c:194-196,253,296-300`, cache and identity-cache preflights. Cache eviction is availability policy, not fail-closed admission. |
| Bloom aggregation / STP | 256 bloom peers/leaves; 32 tree peers; action storage peer cap+1. Bloom wire filter exactly1024 bytes. Full peer/storage operations refuse. | Authenticated links; `handlers/bloom.c:7-26,44`; `handlers.c:182`; core bloom state/wire and STP decode. Large coordinate/signature-chain wire domains are still bounded and allocated off small fiber stacks. |
| Path candidates / outputs / probes | Eight candidate slots; rejected when no free candidate. Fixed/caller-bounded path output arrays and diagnostic probe state. | Authenticated path messages and trusted control actions. `proto/path/candidate.c:5-33`, `handlers.c:203`; future sibling diag must preserve its own session/result caps. |
| Worker queues / packet pool | Default2 workers, each queue 64 plus bounded results 64; constructor max 64 workers/capacity 65536. Packet pool 256; try-submit refuses full. Dispatcher notices/commands 64. | Raw packets may occupy pool slots before AEAD, but cannot grow it; worker completion requires generation/key validation. `node/worker_set.c:53-64`; `node/dataplane/snapshot.c:153`; `handlers.c:6-8,117-118`. |
| Pending / held datagrams | Default 8 per destination, hard max 16; <=256 destinations, payload <=1400 bytes. Full queue drops oldest; unknown destination with no slot drops. | Local API sends, not arbitrary unauthenticated peer allocation. `handlers/internal.h:38-41`; `handlers/session.c:97-129`; expiry :407-411. Bounded but **not refuse-new** semantics; wiping L4. |
| Control connections | **32** active slots, listen backlog 128; close excess immediately before stacks. Request 4096 bytes, depth 64, one request per connection, 5-second read/execute/write budgets. | Filesystem socket/group admission, not remote network authentication; no accept token bucket upstream or here. Provider must cooperate with deadline; a hung provider holds its slot, rather than permitting unbounded replacement (M7). |
| Log / config diagnostics | Log line 1024 bytes, bounded rotation count/configured file cap; failed rotation now drops write. Diagnostic entries 4096, each formatted in 4096 bytes. | Local config and untrusted events can trigger messages. Synchronous logger is not a per-message rate limiter; disk/CPU latency remains operational. `log.h:42`, `log.c:295-311`, `config/loadlog.c:19-20`. |
| Stats/history and runtime registry | Core history rings 3600 fast / 1440 slow samples, caller-bounded peer storage, transactional full refusal; subsystem registry fixed `FIPSD_RUNTIME_MAX_SUBSYSTEMS`. | Trusted dispatcher/runtime, not raw network writers (`include/fips/node/stats_history.h:8-11`, `src/node/stats_history.c:168`; `fipsd/src/runtime.c:124`). Host stats/metrics integration is sibling scope, not yet approved. |

STUN and Nostr: no registered daemon adapters in this tree, so no invented
acceptance/rate-limit pass. Before enabling them, port upstream inbound-offer
concurrency/per-npub admission, expiration, relay-event deduplication and signed
event validation; STUN replies must match outstanding bounded transactions.
Nostr protocol review of a future implementation must cover REQ/EVENT/EOSE,
OK/CLOSED/AUTH and reconnects; no Nostr implementation was changed here.

## (b) Memory-safety hot spots — hand sample and harness cross-check

Reader/writer primitives (`libfips-core/src/proto/codec.c`) compare requested bytes
against remaining length before copying or advancing. Their invariant is
`pos <= len/capacity`; public callers must not forge reader internals. The
following **11 codec families** were checked by hand against truncation, variable
length and copy/borrow boundaries. Harness names exist in `libfips-core/fuzz/` and
its README. This is a representative manual sample, not proof that all inputs to
every function are safe. No new remote out-of-bounds read/write was identified.

| Codec / source anchor | Checked boundary | Existing libFuzzer target |
|---|---|---|
| `proto/coord.c:127-146` | u16 count, `2+16*count` preflight and destination capacity before entries; maximum count cannot overflow size_t on supported hosts. | `fips_fuzz_coord` |
| `proto/link.c:123` | Fixed link fields before reads; payload is a bounded borrowed tail, not unchecked copying. | `fips_fuzz_link` |
| `proto/stp/wire.c:25-61` | Fixed prefix, count*32 plus signature tail available before walking/copying chain. Large scratch object is not placed on daemon fiber stack. | `fips_fuzz_stp` |
| `proto/bloom/wire.c:35-54` | Fixed header, supported size class and exact 1024-byte residual before filter copy. | `fips_fuzz_bloom` |
| `proto/lookup/wire.c:37-84` | Request/response fixed prefix and coordinate length probe before copies; response proof tail checked separately. | `fips_fuzz_lookup` |
| `proto/fmp/wire.c:98-155` | Header/type/length validation; exact handshake payload sizes before crypto; u16 body framing bound. | `fips_fuzz_fmp` |
| `proto/fsp/wire.c:44-96` | Address count*16 borrowed span and handshake length checked; strict decode rejects invalid flags/trailing body. | `fips_fuzz_fsp` |
| `proto/routing/wire.c:68-80,99` | Exact signaled body size and coordinate tail; **new encode u16 refusal** closes silent narrowing. | `fips_fuzz_routing` |
| `proto/mmp/wire.c:21,86` | Fixed echo/report minimums and presence-dependent fields before access, bounded wire-width conversions. | `fips_fuzz_mmp` |
| `proto/path/wire.c:17-30` | TLV u16 length must fit remaining block; unknown critical type rejected, bounded skip of optional type. | `fips_fuzz_path` |
| `proto/codec.c` | Primitive scalar/slice read/write failure leaves the operation unadvanced; subtraction avoids unchecked `pos+n`. | `fips_fuzz_codec` |

Some lower-level decoders deliberately leave a tail for an enclosing protocol;
that is not universal oversized-input rejection. The enclosing framing/strict
entry point must enforce its exact body. Updated the fuzz README's stale claim
that MMP/control were merely future gaps; MMP already existed, daemon parsers now
have harnesses below.

Daemon fixed-buffer review: `control/control.c` checks `sun_path` separately from
PATH_MAX before memcpy/bind; response formatting uses measured allocation;
`log.c:135` truncates to a 1024-byte line with bounded formatting; JSON reads at
most 4097 bytes to detect the 4096-byte boundary; config loader bounds file and
in-memory input to 8 MiB. Config keys <=255 bytes, expanded objects <= 100000 nodes,
depth <=64. Request parser rejects NUL in command names and duplicate keys.
Config strings are owned dynamic allocations, not unconstrained writes to fixed
arrays. Narrowing from size_t to packet u16/epoch u32 was checked at framing,
path-MTU and routing seams: M6 was the actionable silent truncation. Rate/time
arithmetic has explicit saturation/bounded conversion helpers; caller-supplied
invalid token bucket rates remain a documented core API precondition, with
configuration validation on the daemon path.

## (c) Privilege and filesystem

- Socket mode **0770**, private managed directory **0750**, `fips` group when
  available, otherwise effective group with warning. Shared/custom parents are
  left unchanged per `control-socket.md`; socket creation/group/mode failure
  refuses startup. Private directory opened `O_DIRECTORY|O_NOFOLLOW`; socket
  lstat rejects symlinks/non-sockets and stale cleanup checks inode identity.
- `umask(0027)` is set in `main.c:94`. CLI `--log-file` is intentionally opened
  **before config and before this umask**; its explicit 0640 file creation mode
  never grants other access, but ancestor creation follows the earlier umask.
  The configured logger is opened after config/umask. M4 prevents FIFO/device/
  hardlink targets; L2 documents ancestor trust. A launch-authorized user can
  still choose an ordinary writable file as the log (including the config
  itself): `--log-file` is not an untrusted filename API or privilege boundary.
- No implemented root-to-fips credential transition: `privilege_drop` refuses
  unsupported requested transitions, and main does not drop uid. If launched as
  root, parsers, logger and all workers remain root. Prefer service-manager
  User/Group setup from upstream unprivileged guide; membership in `fips` is
  administrative control authority, not a read-only observer role. Future
  native API access additionally permits use of the node identity.
- YAML owner/mode checks are absent (L1), unlike upstream's distinct persistent
  key-file warnings. Protect nsec config as 0600, or narrowly scoped 0640 with a
  trusted service group; protect every parent. No claim of Linux/FreeBSD socket
  permission enforcement was established by this macOS test run.

## (d) Control socket and authorization

`control/protocol.c:64-94` allows only enumerated read queries or validated live
commands. Reads retain an immutable snapshot until JSON rendering completes.
Mutations have **no parser-side fallback**: they require `provider->command`,
which owns authorization/admission and dispatches to the single writer. There is
no per-command OS-user ACL; the documented filesystem `fips` group is the trust
boundary. A1 is a missing remote-peer ACL, not a hidden local provider bypass.

4096-byte input, one request, 32 concurrent clients and pre-Jansson 64-depth check
are explicit. Jansson uses `JSON_REJECT_DUPLICATES`; embedded-NUL commands fail.
Depth counting respects quoted/escaped brackets and does not replace syntax
parsing. Unknown-command errors can echo the caller's own bounded name; normal
errors expose field names/paths/status, not stored nsec or traffic keys. The
capacity regression holds 32 real provider calls at a channel barrier, confirms
client 33 closes without dispatch, releases all owners and checks zero slots.
The existing real-socket suite covers oversized/multiple requests and shutdown.
M7 means an execution-timeout error does **not yet guarantee no later mutation**.

## (e) Secrets and daemon lifetime

Inspected **every `FIPSD_ERROR/WARN/INFO/DEBUG/TRACE` call under `fipsd/src`**, plus
config diagnostics and identity-loading errors. No intentional nsec/private-key/
AEAD-key/payload logging was found. Public npub/address, key *field names*, file
paths, environment worker-count values and counters are logged. Never use
arbitrary secret strings as field names, paths or log-level/worker environment
values; these are diagnostic inputs, not secret channels.

Identity is loaded from `node.identity.nsec`, not an environment nsec override.
Environment reads cover paths, log level and worker settings. On free,
`config/value.c:17-25` wipes owned scalar text; file-loader buffer is wiped
(`config.c:126`); successfully loaded libyaml document scalars are wiped before
release (`value.c:178-192`). Libyaml's scanner/input buffers and partial-error
allocations are not guaranteed wiped. Reload acquires the old GoSwap ref, builds
a new owned tree, publishes/rejects it and frees the candidate; non-logging
changes including identity are restart-required (`config/reload.c:45-72`). The
node deliberately retains its initial config ref until destruction; old nsec is
therefore retained for the running identity, not promised erased at logging
reload. Shutdown destroys identity/secp state, releases the retained config and
wipes startup epoch (`node/context.c:326-327`). Worker bare keys are destroyed on
all completion paths. L4 covers payload remnants, distinct from secret keys.

No mlock, process-wide core-dump disabling, or ptrace restriction is established
by this port. Set service core limits appropriately and restrict same-uid debug
access; sanitize collected crash reports. These are defense-in-depth deployment
requirements, not proof that a compromised same-uid process cannot read memory.

## (f) Concurrency and libgo audit integration

- Dispatcher remains the sole writer for peer/session transitions, receive replay
  marking and send-counter reservation. Workers operate on key-only clones and
  explicit counters (`node/dataplane/keys.c:16-26`, `node/worker_set.c`). Reserving
  a counter before a failed queue submission burns it rather than reusing it.
- Receive completion checks link generation/tag **and key** before authenticated
  processing (`node/dataplane/dispatch.c:1-22`); replay check/mark is serialized
  only after successful authentication. Rebind send-state changes are sibling
  owned: preserve this reservation/key/generation contract across address changes.
- GoSwap loads retain **under its mutex**, store releases the previous owner after
  swapping (`libgo/src/swap.c:35-44`). Config/reload and control query readers keep
  refs through use. Dataplane packets retain immutable routing/epoch snapshots;
  snapshot destruction releases owned coordinates/key clones only after packet
  refs go away (`node/dataplane/snapshot.c:17-33`, `encrypted.c:17-24`). No bare
  reload pointer use-after-free was found in these reviewed call paths.
- The prior bare-key Noise fix is respected: clone does not carry live send/replay
  authority and explicit-counter APIs are used. The libgo audit repairs (TSan
  fiber-context/TLS annotation discipline and indefinite idle-I/O parking with
  explicit wake) are preserved. Control stop explicitly wakes the accept waiter
  after descriptor replacement (`control.c:336-353`), rather than relying on the
  old polling timeout. No libgo sources were modified.
- No `no_sanitize("thread")` site in fipsd/libfips-core. Existing libgo exclusions
  are the noinline current-thread errno accessors (`fiber/src/io.c:63-64`) and the
  portable context-entry wrapper after TSan fiber-context selection
  (`fiber/src/context_portable.c:54-60`), not blanket daemon suppression.
  **Important:** CMake excludes the sans-I/O core from TSan instrumentation;
  passing its tests in the TSan build is not race evidence for the core. Daemon
  host/workers/libgo are the threaded instrumentation target. M7 remains a logical
  ownership defect even though the current TSan tests pass.

## (g) Four upstream Noise notes and protocol verdict

| Prior note | Recheck of daemon mitigation | Verdict |
|---|---|---|
| N2 — AEAD encrypted message bound | Core's65535-byte limit remains. Packet/scratch/framing bounds reject larger host packets before AEAD; new daemon parser fuzzers do not substitute for crypto fuzzing. | Existing core correction preserved, host additionally bounded. |
| N4 — replay mark defensive recheck | Core mark still rechecks; workers do not mark replay. Dispatcher authenticated completion performs serialized authority update. | Mitigated in reviewed host path; preserve on sibling rebind integration. |
| N11 — unauthenticated msg1 work | IK global bucket before decode/ECDH; bounded pending guard released on every exit. XK setup bucket keyed by authenticated carrier before session setup. Lookup response signing independently limited. | Meaningful upstream-equivalent limiter integration, not immunity to distributed or authenticated-peer CPU DoS. |
| N14 — no transcript exporter/channel binding | No host exporter/channel-binding use found; no daemon layer repairs the empty-handshake-AAD/profile property. | Still upstream/design follow-up; do not claim channel binding. |

Peer restart epoch comparison is equality/inequality, **not monotonic freshness**
(H1). Worker generation/key rejection protects stale completions, but cannot
repair a replayed handshake which deliberately installs a different generation.
ACL order is A1. Routing/path/rekey transitions preserve bounded output arrays
and typed failure returns; no additional crypto primitive changes made.

## (h) Fuzzing, build hardening and verification

Added `fipsd/fuzz/` targets **`fipsd_fuzz_control`**, **`fipsd_fuzz_config`**.
`FIPSD_ENABLE_FUZZERS` defaults to `FIPS_ENABLE_FUZZERS`; production `fipsd_lib`
parsers get `fuzzer-no-link` coverage. Clang required, TSan combination refused. Both instrumented libraries propagate
`-fsanitize=fuzzer-no-link` to ordinary consumers for coverage-runtime linkage
without replacing their main. A no-ASan/no-UBSan fuzz-enabled daemon/test build
first reproduced missing coverage symbols; that build now serves as the link
regression (the existing core fuzz setup needed the same small correction).
Control harness runs real parse/dispatch without a mutation provider and checks
encode/decode equality; YAML harness runs the real in-memory loader/normalizer
and destructors, not a toy parser or repeated filesystem loads. Seeds copied
from config corpus/fixtures and actual requests inside the fipsctl path goldens,
plus malformed JSON. Added both names on one line in each of the **four** CI
build/run lists; run loops choose the correct component corpus/binary path.

Existing `.github/workflows/fips-ci.yml` sanitizer lanes build `fipsd_tests`;
TSan deliberately excludes `^FipsCore`. Top-level `CMakeLists.txt:241-260` already
supplies stack-protector-strong, non-sanitized FORTIFY (respecting distro overrides),
PIC and ELF relro/now/noexecstack. Kept those conventions instead of duplicating
FORTIFY in targets. Added CMake `CheckPIESupported` for the daemon executable so
PIE linking is checked, not merely PIC compilation. Optimized local Release
build with `-Werror` passes; compile commands show `_FORTIFY_SOURCE=2`,
`-fstack-protector-strong` and `-fPIE`; `otool -hv` reports `PIE`. ELF runtime
hardening is source-reviewed, not locally executed on macOS.

### Commands and results

All commands ran with cwd and paths under
`/Users/bizarro/Documents/Projects/nostrc-wt/fips-secreview` (abbreviated `$W` below,
expanded to that absolute path for execution). Initialized only
`third_party/nostrdb` with `git -C "$W" submodule update --init --depth 1
third_party/nostrdb` before top-level configure. No hook wrapper used.

Common CMake options (Ninja, Apple clang 21.0.0):

```sh
cmake -S "$W" -B "$W/.security-review/asan" -G Ninja \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Debug -DGO_WARNINGS_AS_ERRORS=ON -DBUILD_FIPS=ON \
  -DBUILD_APPS=OFF -DBUILD_RELAYD=OFF -DBUILD_LIBHANAMI=OFF \
  -DBUILD_LIBMARMOT=OFF -DBUILD_MARMOT_GOBJECT=OFF \
  -DBUILD_NOSTR_GOBJECT=OFF -DBUILD_NOSTR_GTK=OFF -DSIGNET_ENABLE=OFF \
  -DWITH_NOSTRDB=OFF -DLIBNOSTR_WITH_NOSTRDB=OFF \
  -DGO_ENABLE_ASAN=ON -DGO_ENABLE_UBSAN=ON
cmake --build "$W/.security-review/asan" --target \
  fipsd_tests fips_core_skeleton_test fips_core_host_test -j 6
TMPDIR="$W/.t" ASAN_OPTIONS=detect_leaks=0:strict_string_checks=1:halt_on_error=1 \
  UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
  ctest --test-dir "$W/.security-review/asan" -R '^Fips' --output-on-failure
```

TSan used a separate `tsan` build with `-DGO_ENABLE_TSAN=ON`, no ASan/UBSan, same
build targets and `TMPDIR="$W/.t" TSAN_OPTIONS=halt_on_error=1 ctest --test-dir
"$W/.security-review/tsan" -R '^Fips' --output-on-failure`. Short private TMPDIR
keeps test UNIX-socket paths below macOS sun_path capacity; it does not relax a
product bound. Release used the common feature options with `Release`, no
sanitizers, building target `fipsd`.

| Gate | Result / limit |
|---|---|
| Warnings as errors | Debug ASan/UBSan, Debug TSan, fuzz builds and optimized Release succeeded. Initial new-test helper return-type and local-name collision errors were fixed, not suppressed. |
| ASan/UBSan `^Fips` | **120 passed, 0 failed, 1 skipped** of 121. LSan unavailable in this Apple runtime (`detect_leaks=0`); no leak-clean claim. |
| TSan `^Fips` | **120 passed, 0 failed, 1 skipped** of 121. Threaded host instrumented; core instrumentation exclusion noted above. |
| Fuzz-enabled ordinary linking | `FIPS_ENABLE_FUZZERS=ON`, no ASan/UBSan: built `fipsd`, `fipsd_tests`, and both parser fuzzers after coverage-link correction. |
| Optional interop | `FipsdControlInteropTest` skipped because Rust fipsctl was not built; this is **not** an interop pass. Existing golden/differential tests ran. |
| `fipsd_fuzz_control` | ASan/UBSan **3,498,722 executions in 31s**, `-max_total_time=30 -timeout=5`; exit0, no crash/sanitizer artifact. |
| `fipsd_fuzz_config` | ASan/UBSan **197,769 executions in 31s**, same flags; exit0, no crash/sanitizer artifact. |
| Direct static checkers | `check-unsequenced-args.py --root "$W"`; `check-gui-test-gdk-frame.py --root "$W"` (55 sources); `check-linux-ci-packages.py --root "$W"`; `gnome/groundhog/tests/check_privacy.py "$W/gnome/groundhog"` (25 rules): passed. |
| Workflow / whitespace | `actionlint "$W/.github/workflows/fips-ci.yml"`, `git -C "$W" diff --check`: passed. No hosted run triggered. |

Fuzzing used **real mutation fuzzing**, not corpus replay. Apple clang lacked its
libFuzzer archive; built a disposable LLVM `llvmorg-21.1.0` compiler-rt/lib/fuzzer
runtime (commit `3623fe661ae35c6c80ac221f14d85be76aa870f1`) inside the worktree and
selected it using a private `-resource-dir` overlay. No system compiler files
were changed. Commands used writable **copies** of checked-in seed directories,
artifact directories under `.security-review`, and the same ASan/UBSan options.
30 seconds is a smoke budget, not exhaustive parser or allocator verification.

Regression anchors: control-capacity barrier (`tests/test_control.c:461` and
`FipsdControlCapacityTest`); JSON depth/string escapes (:497); YAML large-anchor
alias and diagnostic saturation (`tests/test_config.c:315`); FIFO/hardlink refusal
and blocked-rotation/recovery plus deterministic partial-write accounting
(`tests/test_log.c`, test-only write interposition commits two real bytes then
returns ENOSPC); PathBroken maximum
representable/first-overflow/maximum-coordinate cases
(`libfips-core/tests/test_routing.c:383`). No sleep-based fixture was introduced. A one-shot inline Oracle review caught
the partial-write accounting hole and conditional coverage-link issue; both were
fixed and verified. An initial RLIMIT_FSIZE regression did not induce an error
on this macOS host; replaced that unreliable fixture with test-only syscall
interposition, not a skipped test or relaxed assertion. RepoPrompt artifact generation refused passive tracked-
submodule status, so no different checkout was substituted for that review.

## Per-area verdict and release handoff

| Area | Verdict |
|---|---|
| Codec, utils, identity, cache | Sampled memory boundaries sound after M6; earlier Noise findings remain the crypto baseline. Caller invariants and eviction policies are explicit. |
| STP/bloom/routing/lookup | Bounded storage; no universal flood-control pass (L3). Authenticated input can still consume CPU and churn caches. |
| FMP/FSP/node/session/rekey/profiles | **Not release-approved:** H1 restart replay; prior N14 remains. Existing crypto/replay/limiter defenses preserved. |
| MMP/path/probe | Bounded sampled wires/state; sibling rebind/send-state work requires its own integration checks. |
| CLI/log/privilege/config/host_os | Local medium defects fixed; launch identity/path ownership/dump policy remain deployment responsibilities. No full root sandbox/privilege drop. |
| Control | Parser/concurrency bounds fixed; **M7 cancellation/response ownership outstanding** in protected host dispatcher. |
| Lifecycle/worker/dataplane/concurrency | Local TSan/tests pass for reviewed tree; no snapshot UAF found. Does not certify future ACL/rebind changes or prove absence of races. |
| Build/fuzz/CI | New parser harnesses and four CI lists added; local gates pass with explicit interop/LSan/platform limits. Hosted Linux/FreeBSD/transport evidence absent. |

**Version decision:** PATCH-class corrections fold into **libfips-core 0.1.0
Unreleased and fipsd 0.1.0 Unreleased**, with no bump/tag/valid-wire change; recorded
in `VERSION_MANIFEST.md`. Remaining file-ready findings above are the handoff,
not Beads updates. Before release, close H1/A1 and M7, integrate/retest sibling
ACL/rebind/metrics work, run the hosted platform lanes and real transport/interop
gates. Scratch builds, private fuzz runtime/corpus clones and temporary sockets
are disposable; submodules must be deinitialized at session completion.
