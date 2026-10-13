# G02 shared authorisation: security-boundary review

## Context / Scope

**Verdict: REQUEST CHANGES.** The extraction introduces an unsynchronised backend/cache access boundary. There is also a queued-work permission-recheck gap. The claimed-name isolation guarantee has an inherited, explicit no-PID-platform exception; this must not be presented as universally verified caller isolation.

Reviewed `git diff master...origin/grotto/g02-shared-authorisation-service`:

- `master`: `2df9431ff22699d4a2e503e25ebf401549cf26af`.
- Branch: `9dcfd6e19676e7478743edfee22bba1f2b6e5dba`.
- Branch worktree: `/Users/bizarro/Documents/Projects/.repoprompt-worktrees/nostrc/rp-agent-7a69d51f-grotto-g02-shared-authorisation-c78840f9`; HEAD matches the remote-tracking revision and was clean.
- Design requirements: only §5.2 and §5.3 of `docs/plans/grotto-signer-ux-2026-10-12.md` (139–179), plus §9 (519–554) solely for the requested verification-gap comparison. No other design sections were read.
- Scope: the six requested boundaries. Unchanged core/caller code was followed only where necessary to verify the changed execution path. No implementation, tracker, commit, push or branch changes. This report is saved in the primary checkout, not the reviewed branch.

**Evidence labels:** “confirmed” means established from the pinned source or a stated check; it does not mean an exploit was reproduced. “Plausible” identifies an untested runtime consequence or conditional integration hazard.

## Findings (severity order)

### F1 — High: the new worker does not serialise against key-management cache mutations

**Confirmed new concurrency defect; plausible runtime use-after-free/crash, not reproduced.**

Locations:
- `nips/nip55l/src/glib/signer_approval_service.c:875–905,919–931`: backend work runs on a pool thread; `backend_lock` protects only these workers.
- `nips/nip55l/src/glib/signer_service_g.c:400–407,482–490,538–565`: `StoreKey`, `ClearKey` and `CreateProfile` still call the backend directly on the owner thread, outside that lock.
- `nips/nip55l/src/core/signer_ops.c:109–127,480–484,1284,1356–1363`: shared cache pointers are read/copied without a mutex and wiped/freed/replaced by management operations.

A legitimate UI key-clear/store can overlap a gated request's identity resolution or execution. For example, the worker can reach `strdup(g_cached_active_sk_hex)` while owner-thread `ClearKey` wipes/frees that allocation. The new one-worker pool and static lock serialise gate jobs with other gate jobs, **not** with the pre-existing management handlers. That violates §5.2's requirement to serialise a backend that is not thread-safe, and undermines reliable identity evaluation and terminal completion when memory access races.

This is not a confirmed reply-callback UAF, and it is not proof of signing as a different identity: the core separately verifies a selected npub against the resolved secret (`signer_ops.c:677–689`). That check does not make the cache pointers race-safe. Before this extraction, the ordinary gated `perform` path ran in the handler (`master`'s `signer_service_g.c:755–760`); this change adds another concurrent reader.

**Recommendation:** put gate execution and existing backend mutations under one actual execution/ownership boundary, without introducing a blocking owner-thread wait that prevents cancellation. Add deterministic concurrent clear/store versus resolution/signing tests and run them under TSAN/ASAN.

### F2 — High, conditional platform exposure: caller-supplied names still select authority on an unattested bus

**Confirmed inherited behaviour, not a new branch regression. Exposure is conditional on the non-Linux bus reporting the same UID but no PID.**

Locations:
- `nips/nip55l/src/glib/signer_caller.c:562–573`: creates `unattested` callers on that platform path.
- `nips/nip55l/src/glib/signer_service_g.c:139–150`: both direct and wrapped calls turn such a caller's claimed argument into a different caller object.
- `nips/nip55l/src/glib/signer_caller.c:393–402`: that principal is `claimed:<app_id>`.
- `nips/nip55l/src/glib/signer_approval_service.c:385–387,858–859`: the resulting principal selects grants and can acquire remembered grants.
- `nips/nip55l/src/glib/signer_caller.c:364–368`: any same-UID unattested caller also passes the approver check.

On this path, a second caller can claim the same application name and consume its remembered permission; it can also answer approvals without being distinguished from the UI. `CallWithContext("SignEvent", (..., claimed_app_id), ...)` preserves this exact behaviour. The **context dictionary** does not introduce this authority—the existing method argument does.

`master` already takes the same claimed-principal path (`signer_service_g.c:722–730`). Thus this is a conflict between the unconditional “immutable verified principal / claimed name never credential” language in §5.2 and deliberately retained platform compatibility, not evidence that G02 newly enables spoofing on an attested Linux bus.

**Recommendation:** explicitly resolve/document the no-PID exception before claiming universal caller isolation. If the §5.2 boundary is required on that platform, names cannot select reusable permission authority and an unattested caller cannot authenticate as the approval UI. Do not silently change the legacy contract as part of this review.

### F3 — Medium: an explicit denial can arrive after queue admission but before key use

**Confirmed missing execution-time permission check; plausible queued-denial interleaving, not reproduced. Result suppression is present.**

Locations:
- `nips/nip55l/src/glib/signer_approval_service.c:761–773`: checks effective denial on the owner thread, then queues execution.
- Same file `876–905`: the worker checks cancellation and identity, but not effective permissions before `perform`.
- Same file `1009–1014`: a remembered denial writes the operation/principal/identity deny without cancelling other already-executing requests for that scope.
- Same file `850–851`: the next denial check is after backend work has returned.

Two different-argument requests from the same principal/identity intentionally have different prompts. With the single worker occupied, approve A (queueing its execution), then deny B with remember. A's cancellation flag remains clear; when it reaches the worker, it can touch keys and perform cryptography despite the now-effective deny. `job_done` will deny/suppress its result if that policy is still effective, so this is **not** a demonstrated signature/plaintext disclosure. It nevertheless misses §5.2's permission recheck before key use, and the asynchronous queue makes the admission/execution gap material.

**Recommendation:** invalidate affected queued work when a deny becomes effective and make execution admission reflect the current permission revision. Test a deny introduced while another request is deterministically queued, not only before approval.

## Boundary conclusions

### 1. Caller isolation / daemon self-attribution

- **Attested path: no new claimed-name bypass found.** Browser origins are honoured only after `signer_caller_may_assert_origin` checks the bridge's executable path and device/inode (`signer_service_g.c:140–146`; `signer_caller.c:334–360`). Other claimed names remain display metadata. The signing backend ignores `app_id` (`src/core/signer_ops.c:803–807`). F2 is the explicit platform exception.
- `reason`, `purpose`, `flow_id` cannot select principal, identity or permissions: the wrapper accepts only those keys and forwards the same operation arguments to `gated_call_full` (`signer_service_g.c:190–225`). Submission copies the context separately; identity is pinned to the resolved npub (`signer_approval_service.c:845–846,951–962`).
- `signer_caller_for_nip46` validates/canonicalises hex and creates the separate `nip46:` namespace, never an approver (`signer_caller.c:74–84,364–366`). **It does not authenticate a signature.** Its documented precondition is a verified signature plus decryption/client binding (`signer_caller.h:77–81`). A pinned-source call-site search found no production NIP-46 adapter using this constructor yet—only its test. An adapter passing an unverified principal string would be a **plausible future integration hazard**, not a confirmed branch exploit.
- No request execution path found that calls the daemon's own `org.nostr.Signer` methods and therefore re-identifies a remote request as the daemon. Execution goes to `perform`/core backend (`signer_approval_service.c:904–905`). The bus calls in this adapter target `org.freedesktop.DBus` for credentials, ownership and activation, not signing. Remote authentication/self-attribution remains unproven end-to-end until a real adapter is exercised.

### 2. Exactly-once completion / cancellation / reply ownership

No confirmed double-complete or transport-continuation leak found in the reviewed valid-contract paths:

- Terminal `finish` is guarded, holds a request reference, removes the table entry and drains continuations (`signer_approval_service.c:243–260`). Each continuation detaches its handle **before** invoking its transport callback (`120–124,237–241`).
- Cancel removes just that continuation; connection close snapshots referenced handles and cancels only matching connections (`262–270,1083–1095`). Joined work remains while another continuation exists. Worker results hold independent handle references and skip detached handles (`746–754,854–867,926–929`).
- Pause increments generation, cancels requests, and suppresses late worker results across pause/resume (`832–836,1112–1120`). The after-backend barrier test exercises suppression after crypto but before delivery (`tests/test_signer_approval_service.c:147–180`).
- `BusReply` owns an invocation reference until one reply (`signer_service_g.c:118–150`); the NIP-5F job owns separate transport/reply references (`signer_nip5f.c:50–82,189–209`). Owner-thread preconditions remain mandatory for handle cancellation.

F1 is a separate backend lifetime defect, not evidence these reply detach/reference mechanisms themselves are broken. Reentrant joined-delivery/cancel/revoke and teardown-under-blocked-backend combinations still lack proof (below).

### 3. Join key / remembered scope

**No different-operation/argument prompt collision found, apart from the theoretical SHA-256 collision assumption.** The key contains principal (or connection for unidentified callers), pinned identity, operation kind **and numeric enum**, and length-delimited arguments with NULL distinct from empty (`signer_approval_service.c:698–714`). Event JSON is parsed/serialised before joining; peer hex is lowercased (`955–956,973–987`). Thus raw/B64 operations remain separate despite sharing legacy grant sections. Only announced PENDING requests join (`788–805`); each surviving continuation gets its own backend execution (`926–929,888–905`). Context/transport/claimed labels are intentionally not authority-bearing join components.

A successful remembered approval writes once, outside the per-result delivery loop (`858–867`). No joined-call-specific principal/identity widening found. **Remember is still the legacy operation-kind/principal/npub grant, not an argument-digest or event-kind grant** (`410–423`). Consequently an event permission covers other event arguments and the raw/B64 variants share their existing permission section (`30–44`). This is existing permission breadth, not an accidental broadening caused by joining; it must not be represented to users as payload-only consent. No new permission-policy contract is inferred here.

### 4. Headless first-party defaults

**Path/device/inode checks are actually enforced before an implicit allow**, not merely documented (`signer_approval_service.c:358–390`): same UID, executable present, nonzero inode, EXE or SYSTEMD_SCOPE kind, exact configured path, and matching device/inode. WEB_ORIGIN and NIP46 callers cannot get these defaults. Effective exact or wildcard deny beats both a matching allow and defaults (`329–345,385–390`). Caller executable attributes come from peer/process resolution, not the display label (`signer_caller.c:422–440`; `signer_nip5f.c:142–150`). These checks use the authenticated caller snapshot; this review does not claim perpetual re-attestation across an exec on a retained connection.

Stock list is unchanged (`nips/nip55l/CMakeLists.txt:186–199`):
- `get_public_key`, `get_relays`: `nostr-homectl`, `nostrfs`, `nostr-home-fuse`, `nostr-home-syncd`, `nostr-home-fetch`, `nostr-homed-provision`, `nostr-home-status` under bindir, plus `nostr-notify-daemon` under libexecdir.
- `nip44_decrypt`: only bindir `nostr-homectl`.
- No stock implicit signing, encryption, conversation-key, or `nostr-seal` grant.

### 5. D-Bus preservation / whitelist / bounds

A programmatic XML comparison of all **32 pre-existing methods/signals**, including argument names/types/directions and member annotations, found **zero removals or changes**; only `CallWithContext` was added. This includes all §5.3 preserved signatures. Metadata changes are additive dictionary fields, not signature changes.

The wrapper has a closed six-method table: SignEvent, NIP44DeriveConversationKey, NIP04Encrypt/Decrypt, NIP44Encrypt/Decrypt. Exact `(sss)` tuples are required; management, recursion, unknowns and excluded variants cannot dispatch (`signer_service_g.c:190–229`). Plain direct and wrapped handlers reach the same `gated_call_full` and service/backend, not a self-D-Bus call or a separate execution engine (`135–154,220–225,367–387`).

Context rejects duplicates, unknown keys and wrong types; reason enforces both 160 characters and 512 bytes, purpose/flow_id 64 bytes (`200–215`). Shared submission checks argument/selector/context bounds **before event JSON parsing** (`signer_approval_service.c:941–948,973–979`): argument A ≤1 MiB, B ≤128 bytes, selector/claimed label ≤4096 bytes, plus required peer shape. These are handler-level bounds, not a promise that GLib never unmarshals a D-Bus message before dispatch. No application JSON parser is reached before the relevant size check.

### 6. G02-specific §9 verification gaps

**Confirmed gaps in the inspected tests, not a demand to implement unrelated G03–G12 features:**

- **Concurrency / ownership:** test actual owner-thread StoreKey/ClearKey against gate workers (F1); denial while queued (F3); cancellation while resolving, executing or blocked on key access; shutdown/unexport/re-export with jobs outstanding; reentrant reply callbacks that cancel another joined continuation, revoke or pause. Current joined cancel is before execution (`test_signer_approval_service.c:74–94`); the deterministic delayed-result test covers pause only (`147–180`). ASAN/UBSAN callback/backend-lifetime and TSAN execution evidence was not established here.
- **Join/remember matrix:** current tests prove canonical-event joining, changed plaintext and another principal (`74–112`), but do not assert isolation for changed identity, peer, operation/raw-versus-B64, kind/timestamp/tags, NULL-versus-empty inputs, or two successful joined replies with remember writing exactly one correctly scoped permission. The remembered no-identity connect test counts a grant (`131–136`), not a joined remembered-scope matrix.
- **Caller/transport evidence:** NIP-46 coverage checks a manually constructed caller, not verified remote request admission (`63–72`). Most service tests label a synthetic local caller's context NIP46 (`55–61`); that is not proof of transport authentication or two real remote clients interleaving. Existing D-Bus browser isolation (`test_signer_dbus_contract.c:1761–1968`) and NIP-5F integration (`2014–2130`) are useful retained coverage, not real NIP-46 acceptance.
- **Wrapped parity:** the new real-bus happy path exercises only SignEvent (`test_signer_dbus_contract.c:1252–1288`). It rejects malformed/oversized context and excluded names (`1227–1250`), but lacks successful parity for the other five allowed methods, wrapped typed-error opt-in/isolation, direct-versus-wrapped join/remember behaviour, changed reason/flow with unchanged authority, and oversized event/selector rejection through the wrapper. Existing typed-error tests call direct methods (`1609–1681`).
- **Defaults:** the real service test proves allowed reads/decryption, no signing, NIP46 exclusion, wrong inode and wildcard deny precedence (`test_signer_approval_service.c:241–290`). Add negative path/device/UID/sandbox cases and real peer-derived executable evidence; it currently supplies a synthetic SignerCaller and test override rather than exercising every compiled stock member.
- **Expiry and permission failure:** existing real-bus tests retain pending expiry (`test_signer_dbus_contract.c:1683–1721`); the new deadline test supplies an already-expired deadline (`test_signer_approval_service.c:194–205`). Missing deterministic expiry during backend work/joined delivery, remembered-grant write failure, and the queued permission mutation case. The new policy test writes a deny **before** approval (`228–239`), so cannot detect F3.

## Verification performed

- Pinned-source diff/read/call-path review and programmatic XML surface comparison as described above.
- `git diff --check master...origin/grotto/g02-shared-authorisation-service`: passed.
- Existing worktree build, `ctest -R '^nip55l_approval_service$' --output-on-failure`: passed (service plus `nip55l-test-homes` fixture).
- Existing worktree build, `ctest -R '^nip55l_dbus_contract$' --output-on-failure`: passed (contract suite plus fixture; 14.44 s for contract).
- Build cache selects `NIP55L_SECRET_BACKEND=libsecret` and Groundhog `secret-service`; no developer keychain manipulation. These were existing binaries, not a fresh build or sanitizer run. Passing focused tests is not production/remote acceptance and does not reproduce or exclude F1–F3.

## Recommendations

1. Resolve F1 before merging: the new worker boundary must include all concurrent backend/cache users.
2. Add execution-time permission invalidation/recheck for F3, with a deterministic queued-denial regression.
3. Record the F2 platform-contract exception explicitly or obtain a fail-closed policy decision; do not claim verified-name isolation where the source deliberately lacks it.
4. Add the bounded G02 regression matrix above, retaining the existing D-Bus/NIP-5F suites. Real NIP-46 verification is pending adapter integration, not proved by constructing a principal string.

No source changes proposed as a patch or applied. Report-only work: **no component version bump**.
