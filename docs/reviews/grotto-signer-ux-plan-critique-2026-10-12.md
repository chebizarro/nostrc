# Critique: Grotto Signer UX plan vs. context_builder baseline

Date: 2026-10-12
Subject: `docs/plans/grotto-signer-ux-2026-10-12.md` (the plan)
Baseline: `prompt-exports/oracle-plan-2026-10-12-151445-grotto-signer-ux-pla-c3a2.md`, from `## Generated Plan` (line 131) onward (the export)

## Scope and method

- Only the five requested lenses are covered.
- I read both documents in full and spot-checked the seams named below against the code.
- Code facts I verified myself are cited `file:line`.
- Anything I did not verify is marked **(unverified)**.
- Section numbers (§) refer to the plan.

Verified or checked and found accurate: the plan's citations for the `ExecReload`/`IPAddressDeny` unit lines, `bunker_deny_unattended` (`main_daemon.c:94-102`), the ignored listener failure (`bunker_service.c:431,438`), `nostr_nip46_bunker_listen` returning -1 on a NULL secret (`nip46_session.c:2798-2804`), the `connect` params handling (`:3279-3300`), and the absence of a Grotto row in the `VERSION_MANIFEST.md` component table.

---

## 1. Export content missing, weakened or generalized in the plan

The plan is close to a lossless carry-over. Every method signature, field list, limit, state enum, token rule and ctest name in the export survives. Differences that matter:

1. **Lock-copy rule weakened (§5.8).**
   - The export says to rename auto-lock settings *and* "do not retain 'keys locked' copy without backend enforcement."
   - The plan keeps only "rename the auto-lock settings to say which they control."
   - The enforcement-linked prohibition is gone, so no test or promotion-gate line catches leftover "keys locked" strings.
   - Restore it as a `signer/ui-layout` or `desktop-contract` assertion.
2. **Social-recovery gate dropped; OC-3 contradicts §4.4.**
   - The export hides social recovery "until share creation/reconstruction and daemon custody round-trip are tested."
   - The plan folds it into OC-3's generic "end-to-end tested signing/custody path."
   - OC-3's rationale says "Scout found them unwired (§4.4)", but §4.4 lists social recovery (`page-settings.c:450`) under **Wired**, and `tests/test-social-recovery.c` exists.
   - Hiding a wired, tested feature needs its own stated reason and unhide criterion, not an "unwired" label.
3. **"Absent from feature claims" lost.**
   - The export requires hardware, multisig and delegation to be "hidden *and absent from feature claims*."
   - The plan says only "UI stays hidden" in §5.7.
   - The metainfo and desktop-contract items should assert it (§5.11 covers metainfo only generically).
4. **Activity signal generalized.**
   - The export has `ActivityChanged(t sequence)` and `ListActivity(before_sequence)`.
   - The plan says "monotonic revisions", which blurs the activity sequence number with the snapshot revision used for the other three signals.
   - State that activity uses the record sequence, so a GUI can fetch "after N".
5. **"Needs review" has no concrete surface.**
   - The export says only that migrated allows are recorded as "Needs review"; the plan repeats it.
   - There is no storage field, no `ListPermissions` shape, no UI location and no definition of how a user clears the state.
   - Carried over faithfully, but it is a load-bearing term with no implementation content in either document.
6. **Version "correction" overstated (§8, §11).**
   - The export's "Grotto 0.2.0 / nip55l 0.8.0" was explicitly "provisional, subject to `AGENTS.md`/tags."
   - The plan labels it a *correction* and then leaves "add a Grotto row?" as open question 3.
   - The nip55l fold-into-0.7.0 rule is sound (manifest rows 40, 64-66 show the same practice).
   - Call it "resolved the export's deferred policy lookup", not a correction.

## 2. Under-specified seams, unresolved decisions, contradictions, wrong references, missing dependencies

### 2.1 The existing gate entry point is not named (§5.2, §7)

- `nips/nip55l/src/glib/signer_gate.h` already defines a transport-neutral submit contract:
  - `signer_gate_submit(who, conn_key, op, a, b, selector, reply_fn, user_data)`.
  - A once-only `SignerGateReplyFn`.
  - `signer_gate_connection_closed(conn_key)`.
  - A documented guarantee that a NIP-5F request is decided "exactly like the same D-Bus call from the same process."
- The plan describes "internal submit/complete/cancel and trusted transport-principal contracts" as new, and lists `signer_gate.h` only as "Align with extracted service."
- Gaps the plan never states:
  - `SignerGateOp` has four ops (`GET_PUBLIC_KEY`, `SIGN_EVENT`, `NIP44_ENCRYPT`, `NIP44_DECRYPT`).
  - §5.9's method table needs `nip04_*`, `connect` and `ping`.
  - The service must add those ops.
  - Pairing-style ops with no key need an explicit "no identity" mode.
  - `SignerGateReplyFn` has no cancel handle, so G02 must add one for pause, revoke and expiry.
  - `SignerCaller` is derived from bus or socket peer credentials. The plan's "trusted transport-principal contract" must say how a `nip46:<hex>` caller is built:
    - Is it "identified", so remembering is allowed?
    - Is `is_approver` forced false?
    - What is its `conn_key`?
- The gate also has a *built-in defaults for first-party headless services* step (signer_service_g.c:~50). It is absent from the §5.2 processing order and from the §5.10 migration. After v2 evaluation is enabled, `nostr-notify-daemon`, `nostr-seal`, homed and similar services would either lose access or keep an invisible bypass.

### 2.2 "Create/join a pending request": join key undefined (§5.2 step 6)

- Today "identical queued calls from one connection share a request" (gate doc). With exact-kind scoping and batching, "identical" needs a definition.
- Candidate join key: principal + identity + operation + canonical-argument digest.
- Specify it, and say what a joined continuation inherits when the decision is "remember".

### 2.3 Control1 is called "canonical" but is incomplete (§5.3)

- **Missing methods.** The plan states "Management also needs authenticated identity-export operations (§5.7)" without signatures. Also absent:
  - identity rotation;
  - a setter for "Default for older apps" (§5.4/§5.7 requires an explicit separate setting; today it is GSettings `default-identity`, written by the GUI and read by the daemon, `main_daemon.c:137-140`);
  - a client-label setter (§5.4 "user label", §5.11 "real client-label field").
- **Bus name unstated.** Control1 is "exported by `grotto-daemon`" at `/org/nostr/signer/grotto` but §5.3 never says on which bus name (presumably `org.nostr.Signer`; "owning `org.nostr.Grotto` alone is insufficient" implies the other name is the approver's).
- **Auth helper not wired.** The authentication helper is `signer_caller_is_approver()` (`signer_caller.h:113`), in an internal nip55l `src/glib` header. §7 lists no change to expose it to `apps/grotto/daemon`.
- **Lost-update race.**
  - `SetPermissions(principal, identity, permissions)` is "one complete replacement" with no base revision.
  - The daemon concurrently writes scoped grants from `ApproveRequest(remember=true)`.
  - A GUI that read, edited and replaced will silently delete a grant created in between.
  - Add an expected-revision argument, or per-record add/remove semantics.

### 2.4 Self-test: undefined principal, and an ordering contradiction (§5.7, §10)

- The self-test "travels the real daemon gate and shows **one** review naming the operations."
- The gate requests are three separate calls (sign, NIP-44 encrypt, NIP-44 decrypt), made by the Grotto GUI, which is also the approver.
- Undefined:
  - **Principal.** What is its principal, and may the approver approve its own request? The request-shape `MANAGEMENT` transport (§5.2) is never used or defined anywhere else.
  - **Grouping.** One-review grouping needs `flow_id`/batching, which the plan places in G11 (item 12). G09 (item 10) depends only on G04 and G07, so G09 cannot meet its "Done when" without pulling G11's grouping forward.
  - **Event kind.** The "harmless" event's kind is unspecified. It interacts with required-confirmation kinds and with `signer/permissions` tests.
- Decide: a dedicated `MANAGEMENT` self-test request type (one request, three operations), or three requests grouped by an internal `flow_id`. State the principal either way.

### 2.5 `CallWithContext` (§5.3, §10)

- **Variant carve-outs.** "Whitelist existing gated client methods from the canonical XML" is ambiguous for variant families:
  - the `*ForApp` methods carry a caller-claimed `app_id` that is only honoured for the browser bridge (gate doc, step 1);
  - the `*B64` variants;
  - `DecryptZapEvent`;
  - `EnableTypedApprovalErrors`, which is per-connection state.
  - Decide which are in or out, and that typed-error mode applies to the wrapped reply.
- **Landing item.** The only work item naming it is G11 (item 12). But the request fields `reason` and `flow_id` (§5.2), the `GetApprovalInfo` additions (G07) and the nip55l XML change all belong to the same atomic nip55l change as G02. Landing the XML signature twice invites ABI churn inside the "fold into 0.7.0" version plan. Decide whether G02 lands the entry point (with reason/flow plumbing) and G11 only adds the Groundhog adapter.

### 2.6 Notification semantics with coalescing (§5.5)

- "Coalesce into one summary notification when several pend", yet buttons are **Review** and **Deny** ("deny once").
- On a summary notification, which request does Deny target? Candidates: all pending, the oldest, none.
- Denying unseen requests from a content-free notification is exactly the risk OC-1 rejects for Approve. Either:
  - drop Deny on summary notifications; or
  - define it as "deny all currently pending", with a visible count.

### 2.7 Ctrl+Q / "Quit Approval UI" with pending requests (§5.1, §5.6)

- **Pending work at quit.** The daemon summons the approver via `StartServiceByName` (`approver_summon`) whenever a request needs consent. So Quit is not sticky: the next request re-launches the UI.
  - What happens to requests pending at quit time: deny, leave to the 300 s TTL, or hand off?
  - Not stated.
- **Stale notifications.** Notifications persist after exit. The plan's stale-action handling covers this; still, define whether quitting withdraws them.

### 2.8 Bunker `LISTENING` definition (§5.9)

- "Accepted subscription registration." Nostr relays do not acknowledge a `REQ` except with `EOSE` or `CLOSED`.
- Define it: `EOSE` received on at least one relay and no `CLOSED`/auth-required rejection.

### 2.9 `nostrconnect://` relays vs configured relays (§5.9, §5.10)

- In `nostrconnect://`, the client names the relays and the signer must answer and receive requests on them.
- The plan says "contact only user-confirmed relays" and "complete the encrypted connect ack", but:
  - `ConfigureBunker` and `bunker.ini` model one signer-wide relay list;
  - `clients.ini` v1 has no relay field;
  - the systemd network policy (§5.11) assumes a known set.
- Undecided: are per-client relays persisted, subscribed after restart, and counted in `LISTENING`/`DEGRADED`? This changes the `clients.ini` schema and the `ConfigureBunker` contract.

### 2.10 Legacy allowlist meaning (§5.10 vs §5.9 G00)

- G00's smoke uses "a nonempty client allowlist" (today's admission). §5.10 makes `bunker-allowed-pubkeys` "admission-only legacy clients needing review."
- Can an allowlisted legacy pubkey `connect` without a pairing token? The `connect` method row says "Pairing token or existing-paired check". Define whether legacy-admitted counts as "paired".

### 2.11 Preview table has no pairing/`connect` row (§5.5)

- `connect` is a new operation that creates a review, and carries the Amber-style trust-tier choice.
- The readable-preview table has no `connect` row (what is shown: transport key fingerprint, claimed name, requested permissions, chosen identity).

### 2.12 Test seams named but not specified (§9)

- The loopback relay and the NIP-46 client for `signer/bunker-e2e` are not named (which in-tree relay and client?).
- "Disposable test key backend" for the daemon (headless CI has no Secret Service) is a fixture with no implementer. The new transport-key slot needs the same backend switch.
- `signer/background` and notification tests need fake `org.freedesktop.portal.Desktop` and `org.freedesktop.Notifications` services on the private bus. The plan does not say who writes them.
- `signer/blueprint-drift` needs a pinned `blueprint-compiler` version, or a Noble/Resolute CI matrix gives false drift.

## 3. Details the code disproves, the task does not need, or a simpler named design replaces

### 3.1 Core NIP-46 server answers outside any callback, and has its own ACL (verified; plan under-states it)

In `nips/nip46/src/core/nip46_session.c`:

- `get_public_key` returns `nostr_key_get_public(s->secret)`: no ACL, no callback (`:3120`).
- `sign_event` signs with `s->secret` when no `sign_cb` is installed (`:3143` onward).
- `nip44_encrypt/decrypt` use `s->secret` directly and "no callback is supplied" (`:3200-3230`).
- A second authorization store exists: `acl_set_perms`/`acl_has_perm`.
  - It is filled from the **client-supplied** `connect` params[2] (`:3279-3300`) and from the `nostrconnect://` URI `perms=` (`:3833`).
  - With no perms it grants all six default methods (`:3451`).
  - `authorize_cb` is a synchronous `int` (`:3287`).
- The plan's "request-aware async handler" and "separate transport key" do not say these built-in paths must be switched off. Consequences if any one remains reachable once the transport key differs from the account key:
  - `get_public_key` silently returns the *transport* identity.
  - NIP-44 uses the transport key.
  - Client-requested permissions persist in the core ACL after the daemon revokes the client.
- **Correction.**
  - When the request-aware handler is installed, the core must route *every* method, including `get_public_key`, `ping` and `nip44_*`, to it, and must never consult `s->secret` as the account key or the core ACL.
  - Client-supplied `perms` (connect params and URI) become hints that seed the review UI, never grants.
  - Add to `signer/bunker-e2e`: returned pubkey equals the account key and differs from the transport key; revoke removes access even from a replayed pre-revoke request.
- The core already carries a separate `bunker_secret_hex`/`bunker_pubkey_hex` pair in the `nostrconnect` path (`:3825-3830`). The transport-key setter (§5.9, "no reliance on unexplained auto-generation") should reconcile with this existing field rather than add a third secret concept.
- G00's "does it listen?" question is the same conflation: the `listen` guard checks `s->secret` (the account/signing slot), which the wrapper never sets. G00 should assert which field is populated.

### 3.2 Onboarding has two create paths in code (§4.4, §5.7)

- `onboarding-assistant.c:413-446` defines `CreateProfileCtx` and `on_create_profile_done()`, calling the daemon's `CreateProfile` method.
- `CreateProfile` is in `org.nostr.Signer.xml` and carries a `passphrase` argument "reserved for future use."
- The plan describes only the `StoreKey` path (`store_generated_key`, `:569-610`) and says to remove the creation passphrase.
- **Correction.** Name which path is live (I did not trace the callers **(unverified)**) and either delete the `CreateProfile` path or document why it stays. A daemon-generated key cannot yield the BIP-39 phrase the plan wants to preserve, so `StoreKey` is the only compatible path. Do not extend `CreateProfile`'s reserved arguments.

### 3.3 `auth_url` and `grotto://request/<id>` (§5.9)

- Both are optional and "never advertised as universally supported." With notification-first approval (OC-1), no unmet requirement drives them.
- No work item (G05, G06, G08) lists `auth_url` emission as a deliverable, so it is an orphan.
- **Correction.** Mark it explicitly deferred. Keep the `grotto:` open handler only if it has another consumer (the plan's §5.11 handler list). This avoids shipping an authority-adjacent URI scheme with no caller.

### 3.4 `ExecReload`/SIGHUP removal vs "bunker drop-in"

- Accurate per `grotto-daemon.service:16,68-69`. The plan's remedy is right. No correction, but note that headless `--no-approval-ui` (G05/G06) needs an `ExecStart` variant, not only a drop-in with the IP filter relaxed.

## 4. Requirements and edge cases absent from both documents

1. **Replay of old NIP-46 request events.**
   - §5.9 caches completed RPCs for 10 minutes and dedups by event ID, but sets no `created_at` freshness window and no `since` filter.
   - After the cache expires, or after a restart, a relay can replay a stored request (for example `sign_event` for a kind with a remembered allow). The daemon would execute it again.
   - Required:
     - subscribe with `since = now − skew`;
     - reject events outside ± a stated window;
     - keep the seen-ID window at least as long as the freshness window;
     - decide whether it survives a restart.
2. **Caller gave up before the decision.**
   - The NIP-07 host times out at 120 s (`main_native_host.c:211`) and the daemon TTL is 300 s.
   - Under notification-first approval, a user can approve at 150 s. The daemon then executes and produces a result nobody receives (a consumed grant, or a "remember" the user did not intend).
   - NIP-46 clients that retry with a fresh RPC ID create duplicate reviews, because RPC-ID dedup does not cover them.
   - Define a deadline derived from the caller where it is known, and join or supersede retries (same principal + method + params digest).
3. **Pause vs in-flight execution.**
   - EXECUTING work can be waiting on a keyring unlock prompt, which cannot be cancelled.
   - The plan says "results already produced or sent cannot be recalled."
   - State the middle case: a result produced after the pause but not yet handed to the transport is suppressed and returns a paused error.
4. **Client/daemon version skew.**
   - The manual matrix includes "daemon upgrade", but there is no policy for a new GUI against an old daemon without Control1, or the reverse. `GetStatus` capabilities exist; say what the GUI does when they are missing (read-only mode, refusal, or upgrade prompt).
5. **Transport-key loss and reset.**
   - If the keyring entry (`org.nostr.Grotto.BunkerTransport`) is lost, every persisted pairing in `clients.ini` points at a dead endpoint.
   - Define status reporting, an explicit "reset bunker endpoint" action that revokes pairings, and the uninstall/profile-reset behavior.
6. **Persistence concurrency.**
   - The daemon is the single writer of the v2 permissions and `clients.ini`, but the legacy `signer-grants.ini` is still written by older GUIs and read by `signer_grants.c`.
   - State what the new daemon does if it finds a legacy broad allow written *after* migration (re-run the unsafe-allow removal, or treat the file as untrusted input).
7. **Cancellation triggers for NIP-46 continuations.**
   - §5.2 says "transport disconnect removes its continuations", but NIP-46 has no connection.
   - The real triggers are deadline, revoke, pause, bunker disable or relay reconfiguration, and the client's `disconnect`. List them.

## 5. Questions that would materially change the design or order

1. **Core NIP-46 ownership and consumers.**
   - Who besides Grotto uses `nostr_nip46_bunker_*` and the built-in method paths (signet, tests, homed)?
   - If other consumers rely on the built-ins, "retain legacy APIs" in §5.9 is a hard constraint and the handler must be opt-in. If only Grotto uses them, the legacy server paths can be removed, which is smaller and safer.
2. **Extend `signer_gate_submit` or replace it?** This decides G02's size (L, 6-9 days) and the NIP-5F regression surface (see §2.1).
3. **Are `nostrconnect://` per-client relays persisted and subscribed?** This changes `clients.ini`, `ConfigureBunker`, the `LISTENING` definition and the systemd policy (see §2.9).
4. **One review or three for the self-test?** Single request type or grouped requests decides whether G09 depends on G11 (see §2.4).
5. **Does `CallWithContext` land in G02 or G11?** This decides whether nip55l's XML and the version fold are touched once or twice (see §2.5).
6. **Order: G01 versus G05.**
   - G01 builds production `signer/bunker` tests against `bunker_service.c`, which G05/G06 then largely rewrite (async adapter, pairing, reconfigure).
   - Consider limiting G01 to the "no false RUNNING" fix plus keeping the G00 smoke, and moving the broader `signer/bunker` suite to G05 to avoid throwaway tests.
7. **Which wins, 120 s or 300 s?** Is the native-host timeout (120 s) shortened to the approval window, or does the daemon learn caller deadlines (see §4.2)? This shapes the request `deadline` semantics for OC-1.
8. **Is a legacy-allowlisted client "paired"?** (see §2.10)
9. **Social recovery.** Is hiding a wired, tested feature intended (OC-3 scope), or should it stay behind a documented gate? (see §1.2)
