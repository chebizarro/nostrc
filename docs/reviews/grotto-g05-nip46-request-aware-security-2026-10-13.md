# G05 request-aware NIP-46 security review

## Context / Scope

**Verdict: REQUEST CHANGES.** The live Grotto path closes the account-key/core-ACL bypasses and authenticates remote callers correctly. One cache-policy defect is runtime-confirmed; a reconnect-cursor availability risk remains plausible. Passing tests do not establish the complete cancellation matrix.

Reviewed `git diff master...origin/grotto/g05-nip46-request-aware`: base/merge-base `eaf62ae4c436e649888817b5cce7aafc5321ca8a`, pushed head `3c3cbcfb8d60a54411e56e99f521b9ea9b36d7bb`. Source and line numbers below are from the supplied clean worktree `/Users/bizarro/Documents/Projects/.repoprompt-worktrees/nostrc/rp-agent-c85a441d-grotto-g05-nip46-request-aware-b7cd9e55`, whose HEAD equals that pushed head. Contract: plan §5.9, §5.2 **NIP-46 caller contract only**, and §9 `signer/bunker` / `signer/bunker-e2e`. No general protocol, UI, tracker, packaging, or version audit. No code/tracker changes, commits, pushes, hook installation, or keychain configuration changes. This report is the only new repository file; documentation-only, **no version bump**.

“Confirmed” identifies source-proven behavior or an explicitly described runtime reproduction; “plausible” identifies an unexercised consequence, not a demonstrated exploit.

## Findings (severity ordered)

### R1 — Medium / Confirmed: expired scopes can still release cached successful results

**Locations:** `apps/grotto/src/bunker_service.c:243-265`; `nips/nip55l/src/glib/signer_permissions.c:175-178,196-204,323-326`.

The adapter reloads permissions before checking a completed RPC's revision/generation. Reload recalculates `next_expiry` using only *future* expirations, but unchanged on-disk rows do not increment the revision. Once a grant expires, this reload removes its expiry fence; the cached revision can again appear current. The completed-RPC branch returns the old result without evaluating the operation's effective permission. This differs from an otherwise identical fresh request, which is rejected or requires review.

**Runtime reproduction:** a temporary copy of the E2E driver outside the checkout (`/tmp/g05-expiry-review.c`) exercised the unchanged daemon in headless mode, with an explicitly paired Medium client and a two-second `nip44_encrypt` grant. It encrypted once, waited three seconds, replayed the same signed envelope, then sent the same operation/params with a fresh RPC ID. Replay returned success; the fresh RPC returned `org.nostr.Signer.Error.NoApprover`. The entire modified driver exited 0; log: `/tmp/g05-expiry-review.log`, marker `EXPIRY PROBE CONFIRMED`. It used the CTest headless fixture's private HOME/XDG/bus and disposable transport backend, not the developer keyring.

**Impact boundary:** no new signing/encryption was observed on the cache hit, and this does **not** defeat explicit revoke. It permits post-expiry retrieval of an old account-operation result for the cache lifetime (including, by the same branch, cached decrypted plaintext). The expiry-policy fence intended by the adapter is ineffective after reload.

**Recommendation:** invalidate the effective-policy revision when expiry is crossed, or re-evaluate the cached RPC's pinned scope before releasing its result. Add the above headless expired-grant counterexample, including a decrypt result; preserve idempotent caching rather than re-executing crypto.

### R2 — Medium / Plausible: a rejected future envelope can poison reconnect `since`

**Locations:** `libnostr/src/subscription.c:462-478,504-517,574-584,1008-1009`; `libnostr/src/relay.c:1137-1144`; `apps/grotto/src/bunker_service.c:228,235-239`; `nips/nip46/src/core/nip46_session.c:2956-2968`.

The underlying subscription advances its reconnect cursor when an event is enqueued, **before** the Grotto freshness and pairing checks. A validly signed kind-24133 envelope from an unpaired sender can have a far-future timestamp, pass the subscription's lower-bound filter, advance that cursor, and then receive `stale_request`. A subsequent relay reconnect reissues `REQ` with that future cursor. A relay honoring `since` can consequently omit ordinary requests until that timestamp, despite the bunker reporting readiness after EOSE. This affects that relay's cursor; other relay cursors are independent, and total outage requires it to be the sole usable relay.

This is an interaction with inherited cursor machinery, not a newly introduced signature or account-authority bypass. The enqueue/refire sequence is source-confirmed; prolonged outage on an actual disconnect/reconnect was **not reproduced**. The current future-event assertion checks only the error, not the next `REQ` (`apps/grotto/tests/test-bunker-e2e.c:309-310`).

**Recommendation:** ensure freshness-rejected/unadmitted envelopes cannot advance this RPC subscription's recovery cursor, or use a bounded RPC recovery window independently of an untrusted maximum timestamp. Test a future envelope followed by reconnect and a normal paired request, asserting the emitted `since` and successful delivery.

### R3 — Medium / Confirmed coverage gap: “exactly once” E2E assertions count presence, not multiplicity

**Locations:** `apps/grotto/tests/test-bunker-e2e.c:69-73,104-115,319-337,464-483`; `apps/grotto/tests/test-bunker.c:176-177,208-212`.

Pause/revoke checks labelled “exactly once” establish an error response, but the capture table replaces values by RPC ID and the final audit deduplicates replies in a hash set. Two responses for one envelope would satisfy those checks. The core unit test does prove that a *second explicit completion call* fails and that late completion after stop is suppressed; it does not exercise two competing gate/lifecycle completions.

No bunker E2E counterexample covers deadline-versus-approval, disable/reconfigure-versus-delivery, shutdown with a pending gate call, queued-connect-ack cancellation, or cancelled backend completion. Thus no double completion was found in the implementation, but the full §5.9 assertion is not demonstrated.

**Recommendation:** count terminal replies per **request envelope** (intentional retry/cache responses are separate deliveries), late callbacks, backend invocations, and grants. Exercise each cancellation race with deterministic barriers. Keep unavailable live configuration controls explicitly deferred rather than calling the whole matrix passing.

### R4 — Low / Plausible: final transport-secret erasure is not compiler-resistant

**Locations:** `nips/nip46/src/core/nip46_session.c:605,3182-3187`; `nostr-gobject/src/nostr_nip46_bunker.c:165-171`.

Replacement uses `secure_wipe`, but destruction of the reused `bunker_secret_hex` slot uses ordinary `memset` immediately before `free`. An optimizing compiler may eliminate that dead store, leaving the new persistent transport secret in freed heap memory. This is inherited cleanup newly relied upon for transport custody; optimized-binary erasure and heap recovery were not tested.

**Recommendation:** use the existing non-elidable wipe on the destruction path too. No alternative key store or new abstraction is needed.

## Contract checks / evidence

### 1. Core bypass closure — confirmed for the installed-handler live path

Relay dispatch returns into `bunker_dispatch_deferred` before the legacy dispatcher (`nip46_session.c:2772-2776`). Direct `handle_cipher` likewise branches before the `s->secret` guard (`:3324-3328`); every parsed method, including unknown/control methods, goes to the handler (`:3256-3283`). Legacy `bunker_reply` is refused (`:3286-3287`). Signer-initiated URI connect grants ACL **only without** the handler (`:4071-4075`). Consequently no handler-dispatched RPC reaches legacy `get_public_key`, no-callback signing, direct account NIP-44, `acl_set_perms`, or `acl_has_perm`.

Transport encryption/decryption and envelope signing still use the dedicated transport secret, correctly (`:534-536,2402-2432,3081-3099`). The generic transport helper retains a legacy `s->secret` fallback; that is not an account-method bypass, and Grotto installs the explicit transport slot before listening (`bunker_service.c:359-369`).

Client `connect` perms are passed as display hints (`bunker_service.c:290-302`; `signer_approval_service.c:203-227,256-261`), not persisted core grants. The account selector comes from durable pairing/config, not connect params. `get_public_key` is scoped through nip55l's account resolver (`signer_approval_service.c:549-552`) and converted to account hex (`bunker_service.c:165-169`), not taken from the endpoint. The normal-success guarantee is account identity, not transport identity.

### 2. Caller authentication — confirmed; no relay-controlled principal found

The core checks recipient p-tag and outer signature (`nip46_session.c:2736-2769`), then decrypts using that verified author as the NIP-44 peer before invoking the handler (`:3257-3280`; default mode `:461-463`). Grotto constructs the principal only at the resulting callback (`bunker_service.c:292`). `signer_caller_for_nip46` lowercases the 64-hex pubkey and sets both principal/connection identity to `nip46:<hex>` (`signer_caller.c:75-84`); NIP-46 callers can never be approvers (`:364-366`). Claimed names and RPC params supply no identity authority. A relay can replay a valid envelope, not forge another client's authenticated principal without its key.

The public direct-cipher API deliberately relies on the embedding caller for outer-event verification (`nips/nip46/include/nostr/nip46/nip46_bunker.h:46-52`). Grotto does not use that as a network admission path; requests without an event ID also fail its durable replay guard. Account and transport roles remain separate in the live path.

### 3. Replay — sound revoke/restart boundary, with R1/R2 qualifications

Initial `since = now-600`; adapter accepts only ±600 seconds (`nip46_session.c:2956`; `bunker_service.c:228`). Before gate submission, durable metadata records event ID plus hashed client/RPC ID and method/params digest, expiring at event creation +600; no still-fresh entry is evicted for capacity (`bunker_service.c:193-210,247-289`). Corrupt storage fails closed at startup (`:319-337,381`). Completed response cache is RAM-only, 600 seconds / 1,024 entries; eviction leaves durable replay guards intact (`:118,178-190,276-283`). Changed method/params with the same RPC identity are rejected in memory or after restart (`:260,271-275`).

A relay replaying a pre-revoke `sign_event` gets `not_paired` **before** cache access; if already outside the window it gets `stale_request`. Revoke removes pairing/grants and disconnects cached/pending RPCs (`grotto_control.c:351-359`). After ordinary restart the result cache is gone but seen metadata survives: a recorded request gets `replayed_request`, not a second signature. Re-pairing does not erase those guards; explicit endpoint reset replaces the transport key, rendering old recipient/ciphertext bindings unusable.

### 4. Cancellation / owner context — source protections, incomplete runtime matrix

| Trigger | Implemented ownership / limitation |
|---|---|
| Deadline | Monotonic gate timeout finishes EXPIRED and marks cancellation; owner-context source (`signer_approval_service.c:631-641,987-995`). No deadline race in bunker E2E. |
| Revoke | Gate principal cancellation first, then durable removal and adapter disconnect; queued connect delivery is also fenced (`grotto_control.c:351-359`; `bunker_service.c:463-475`). Pending-sign revoke is tested. |
| Pause | Generation increments, all gate requests cancel; connect idle rechecks pause/generation (`signer_approval_service.c:1135-1144`; `bunker_service.c:124-140`). Pending-sign and cached replay cases are tested. |
| Disable / reconfigure | `bunker_service_stop` cancels unfinished handles, then fences/stops transport and clears RPCs (`:420-436`); start/restart invokes that stop. Method/admission setters cancel affected pending calls (`:489-508`). Public `ConfigureBunker` remains unavailable (`grotto_control.c:379-381`): selected-relay live reconfiguration preserving unaffected requests is **not delivered**, not proven. |
| Shutdown | Daemon unexports signer before freeing bunker; unexport shuts down gate requests (`main_daemon.c:415-421`; `signer_service_g.c:700-704`; `signer_approval_service.c:1157-1162`). E2E restart shuts down an idle daemon, not one with a pending request. |

Core completion uses an atomic once flag and a mutex-protected generation/listening fence (`nip46_session.c:3233-3254`); gate detaches continuations before reentrant replies and guards terminalization (`signer_approval_service.c:272-306`). Worker late results ignore finished requests (`:804-809`). GObject delivery attaches idle sources to the construction `GMainContext`, rather than inline worker invocation (`nostr_nip46_bunker.c:91-123`); backend replies likewise attach to the gate owner context (`signer_approval_service.c:900-905`). Owner-thread dispatch is actually asserted in `test-bunker.c:167-177`. These are once-only **continuation/result** protections; they cannot undo cryptography already inside a backend call.

### 5. Custody — distinct slot, explicit loss/reset; R4 and backend coverage caveat

Secret Service schema/attribute exactly match `org.nostr.Grotto.BunkerTransport`, `slot=default`; Keychain uses service `Grotto Bunker Transport`, account `default`, nonsynchronizable (`signer_ops.c:1330-1334,1364-1381`). Identity listing uses the identity schema/service, not this slot (`:1618-1645,1648-1659,1709-1745`). Loading never creates a key; missing custody yields ERROR, not regeneration (`bunker_service.c:345-362`). Reset authenticates through Control1, stops first, clears remote grants including orphaned rows, removes remote clients, then stores a new key; failure leaves the listener disabled (`grotto_control.c:218-282,287-295`).

Temporary key bytes, store copies, plaintext requests, queued replies, and cached results use secure wiping (`signer_ops.c:1414-1422`; `bunker_service.c:50,341-362`; `nip46_session.c:3141-3142,3222-3227,3272`). Persistent secret destruction has R4. Real Secret Service/Keychain round-trip, non-enumeration, and reset persistence-failure behavior were not exercised: the bunker tests explicitly select a guarded disposable **file** transport backend (`test-bunker.c:312-313`; `test-bunker-e2e.c:594-596`). No developer keychain test was attempted.

### 6. Headless — no unattended admission / allow-all found, except cached expiry R1

The daemon sets no-approval-UI on both gate and adapter. Unknown nonallowlisted connect is refused; the legacy allowlist only admits a new client to the gate, where ASK fails promptly. Existing pairing permits reconnect, not account crypto. The gate's headless fast check is negative-only; ALLOW still resolves/rechecks the account (`signer_approval_service.c:675-705,750-758`; `bunker_service.c:235-244,294-302`). Empty allowlist is no longer startup-fatal because durable paired clients may operate; it is **not** “allow all.” Old auto-approve-kind/session APIs are inert (`bunker_service.c:515-520`). Token/URI entry points are unavailable rather than presented as unattended admission. Headless E2E asserts no approval signal and NoApprover for an ungranted operation (`test-bunker-e2e.c:291-293,505-506`); R1 qualifies continued cached access after a grant expires.

### 7–8. Legacy compatibility and §9 coverage

- Legacy dispatch remains opt-in-compatible when no request handler is installed. Homed integration sources are unchanged by this diff. The sole changed `nips/nip46/tests` file copies the fixture before a middleware callback that now consumes its event; admission assertions are retained (`test_nip46_signed_response_admission.c:124-130`). All 31 registered core NIP-46 tests passed. Homed NIP-46 execution is **unverified here**: existing build has `ENABLE_NOSTR_HOMED=OFF`, with zero matching CTests.
- `WILL_FAIL` is removed; E2E is now ordinary success, but still permits skip 77 when the bus fixture is unavailable (`apps/grotto/tests/CMakeLists.txt:454-464`). It actually ran, not skipped, in this review.
- **Three mandated assertions are present and passed:** returned key equals account (`test-bunker-e2e.c:232`); differs from transport (`:233`); revoke denies the replayed pre-revoke signed request (`:340-341`). The test also verifies the event's account author/signature and independent-peer NIP-04/44 interoperability (`:240-272`).
- `signer/bunker` exercises real lifecycle/config failure, missing custody, stop/start, all-method core interception, empty method ceiling, rejected OK/CLOSED/AUTH, queued-start cancellation, and corrupt replay format (`test-bunker.c:309-324`). It does not establish nonempty per-method scope-denial coverage for every operation.
- E2E covers interleaved clients, deferred decisions, fresh/cache/changed-method replay, stale/future events, a separately signed wrong client, pause/revoke, restart, and endpoint reset. Wrong-client testing is **not** a forged-author/ciphertext-peer-mismatch negative test. Missing: bad signature/wrong recipient/wrong encryption peer through the new server lane, same-method changed params, cache eviction with durable seen guards, replay-store write failure, R1/R2 regressions, and R3's race/count matrix.
- §9 token reuse is absent because CreatePairing/AcceptConnectURI remain unavailable (`grotto_control.c:382-390`). Record this as G06-deferred, not evidence that token admission is safe or complete. Do not expand this review into implementing pairing/UI/configuration.

## Verification and recommendations

Focused rebuild passed: `cmake --build _build --target grotto-daemon test-bunker-startup test-bunker-e2e -j4`.

`ctest --test-dir _build --output-on-failure -R '^(signer/bunker(-e2e|-headless)?|test_nip46_.*)$'`: **35/35 passed** (31 core tests, three bunker tests, one fixture), 25.83 seconds. `git diff master...HEAD --check` passed. Logs: `/tmp/g05-review-build.log`, `/tmp/g05-review-ctest.log`. All 31 core NIP-46 executable targets were subsequently rebuilt explicitly and `ctest --test-dir _build --output-on-failure -R '^test_nip46_.*$'` passed **31/31** again (1.21 seconds); logs `/tmp/g05-review-core-rebuild.log`, `/tmp/g05-review-core-retest.log`. Separate expired-scope headless probe passed its counterexample against the same daemon. No full CTest, sanitizer/race, Linux homed, or real keyring acceptance claim.

Priorities: fix and regress R1; validate/fix the bounded reconnect cursor in R2; strengthen exact-once counters and the implemented cancellation triggers in R3; replace destructor memset in R4. Keep G06 controls/token work and homed/backend acceptance explicitly outstanding. **No confirmed remote account-key signing bypass or principal forgery found at this snapshot.**

Navigation note: Jev `find-lines` located the contract, expiry-fence, and cursor spans (11 requests / 105,004 input tokens / USD 0.004410 recorded); one `filter-search` usage summary was not retained. The expiry query led to the runtime probe; all findings were checked in source, not accepted from navigation scores.
