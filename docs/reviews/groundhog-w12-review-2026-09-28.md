# Groundhog W12 integrated peer review — 2026-09-28

## Context / scope

This is an independent review of exactly `1d58102e4c2e2ce523ac6b9e8f5923ccde80fab6..d12fcc89bab3d6027f0d89064fecd837ba96dfe6`, ignoring beads-only commits (`2854b18a`, `7fec336d`, `694af63a`, `c63d7173`, `4e20ae1a`, `861415ce`, `1e485ad7`):

- `e6a9a28d`: privacy and modern-UX charter (`docs/designs/groundhog-privacy-ux-charter-2026-09-28.md`, nostrc-qp24.17). It is the normative reference for the privacy checks below.
- `a8a9c0d7`: libmarmot requires an UpdatePath when a Commit's proposals demand one (nostrc-jnfp). Security fix; libmarmot 0.3.6.
- `8d310110`: NIP-17 receive pipeline and conversation model (`gh-dm-inbox*`, `gh-conversation*`, `gh-message*`; nostrc-qp24.10.5).
- `38c7f51b`: NIP-17 send pipeline (`gh-dm-send*`, `gh-inbox-lookup*`, `gh-inbox-resolver*`, `gh-message-status.h`, self-DM envelope).
- `687dbd13`: seal `expiration` acceptance and per-layer expirations (`gh-nip17-inbox*`, nostrc-qp24.24).
- `d12fcc89`: nostr-gobject 2.0.1 `state-changed` fix (nostrc-qp24.4.5), prompt publish write/loss failures (qp24.4.8), and NIP-42 AUTH by caller-chosen identity for scopes and publishes (qp24.4.9).

This review approves the code and tests only. It does not cover release readiness, live relay or signer acceptance, or UI wiring. In the shipped executable, the relay layer is linked through `groundhog-account-relays`, and `gh-nip17-inbox.c`, `gh-nip17-envelope.c` and the send sources are compiled in (`gnome/groundhog/CMakeLists.txt:155,221,266`). No production code instantiates `GhDmInbox`, `GhDmSender` or `GhInboxLookup` yet, and `groundhog-dm-inbox` is not linked into the app (`:247-257`). I checked the specs against the current [NIP-17](https://github.com/nostr-protocol/nips/blob/master/17.md), [NIP-42](https://github.com/nostr-protocol/nips/blob/master/42.md), [NIP-40](https://github.com/nostr-protocol/nips/blob/master/40.md) and RFC 9420 §12.4/§17.4. I changed no code or beads.

## Findings

### Blocking

None.

### Reviewed areas — no blocking finding

- **libmarmot pathless-Commit rejection (`a8a9c0d7`)**
  - **Rule.** `commit_path_required` (`libmarmot/src/mls/mls_group.c:1926-1942`) treats an empty Commit, or any proposal type other than Add, PSK, ReInit and AppDataUpdate, as path-required. Unknown types fail closed. This matches the RFC 9420 §17.4 "Path Required" column: Add N, Update Y, Remove Y, PSK N, ReInit N, ExternalInit Y, GCE Y.
  - **Placement.** The receive check (`:2466-2473`) runs after every ProposalRef is resolved and type-checked (`:2444-2464`). That is the earliest point at which by-reference Updates are known. It also runs before ordering, PSK and any apply step. It exits through `staged_fail` (`:2938-2944`), so the live group is never touched. The encoder also refuses such a Commit (`:3541-3545`).
  - **Proof test is differential, not theater.** `test_pathless_remove_does_not_exclude_removed_member` (`libmarmot/tests/test_mls_group.c:1907-2014`) works in three steps:
    1. It shows that the removed member (Bob) can compute, from his parent-epoch state and the public Commit alone, secrets that reproduce the pathless Commit's confirmation tag and equal the epoch an accepting receiver would install.
    2. It shows that the receiver rejects that Commit with its state byte-identical to before.
    3. For contrast, Bob rebuilds the tree hash and transcript of libmarmot's path-bearing Remove, but misses the tag and the encryption and exporter secrets.

    In `test_pathless_commit_requiring_path_rejected` (`:1792-1902`), every one of the five cases records and rolls back an acceptance instead of aborting. The final `accepted == 0` therefore catches a regression in any case. `test_pathless_add_and_psk_commits_accepted` checks the negative space: path-optional Commits still reach exactly the derived epoch.
- **nostr-gobject 2.0.1 (`d12fcc89`)**
  - **Emission path.** Emission now compares against a main-context-only `emitted_state` (`nostr-gobject/src/nostr_relay.c:158-205`). The worker still stores `state` atomically and queues one idle per transition with a serial number (`:233-261`). Signals are still emitted only from the default main context. `gnostr_relay_disconnect` raises a barrier, so transitions queued before it are dropped (`:884-891`, `:225`).
  - **Regression test.** `test_state_changed_every_transition` (`nostr-gobject/tests/test_relay_connect.c`) requires a lone CONNECTED→DISCONNECTED emission on the main thread. That transition was swallowed before this fix.
  - **Impact on other consumers.** I read every `GNostrRelay::state-changed` consumer outside Groundhog:
    - `apps/gnostr/src/gnostr-plugin-api.c:844-855`
    - `apps/gnostr/src/ui/gnostr-dm-service-send.c:424-445`
    - `apps/gnostr/src/ui/gnostr-main-window-publish.c:584-605`
    - `nostr-gobject/src/nostr_pool.c:113-121`, re-emitted to `gnostr-main-window-pool.c:129-147` and `gnostr-main-window-relay-manager.c:1823`

    Each was written for the documented contract and is idempotent under repeated emissions. The publish attempts guard on `publish_requested` and `settled`, and `ensure_live_multi_sub` guards on `live_multi_sub`. The fix therefore turns handlers that were dead into the intended behaviour, for example a prompt "disconnected before OK" instead of a timeout. It does not add hazards.
  - **`gnome/*`.** No other `gnome/*` component connects to it; Groundhog's two transports are covered below.
- **NIP-42 AUTH (charter §4.4)**
  - **R1: identity.** Identity is an explicit per-URL choice, and the default is NONE (`gh-relay-auth.h:11-51`). ACCOUNT refuses a missing, revoked or other-generation signer (`gh-relay-auth.c:385-391`). EPHEMERAL never touches the account signer (`:399-401`). No automatic escalation exists. A refused ephemeral AUTH ends as CLOSED or AUTH_REQUIRED, as scope and publish tests assert.
  - **R2: ephemeral keys.** A key is generated per attempt and copied into a `secure_alloc` buffer. The hex copy is wiped at once (`:194-201`), and the buffer is freed with wipe right after signing (`:219-221`). That is stronger than the charter's "wipe on socket close". `ephemeral-fresh-per-connection`, `ephemeral-two-relays` and their wire variants assert that keys are distinct per connection and per relay.
  - **R3: verification.** Signed results are verified locally before sending (`:257-293`): id and signature, kind 22242, every `relay` and `challenge` tag bound, `created_at` within ±600 s, empty content, and the expected pubkey.
  - **R4: stale generation.** Revocation cancels pending sign operations outside the lock (`:100-116`). `complete()` turns a success that raced a revocation into PERMISSION_DENIED (`:322-324`).
  - **R5: one AUTH per challenge.**
    - Scope: `maybe_auth` and `auth_notice` (`gh-relay-scope.c:403-470`). A second `auth-required` for the same challenge is reported, not retried.
    - Publish: `resent` makes a second `auth-required` terminal (`gh-relay-publish.c:596-608`).
    - Both: the AUTH's own OK is matched on the exact AUTH event id and never surfaces as the published event's outcome.
  - **Lifetime.** An attempt holds two references, one for its owner and one for the operation. `drop` detaches the owner and cancels, so the callback never runs afterwards. DISCONNECTED, cancel, a URL identity change and `close_transport` all drop the attempt.
  - **Not used: GNostrRelay's `set_auth_handler`.** Groundhog never installs it, so there is no synchronous or EVENT-envelope AUTH.
- **Transport threading (`gh-relay-publish-gnostr.c`, `gh-relay-gnostr-write.c`)**
  - **Signals.** Signal handlers only copy their arguments and queue them to the owning context, and a `closed` flag is checked there (`gh-relay-publish-gnostr.c:88-172`).
  - **Writes.** Frame writes run on a GTask worker. The worker waits on libnostr's answer channel for at most 5 s, then closes and unrefs its reference, which matches the two-reference contract at `libnostr/src/relay.c:1462-1490`. It completes on the owning context (`gh-relay-gnostr-write.c:33-98`).
  - **Failures.** A failed write or a lost connection is now reported as CONNECTION_FAILED at once, before or after the handshake (`:104-111`). This closes W10 non-blocking #2.
  - **References.** Handle references cover each signal closure, the pending connect, each write and each queued delivery.
- **Relay minimization (charter PD-4, §0.1 item 3)**
  - **Recipient wrap.** Targets come only from `GhInboxResolver`, whose contract forbids 10002, home or default relays (`gh-inbox-resolver.h:14-22`). The lookup admits only signed kind 10050 from that author, newest first, with no fallback to an older or EMPTY list (`gh-inbox-lookup.c:292-327`).
  - **Self-copy.** It goes only to the account's own `gh_account_relays_get_inbox_relays` (`gh-dm-send.c:691-698`).
  - **Isolation.** Each wrap gets its own `GhRelayPublish` and so its own private sockets. Legs never share a connection (§4.5 S3).
  - **Without a 10050.** Nothing is sealed or published (`:714-724`).
  - **Tests.** `forged-inbox-ignored` feeds a recipient 10002, a tampered list, another author's list and a future-dated list from discovery and own-relay sources. It asserts that no publish opens, and then that only `BOB_A` and `ALICE_INBOX` receive traffic (`tests/app/test_dm_send.c:895-935`).
  - **No account AUTH on others' relays.** No production caller sets any AUTH identity, so lookups, recipient publishes and the inbox REQ are all unauthenticated.
- **Stale generations**
  - **Send.** Every send callback checks `is_current` (generation plus the operation's cancellable) before recording anything. A late relay OK finishes as CANCELLED and never counts as accepted (`gh-dm-send.c:257-264,514-557,634-660,726-745`). `interrupts` and `publish-interrupted` inject stale lookup events and stale OKs in the window between revocation and dispatch.
  - **Lookup.** It is generation-bound at both `lookup_return` and `_finish` (`gh-inbox-lookup.c:136-155,561-578`).
  - **Inbox.** It bumps a session serial on teardown and discards unwrap results from older sessions (`gh-dm-inbox.c:323-338`). Scope updates are dropped unless the generation is current (`:392-396`).
- **No signer prompt for already-seen wraps**
  - **Check order.** `handle_wrap` checks `gh_conversation_store_has_wrap` (the seen-set delegate) and the in-flight id set before queueing (`gh-dm-inbox.c:361-384`).
  - **Admission.** Admission records both keys through one delegate call, and a duplicate still records its wrap (`gh-conversation-store.c:148-164`).
  - **Tests.** `seen-restart` and `rooms-and-dedup` assert exact signer-call counts, for example "three unwraps, not four" (`tests/app/test_dm_inbox.c:508-540,591-616`).
  - **Concurrency.** The number of concurrent signer calls is bounded (1–4).
- **Seal expiration (`687dbd13`)**
  - **Values.** `parse_expiration` (`gh-nip17-inbox.c:123-137`) accepts only canonical decimal in [1, 253402300799]: digits only, no sign and no leading zero. The bound is checked before each multiply, so it cannot overflow.
  - **Tags.** `expiration_tag` (`:139-164`) requires exactly two elements and at most one expiration per layer. On the seal it admits nothing else, and that check runs before the second signer call (`:255-262`).
  - **Precedence.** `expires_at` takes the rumor value, else the seal's, else the wrap's, as in charter §3.7.
- **Self-DM envelope.** `build_self_async` starts at the self destination, so it makes one encrypt and one sign and produces exactly one wrap (`gh-nip17-envelope.c:245-246,265-287`). The outer-key-reuse check is skipped correctly when there is no recipient wrap.
- **Test realism.**
  - The mock `org.nostr.Signer` performs real NIP-44 and Schnorr operations on a private bus (`tests/app/gh-test-signer.h`).
  - The recording transports assert exact URL sets and open/close order.
  - `wire-relay.h` is now a real libsoup NIP-42 relay that verifies the AUTH event, and it no longer leaves handlers pointing at a finished test's stack.
  - The waits are signal-driven with failure-only deadlines.
- **Versioning.** The manifest matches both build systems. Both bumps are correct PATCH bumps:

  | Component | `VERSION_MANIFEST.md` | Build sources | Bump |
  | --- | --- | --- | --- |
  | nostr-gobject | 2.0.1 | `nostr-gobject/CMakeLists.txt:18` and `nostr-gobject/meson.build:2` | PATCH: behaviour now matches the documented contract; no API change |
  | libmarmot | 0.3.6 | `libmarmot/CMakeLists.txt:16-18` and `libmarmot/meson.build:2` | PATCH: security fix; no API, ABI or wire change; rejects only spec-invalid Commits |

### Non-blocking (follow-up suggested)

1. **Groundhog is not bumped (still 0.5.3; `gnome/groundhog/CMakeLists.txt:2`, `VERSION_MANIFEST.md:21`).**
   - **Why a bump is needed.** The shipped executable changes:
     - The account-relays scope now sees real mid-session DISCONNECTED and CONNECTED transitions (nostr-gobject fix).
     - The relay layer carries AUTH and new write-failure paths.
     - `gh-nip17-inbox.c` tag rules change.
     - The send sources are compiled in.
   - **Recommendation.** Bump to **0.5.4 (PATCH)** in the manifest and the CMake project in one commit. There is no public API, CLI, config or on-disk format in the shipped app. The inbox's `.checkpoint` file is written only by the unlinked `groundhog-dm-inbox`.
2. **An inbox backfill larger than one page can be lost for good (`gh-dm-inbox.c:207-225,447`).**
   - **Cause.** The REQ carries `limit: 1000` and is never paged with `until` (charter §4.5 S6). The checkpoint still advances once every relay has sent EOSE.
   - **Effect.** If more than 1000 wraps fall in the 49 h plus backlog window (for example spam, or a long offline period), the relays return only the newest by `created_at`. Wrap `created_at` values are randomized across two days, so the omitted ones include legitimate recent messages. Advancing the checkpoint then drops them from every later REQ.
   - **Suggested fix.** Hold the checkpoint when any relay returns `limit` events, or page with `until` before advancing.
   - **Impact today.** None, because the pipeline is not linked into the app.
3. **Wraps rejected after a signer call are never recorded, so they are re-decrypted every session (`gh-dm-inbox.c:344-345`).**
   - **Cause.** A seal or rumor that fails validation is only counted.
   - **Effect.** Each restart or relay-set change re-fetches it within the 49 h window and spends one or two signer calls, each possibly a visible approval, on the same known-bad wrap. This does not violate the "already-seen" rule, which is about admitted wraps, but a sender can pin prompts with a handful of wraps.
   - **Suggested fix.** Record the wrap id as seen, or add a rejected namespace in G05's `seen` table, once the rejection is final. That covers every `GH_NIP17_INBOX_ERROR`.
4. **Expired-on-arrival messages are admitted and shown (`gh-dm-inbox.c:289-298`).** `687dbd13` exposes `expires_at`, but `admit()` ignores it, and `GhMessage` does not carry it. Charter §3.7 and EX-4 say such a message is recorded in `seen` only. This belongs to G07. Please confirm that bead owns the check at admission, not only the purge.
5. **Lookup sources are wider than charter §4.3 (`gh-inbox-lookup.c:421-425`, `gh-inbox-lookup.h:14-17`).**
   - **Deviation.** The recipient-10050 REQ also goes to the account's own NIP-65 read and write relays. §4.3 and PD-12 name `discovery-relays` plus the contact's own 10002 write relays. Your own relays thus learn whom you are about to message, just before the publish.
   - **Mitigation.** It is unauthenticated, so this is an IP and timing link only. P1 is satisfied, because these are signed-list destinations.
   - **Other gaps.** Lookups are also send-time, with a 5 min cache, not the §4.5 S2 24 h directory, and self-copy D8 jitter is not implemented. Both are acknowledged as later work (G06 and the contact directory).
   - **Action.** Either amend the charter or drop own read/write relays as sources.
6. **AUTH is built but not yet used by any Groundhog pipeline (`gh-dm-inbox.c:502-513`, `gh-dm-send.c:575-590`, `gh-inbox-lookup.c:432-440`).**
   - **Effect on the inbox.** The inbox never sets ACCOUNT on the account's own 10050 relays, as §4.3 allows. A relay that AUTH-gates kind-1059 reads, which D4 prefers, therefore closes the REQ `auth-required:` and the endpoint stays FAILED (`:415-417`).
   - **Effect on send.** Recipient relays are left at NONE rather than EPHEMERAL, so a relay that requires AUTH for writes yields AUTH_REQUIRED. That is conservative and charter-safe.
   - **Follow-up.** Track the wiring. When it lands, also cover:
     - R6 (at most one account-AUTH signer prompt per relay per session). The current policy is one per challenge, so each reconnect prompts again.
     - The 30 s publish deadline (`gh-relay-publish.c:485-499`), which also bounds a pending account-AUTH approval.
7. **The scope's mid-session reconnect path is now live but untested at the wire level (`gh-relay-gnostr.c:205-235`).**
   - **What changed.** Before the nostr-gobject fix, a dropped scope socket was never announced, so this `on_state` branch was effectively dead in the shipped account-relays scope. It now drives `reset_subscription`, DISCONNECTED and `schedule_retry`.
   - **Interaction.** Scope relays keep libnostr's core auto-reconnect enabled; only publish disables it. `retry_connect` either reuses an already re-established core or replaces the wrapper.
   - **Coverage.** `relay-wire` covers `offline-at-start` but not a drop after EOSE.
   - **Suggested test.** Stop the wire relay mid-subscription and assert, with no sleeps, one DISCONNECTED, one fresh REQ with EOSE, and no duplicate subscriptions.
8. **Narrow races in the nostr-gobject barrier (`nostr_relay.c:245-258,884-891`).**
   - **Store/serial race.** The worker stores `state` before it increments `state_serial`. A `gnostr_relay_disconnect` between the two gives that transition a serial above the barrier, and it is then emitted after DISCONNECTED. This needs a core connect racing an explicit disconnect.
   - **Owning-context race.** Groundhog's publish transport calls `gnostr_relay_disconnect` on its owning context, which may not be the default context (`gh-relay-publish-gnostr.c:301-309`). There, `emitted_state` is written off the main thread while queued idles may run on it.
   - **Impact today.** Both are benign for Groundhog's private relays, whose handlers are disconnected first.
   - **Suggested fix.** Take the serial before the `state` store, or do both under a lock. Document that the barrier holds only when disconnect runs on the default context.

## Verification

- `git submodule update --init third_party/nsync third_party/nostrdb`, then an out-of-tree `cmake -S . -B /tmp/gh-w12-review -G Ninja -DBUILD_GROUNDHOG=ON -DBUILD_APPS=OFF -DBUILD_NOSTR_GTK=OFF && cmake --build /tmp/gh-w12-review -j6`: configure and build succeeded (1129/1129 targets). There were no new warnings in Groundhog, `nostr_relay.c` or `mls_group.c`; Groundhog targets build with `-Werror`.
- `ctest --test-dir /tmp/gh-w12-review -R 'groundhog-|marmot|gobject|relay' --output-on-failure`: **64/64 passed**, with 1 platform skip (`groundhog-launch`). This includes:
  - libmarmot: `marmot_test_mls_group` and 20 other `marmot_*` tests, and `marmot_gobject_test`;
  - nostr-gobject: `test_nostr_gobject_relay_connect` and 9 other `test_nostr_gobject_*` tests;
  - Groundhog relay: `groundhog-relay`, `-relay-publish`, `-relay-wire`, `-relay-publish-wire`;
  - Groundhog pipelines: `groundhog-conversations`, `-dm-inbox`, `-dm-send`, `-account`, `-account-relays`.

  The integrator's 44/44 is a subset of these.
- macOS `leaks --atExit` reported **0 leaks** for `test_mls_group`, `test-groundhog-relay`, `test-groundhog-relay-publish` and `test-groundhog-conversations`. The D-Bus-backed `dm-inbox` and `dm-send` binaries were not leak-checked.
- `git diff --check 1d58102e..d12fcc89` passed.

**APPROVED**: scoped to integrated commit `d12fcc89bab3d6027f0d89064fecd837ba96dfe6` versus `1d58102e4c2e2ce523ac6b9e8f5923ccde80fab6`. There is no blocking finding. The recommended follow-ups are the Groundhog 0.5.4 PATCH bump (item 1) and the backfill-paging fix before the inbox is linked into the app (item 2). The other non-blocking items are also suggested follow-ups.
