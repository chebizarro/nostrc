# Nostr Protocol Review — fipsd rendezvous module (`nostrc-8hvs6.4`)

- **Date:** 2026-11-16
- **Reviewer:** chebizarro@gmail.com (agent-assisted)
- **Branch:** `feature/fipsd-nostr-review` (worktree `fips-nostr-review`), based on `origin/master` `a0a20c7d1`
- **Skill applied:** `nostr-protocol-smells` (taxonomy A–J), per the repo **Nostr Protocol Guidelines** in `AGENTS.md`.
- **Scope reviewed:** `fipsd/src/nostr/*` (runtime, driver, failure_state, signal_gate,
  signal_route, traversal, traversal_io, traversal_machine, stun, events, advert, inbox);
  the libnostr pieces this module relies on that changed for it
  (`libnostr/src/simplepool.c` CLOSED backoff, `libnostr/src/relay.c`
  `nostr_relay_auth`/`nostr_relay_auth_wait`); and the event-driven tests
  (`fipsd/tests/test_nostr_runtime.c`, `test_nostr_traversal.c`, `nostr_relay_fixture.c`).
- **Cross-check baseline:** upstream `/Users/bizarro/Documents/Dev/fips/src/nostr/runtime.rs`
  (and `signal.rs`, `signal_route.rs`, `types.rs`) read-only, for filters, kinds 37195/21059,
  refetch outcomes, admit policy and freshness/skew.

## Verdict

**No must-fix protocol smell was found.** The rendezvous module is event-driven end to end:
one long-lived `REQ` with tight relay-side filters, an explicit stored→EOSE→live boundary,
`publish_and_wait` on every publish (OK/`OK false`/missing-OK all handled), reacted-to
`CLOSED`/`AUTH`, reconnect with a bounded exponential ladder, `event.id`/replaceable-event
dedup that is idempotent on replay and across relays, and event-driven tests with no
sleeps. The A–J checklist in `fipsd/README.md` holds and has been updated to record this
formal review. Four findings are recorded below. None is an A–J protocol smell that this module introduces,
and no `fipsd/src/nostr/*` code change was required (none was invented). The most significant is a
pre-existing **libnostr** concurrency defect — a flaky ThreadSanitizer data race in the relay
connect→write-worker handoff (`relay.c:967` vs `:1064`), outside the CLOSED-backoff and AUTH code
this review covers, which the rendezvous connect/retry churn exposes under load. Because of it the
TSan gate is **not reliably green** on this subset (ASan/UBSan is green); it is dispositioned
`follow-up`. Evidence and the exact behaviours observed follow.

## Method

1. **Surface map.** Searched the scope for `sleep|timeout|interval|poll|retry|reconnect|
   REQ|EVENT|EOSE|CLOSED|AUTH|NOTICE|challenge|since|until|limit|subscribe|publish|NIP-11|
   NIP-42|backoff|dedup|queue|ticker|hardcoded relay`, repeated per path, producing the
   coverage checklist below.
2. **Read the relay-client abstractions first** (`runtime.c` actor, `simplepool.c` pool,
   `relay.c` frame dispatch), then publishers, subscribers, the signal router, the traversal
   worker, and finally the tests.
3. **Verified every candidate against source lines** and against the upstream Rust behaviour
   it must match. Reconnect-ownership, refcount balance in relay replacement, and the
   EOSE/CLOSED/AUTH state machine were traced through both the actor and libnostr.
4. **Confirmed the reviewed state builds and tests green** out-of-tree under ASan/UBSan and
   TSan with warnings-as-errors (see "Verification").

## Inventory — every subscription / publish / frame-handling site

### Subscriptions (`REQ`)

| Site | File:line | Filter(s) |
| --- | --- | --- |
| Persistent live REQ | `runtime.c:255` `subscribe_main` → `subscribe_async` `runtime.c:273` | advert: kind **37195**, `#d=fips-overlay-v1`, authors (configured) or `#d`-scoped wildcard (open), `since=now-advert_ttl`; signal: kind **21059**, `#p=<self>`, `since=now`, `limit=0` |
| Filter builder | `runtime.c:234` `advert_filter` | kind/`#d`/authors/since; `until=now`+`limit=1` only on refetch |
| One-shot refetch REQ | `runtime.c:483` `start_query` → `subscribe_async` `runtime.c:511` | advert: `author=<peer>`, kind 37195, `#d`, `until=now`, `limit=1`, `auto_unsub_on_eose=true` |

### Publishes (`EVENT`)

| Site | File:line | Kind / purpose |
| --- | --- | --- |
| Publish primitive | `runtime.c:279` `published` → `nostr_relay_publish_and_wait(…,1000)` | checks OK; `publish_result` reported; negative/missing OK = failure |
| Local advert + inbox | `runtime.c:288` `publish_local` | kind **37195** advert → advert relays; kind **10050** DM-relay list → signal relays; gated on `s->live` (post-EOSE) |
| Traversal signal | `runtime.c:518` `send_signal` | kind **21059** offer/answer → selected signal relays |
| NIP-42 AUTH | `runtime.c:312` `authenticate` → `nostr_relay_auth_wait(…,5000)` | kind **22242**; event-id OK checked and the AUTH event freed by the helper |

### Frame handling (pool worker callback → actor mailbox → actor)

| Frame | Callback (worker thread) | Actor handler |
| --- | --- | --- |
| EVENT | `middleware` `runtime.c:119` (kind 21059 on main source → signal router; else EVENT msg w/ live-since) | `process`→`cache` `runtime.c:575` |
| EOSE | `subscription` `runtime.c:131` | `process` `runtime.c:576`: `s->live=true`, reset backoff, `catchup`, boundary, `publish_local` |
| CLOSED | `subscription` `runtime.c:131` | `process` `runtime.c:583`: log reason, boundary(false), `s->live=false` (pool keeps socket + re-REQ backoff) |
| AUTH | `auth_challenge` `runtime.c:142` → `commands` | `process` `runtime.c:596` → `authenticate` |
| DISCONNECTED | `connection_state` `runtime.c:148` → `commands` | `process` `runtime.c:590` → `retry` |
| NOTICE | logged by `libnostr` `relay.c:321` (stderr) | not surfaced to driver (see Finding 2) |
| signal admission | `fipsd_signal_router_route` `signal_route.c:31` | target/expiry/dedup/gate/queue, consumes event on all paths |

### Capability probing, reconnect, dedup, freshness

| Concern | File:line |
| --- | --- |
| NIP-11 fetch (per relay, cancellable, 3 s connect / 10 s total) | `runtime.c:638` `nostr_nip11_fetch_info_cancellable` |
| NIP-11 capability gate | `runtime.c:161` `fipsd_nostr_relay_capable` |
| Reconnect (terminal-close replace + re-REQ, 1 s→5 min ladder) | `runtime.c:335` `retry`; CLOSED backoff `simplepool.c:474`, armed `runtime.c:631` |
| Advert dedup/idempotency (author, created_at, lowest-id; `delivered` guard) | `runtime.c:192` `cache` |
| Signal replay memory (bounded, expiry, insert-after-admit) | `signal_route.c:40-54` |
| Freshness/skew (SKEW = 60 s) | `traversal.c:101` `freshness`; skew estimate `runtime.c:421` |

## Findings (ranked by severity)

| # | Severity | Category | Summary | Disposition |
| --- | --- | --- | --- | --- |
| 1 | Medium | J / concurrency (libnostr, pre-existing) | Flaky TSan data race in libnostr's relay connect→write-worker handoff (`relay.c:967` spawns `write_operations`; `:1064` is its first `nostr_connection_write_message`), exposed by the rendezvous connect/retry churn under load; the TSan gate is not reliably green. Not in the CLOSED-backoff/AUTH code in scope. | **follow-up** |
| 2 | Low | H — capability/reconnect | NIP-11 is probed once at startup with no re-probe; a *transient* fetch failure permanently excludes an otherwise-usable relay, and if every configured relay's probe fails at boot the rendezvous subsystem exits silently for the process lifetime. | **fixed** (`nostrc-tukt2`, 2026-11-18; see below) |
| 3 | Info | C — relay frames | Relay `NOTICE` frames are logged by libnostr (`relay.c:321`) but not surfaced to the driver/metrics; libnostr exposes no per-relay `NOTICE` callback. | **not-a-smell** |
| 4 | Info | A/B — divergence | A failed offer *re-send* (repair) while still in `AwaitAnswer` fails the attempt into cooldown, where upstream keeps the in-flight offer and retries the re-send. | **not-a-smell** |

### Finding 1 — flaky TSan data race in libnostr's relay connect→write handoff (Medium, concurrency; libnostr, pre-existing)

Under ThreadSanitizer, `ctest -R '^FipsdNostr'` intermittently aborts two tests
(`FipsdNostrRuntimeTest`, `FipsdNostrTraversal_guards`) with:

```
SUMMARY: ThreadSanitizer: data race relay.c:1064 in write_operations
    #0 ... write_operations             libnostr/src/relay.c:1064   (nostr_connection_write_message(conn, r->priv->connection_context, req->msg, &werr))
  previous access:
    #3 nostr_relay_connect_with_context libnostr/src/relay.c:967    (go_fiber_compat(write_operations, write_arg))
    #4 connect_relay                    fipsd/src/nostr/runtime.c
    #5 run                              fipsd/src/nostr/runtime.c
```

The just-spawned `write_operations` fiber's first websocket write races the connect path
(`nostr_relay_connect_with_context`, reached from the rendezvous actor's `connect_relay`/`retry`)
at the point that same call spawns the fiber and finalizes the connection. The racing object is in
libnostr's relay/connection startup state, not fipsd's.

- **Frequency:** timing/load-sensitive. 0 hits in 60 idle single-process runs; ~2 hits across ~24
  `ctest` invocations, reproducing most readily under CPU contention (e.g. immediately after a
  parallel build). A single prior `ctest -R '^Fips'` TSan pass (as the README reports) can thus
  show "0 failed" while still masking it. ASan/UBSan is unaffected (7/7 green, repeated runs).
- **Why the rendezvous module exposes it:** the actor connects every configured relay at startup
  and, on CLOSED(auth)/DISCONNECT, runs `retry()` → terminal-close + fresh relay + reconnect, plus
  `subscribe_main()` stop/start — far more connect→write-worker handoff churn than a connect-once
  client, widening the startup window TSan flags. Precedent: `nostrc-8hvs6.3` fixed a thread-safe
  relay-debug-init race "exposed by concurrent traversal tests" with `pthread_once`.
- **Nature:** either a genuine missing happens-before on the connect→fiber handoff (connection /
  `r->priv->connection_context` published without a release/acquire edge the fiber observes) or an
  incomplete `go_fiber_compat` fiber annotation for that spawn edge (`nostrc-9965o`). Distinguishing
  the two needs libnostr/go_fiber instrumentation.
- **Why not fixed here:** the defect is in libnostr's **core** relay connect/write machinery
  (`relay.c` `nostr_relay_connect_with_context` / `write_operations`), shared by every nostrc pool
  consumer. It is outside the two libnostr pieces this review covers (CLOSED backoff, AUTH), and a
  correct fix must be validated against the **full libnostr TSan suite**, not the fipsd nostr
  subset — landing it inside this rendezvous review would change shared code under a too-narrow
  gate. Per `AGENTS.md` it is filed as a separate libnostr concurrency bug, not a prerequisite of
  this review.
- **Follow-up (precise):** build a deterministic repro by driving `nostr_relay_connect_with_context`
  + `write_operations` in a tight connect/retry loop under TSan with CPU contention; confirm whether
  `go_fiber_compat` establishes a happens-before edge from the spawning thread's pre-spawn writes to
  the fiber body (if not, annotate the spawn as the synchronization point); otherwise publish the
  connection and its context under the relay mutex with an explicit release before the spawn and an
  acquire at `write_operations` entry. Re-gate `ctest -R '^Fips'` under TSan with repeated,
  contended runs until stable. Track as a libnostr `2.4.x` concurrency fix.

### Finding 2 — NIP-11 probe is one-shot; transient failure is permanent (Low, category H)

`startup()` (`runtime.c:622-658`) fetches each relay's NIP-11 document once
(`runtime.c:638`) and sets `s->usable = fipsd_nostr_relay_capable(info, …)`
(`runtime.c:639`). `fipsd_nostr_relay_capable` (`runtime.c:161-168`) returns `false` for a
`NULL` document, i.e. a relay whose NIP-11 HTTP endpoint was unreachable, rate-limited or
timed out is treated identically to a relay that returned a definitively *incompatible*
document. There is no re-probe: the result is fixed for the life of the process. If **no**
relay is usable, `startup()` returns `false` (`runtime.c:655`), `run()` logs
`"rendezvous startup failed; no compatible relay runtime"` (`runtime.c:662`) and the actor
thread exits. The subsystem's `start()` already returned success, so nothing restarts it —
rendezvous is silently lost until the daemon is restarted.

- **Protocol mismatch:** the skill's resilience guidance (A/H) is "reconnect + re-`REQ` with
  backoff"; capability probing here has neither retry nor backoff, so a boot-time network
  blip is indistinguishable from a permanently incompatible relay.
- **Runtime consequence:** a transient NIP-11 failure drops a working relay for the process
  lifetime; simultaneous failure of all configured relays (e.g. both defaults behind one
  flaky uplink at boot) silently disables overlay rendezvous with no recovery.
- **Why not fixed here:** this is the module's **documented** fail-closed posture
  (`runtime.h:27`: "No document is unknown, not compatible: startup refuses it";
  `README.md`: "Unknown or explicitly incompatible capability documents are refused and
  logged"). For a security-sensitive overlay, refusing an unverifiable relay is deliberate.
  Making the probe retry, or distinguishing *unreachable* (retryable) from *incompatible*
  (refuse), changes that posture and startup-failure semantics, and needs owner/security
  sign-off plus upstream alignment (upstream `nostr-sdk` does not gate connection on NIP-11
  at all). The conservative behaviour is intentionally retained.
- **Follow-up (precise):** add a bounded re-probe of relays excluded by a *NULL* NIP-11
  document (document successfully fetched but incompatible stays permanently refused): on the
  actor's existing channel-select deadline, re-run `nostr_nip11_fetch_info_cancellable` on an
  excluded relay with the same 1 s→5 min ladder used by `retry()`, and admit it to the pool
  if it becomes capable; and when `startup()` finds zero usable relays, keep the actor alive
  arming that re-probe rather than returning `false`. Add a fixture op that serves a NULL/503
  NIP-11 on one relay, then flips it to the compatible document, and assert the relay is
  admitted and reaches EOSE — no sleeps, using the existing main-context command/barrier
  pattern.
- **Resolution (2026-11-18, `nostrc-tukt2`):** implemented as the follow-up above. A
  fetched incompatible document still refuses the relay for the run; a failed fetch marks
  the relay *probing* (never admitted unverified, not a pool member) and arms its `retry_at`
  on the same 1 s → 5 min ladder as `retry()`, from the actor's existing select deadline.
  When a compatible document arrives the relay is added while the pool worker is joined,
  connected and resubscribed, without a restart; the failure and the recovery are each
  logged once. `startup()` fails only when every relay is refused, and a refetch treats a
  probing advert relay as offline. Fixture op: 503 for the next N NIP-11 fetches, or a
  document without NIP-40; `FipsdNostrRuntimeTest` asserts both outcomes.

### Finding 3 — relay NOTICE frames not surfaced to the daemon (Info, category C)

`NOTICE` is dispatched in libnostr at `relay.c:319-325`: it calls `r->priv->notice_handler`
if one is set and always logs `"[RELAY_NOTICE] relay=… message=…"` to stderr. fipsd does not
set a notice handler, and libnostr exposes **no public per-relay `NOTICE` callback**
(`nostr-relay.h` has OK/AUTH/state setters and a raw-frame `set_custom_handler`, but no
NOTICE setter).

- **Assessment — not a smell.** `NOTICE` is a human-readable, advisory frame in NIP-01; it
  carries no subscription/publish state a client must act on (unlike `OK`/`CLOSED`/`AUTH`,
  which are all handled). It is already visible in the transport log. The skill's category C
  concerns *acting* on protocol-control frames and treating a websocket send as a publish
  success — neither is violated here.
- **Optional follow-up (not required):** if operators want relay `NOTICE` text in the
  daemon's own structured log/metrics rather than only libnostr's stderr, add a public
  `nostr_relay_set_notice_callback` to libnostr and forward it through the existing
  `driver.boundary`/log path. This is a libnostr public-API addition, out of scope for this
  review.

### Finding 4 — failed offer re-send in AwaitAnswer is fatal vs upstream retaining it (Info, divergence)

For a `RESEND` command, `send_signal` (`runtime.c:518-554`) looks up the existing outbound
session and, if no relay accepts the re-published offer, frees and zeroes that session
(`runtime.c:552`), reporting `signal_result(…, repair=true)`. The traversal machine's
`REJECTED` guard (`traversal_machine.c:47`) treats a `repair` answer as a rejection while in
`AwaitAnswer`, so a failed repair there drives the attempt to `Cooldown`/`Failed`. Upstream
`resend_inflight_offer` (`runtime.rs:1564`) instead marks the re-send `Available` and keeps
the in-flight offer, continuing to await the original offer's answer until the signal TTL.

- **Assessment — not a smell / defensible divergence.** The two C components are mutually
  consistent (runtime clears the tracker, machine fails the attempt), the behaviour is
  captured in the README transition table, and it is reachable only in the dual-initiator
  suppression path when the repair publish is rejected by *all* signal relays. Failing fast
  to `Cooldown` (then a fresh retry) is a reasonable alternative to waiting out the 120 s
  signal TTL for an answer the peer may never send, because the suppressed responder means
  neither side completes without our offer. Changing it to the upstream "retain and retry"
  shape would require altering both the `REJECTED` guard and the runtime clearing, with
  unclear benefit and real risk to the passing dual-init regression. Recorded for awareness;
  no change made.

## NIP-11 probing — confirmation

- **When:** once per configured relay during `startup()` (`runtime.c:633-647`), before the
  relay is created/connected.
- **How:** `nostr_nip11_fetch_info_cancellable(info_url, canceled, r)` over HTTP(S) derived
  from the `ws(s)://` URL, cancellable against the runtime context; curl connect-timeout
  3000 ms, total 10000 ms (`nips/nip11/src/nip11.c:261-262`). The outcome is logged
  (`runtime.c:640`: `nip1/nip40/nip42/auth/capable`).
- **What refuses a relay** (`fipsd_nostr_relay_capable`, `runtime.c:161-168`): a missing or
  unreachable document; absence of NIP-01 or NIP-40; `auth_required` without NIP-42;
  `payment_required`; non-zero `min_pow_difficulty`; a declared `max_filters` < 2 or
  `max_subscriptions` < 2. NIP-59 is **not** required of relays (client-side crypto), and
  kind 21059 is NIP-01 ephemeral — the `signals` parameter is intentionally unused so advert
  and signal relays meet the same bar. A refused relay emits
  `boundary(url, false, "NIP-11 incompatible")`; a failed fetch emits
  `boundary(url, false, "NIP-11 unavailable; retrying")` and is re-probed (`nostrc-tukt2`).
- **Multiple relays / failover:** the advert/signal relay union is probed and each capable
  relay is added to the pool; the pool fans the REQ across all of them, dedups client-side,
  and publishes to each role's set. (Re-probe on transient failure was Finding 2, now fixed.)

## EOSE / CLOSED / AUTH / reconnect state machine as observed

Threading: the pool worker thread runs the frame callbacks; each callback hands an owned
message to the single-owner actor thread over `incoming` (cap 4096; EVENT/EOSE/CLOSED) or
`commands` (cap 64; AUTH/DISCONNECTED/user commands). A full mailbox cancels the runtime
rather than fabricate progress (`runtime.c:104-113`). A generation fence
(`runtime.c:573`) drops stale EOSE/AUTH/DISCONNECTED from a replaced connection.

```
connect → (worker) frames → actor:

  EVENT (pre-EOSE, live_since==0) ─► cache(), NOT dialed            [stored history]
  EVENT (live, live_since!=0)     ─► cache() + dial immediately     [live]
  EOSE                            ─► s.live=true; backoff:=1s;
                                     catchup() dials cached-undelivered;
                                     boundary(live=true); publish_local  [catch-up → live boundary]
  CLOSED <reason>                 ─► log; boundary(live=false); s.live=false;
                                     pool keeps the socket+auth and schedules a
                                     re-REQ on a per-relay 1s→5min ladder; EOSE resets it
  AUTH <challenge>                ─► authenticate(): generation-fenced;
                                     nostr_relay_auth_wait signs with the FIPS identity,
                                     waits for the event-id OK, frees the AUTH event;
                                     publish_result(kind 22242, ok)
  DISCONNECTED                    ─► if usable & not already retrying:
                                     boundary("connection lost"); retry()
  retry()                         ─► finish any in-flight refetch; pool.stop();
                                     detach old relay callbacks; remove+unref old relay
                                     (refcount-balanced, no UAF); create fresh relay;
                                     add to pool; arm retry_at = now+backoff (×2, cap 5min);
                                     re-subscribe. auto_reconnect=false so the pool's
                                     redial worker cannot race the actor (dial_try_claim
                                     requires auto_reconnect) — the actor is the sole
                                     retry owner, as documented.
```

Refetch (B6) is a separate one-shot pool on the connected advert relays
(`author`/kind/`#d`, `until=now`, `limit=1`, `auto_unsub_on_eose`): completion is **all
expected EOSE** (`runtime.c:566-568`); a 10 s deadline (`runtime.c:501`) only forces a
`Skipped` *failure* outcome and never signals success. Outcomes match upstream
`refetch_advert_for_stale_check` (effective-timestamp comparison: empty→Evicted,
foreign/invalid/cache-full→Skipped, `<=cached`→SameAdvert, newer admitted→Refreshed); only
Evicted/Refreshed reset the failure streak.

## Good patterns (A–J verified)

- **A (no polling):** no timer queries relays; the actor and traversal worker are
  `go_select`/`go_select_timeout` mailboxes whose deadlines are only backoff, expiry, STUN
  RTO, the publish ticker and failure deadlines. The no-deadline path uses `go_select`, not
  `go_select_timeout(UINT64_MAX)`.
- **B (no timeout-close):** history and refetch completion are EOSE-driven; the only close is
  intentional (`retry`/shutdown `nostr_relay_request_close`).
- **C (frames):** every publish is `publish_and_wait` (OK/`OK false`/missing OK handled);
  CLOSED reason retained and surfaced; NIP-42 challenge/response with event-id OK.
- **D (EOSE backfill):** explicit stored(cache)→EOSE(catchup)→live(immediate) boundary;
  bounded `since` window; refetch `until`/`limit`.
- **E (tight filters):** kind + `#d` + authors (+ since) for adverts; kind + `#p` + since +
  `limit=0` for signals; wildcard authors only for explicitly-open discovery, still `#d`- and
  kind-scoped.
- **F (dedup/idempotency):** replaceable `(author, created_at, lowest-id)` advert cache with a
  `delivered` guard; bounded `event.id` signal replay memory inserted only after queue
  admission. Idempotent across two relays and across reconnect — asserted by the test
  (`dials` stays 1 then 2 across duplicate history, re-REQ, disconnect/reconnect).
- **G (no queue wrappers):** GoChannels are the actor/worker mailboxes, not a relay-bypass
  queue; REQ/EVENT semantics are preserved via the pool; UDP classifiers copy into a bounded
  mailbox and never block the receive drain.
- **H (capability/failover):** every relay gets a cancellable NIP-11 probe; explicit refusal;
  multi-relay union; NIP-42 retry. (Re-probe gap = Finding 1.)
- **I (timers):** all operational (backoff, reservation/fetch deadlines, STUN RTO, punch
  cadence/settle, failure cooldown, advert ticker — interruptible on stop). None substitutes
  for event delivery.
- **J (tests):** real libsoup WebSocket/HTTP fixture adapted from Grotto's `bunker_test_relay`
  (Jansson, no JSON-GLib), driven by main-context commands and channel barriers; **no
  sleeps**. It implements OK/EOSE/CLOSED/AUTH and a NIP-11 document, holds EOSE, replays
  duplicate history/live across two relays, rejects publishes, and exercises
  B6/session/freshness/failure and the two-node STUN→offer/answer→punch→IK path. `wait_notice`
  / `receive` use a failure-timeout, not a completion sleep.

## Banned-relay confirmation

`relay.damus.io` appears **nowhere** in the reviewed module, configs or tests. A full worktree
sweep (`git grep -i damus`, excluding `third_party/*` and `docs/nips/*`) found only:
historical `.beads/issues.jsonl`/`sync_base.jsonl` text and the purge issue itself;
`.gitmodules` (the `damus-io/nostrdb` submodule org — not the relay); a pre-existing compiled
gschema binary in the unrelated `apps/gnostr`; and the ban text in `AGENTS.md`. fipsd's default
relays are `wss://nos.lol` and `wss://offchain.pub` (`fipsd/src/config/node.c:77-78`); test
fixtures use loopback `ws://127.0.0.1:<port>/` and `wss://example.com`. No public relay is
hardcoded in the runtime.

## Verification

Out-of-tree, warnings-as-errors (`-DGO_WARNINGS_AS_ERRORS=ON`), macOS/arm64. All three in-scope
test binaries (`fipsd_nostr_runtime_test`, `fipsd_nostr_traversal_test`, `fipsd_nostr_events_test`)
built clean (0 warnings, 0 errors) under both sanitizer configs.

- **ASan + UBSan** (`-DGO_ENABLE_ASAN=ON -DGO_ENABLE_UBSAN=ON`,
  `ASAN_OPTIONS=detect_leaks=0:strict_string_checks=1:halt_on_error=1`): **GREEN** —
  `FipsdNostrRuntimeTest`, `FipsdNostrTraversal_{codecs,states,stun,traverse,guards}` and
  `FipsdNostrEventsTest` all passed (7/7; repeated runs). LeakSanitizer is unavailable on macOS, so
  `detect_leaks=0`.
- **TSan** (`-DGO_ENABLE_TSAN=ON`, `TSAN_OPTIONS=halt_on_error=1`): **not reliably green** — the
  pure/traversal logic and `FipsdNostrEventsTest` pass, but `FipsdNostrRuntimeTest` and
  `FipsdNostrTraversal_guards` intermittently abort on the libnostr relay connect/write data race of
  Finding 1 (~2 aborts across ~24 `ctest` runs; 0 across 60 idle single-process runs). Pre-existing
  libnostr-core race, not a regression from this branch and not in the CLOSED-backoff/AUTH code in
  scope; it is the Finding 1 follow-up.
- **Static checks (all pass):** `scripts/check-unsequenced-args.py --root .` (no findings),
  `check-gui-test-gdk-frame.py` (55 sources), `check-linux-ci-packages.py` (Dockerfile covers all CI
  package sets), `git diff --check` (clean). Run directly, not via the pre-push wrapper.
- No `fipsd/src/nostr/*` code changed, so no component version bump is required (fipsd stays 0.1.0
  Unreleased; libnostr unchanged by this branch) — this is a documentation/review-only change.
- Build directories (`/tmp/fips-nostr-asan`, `/tmp/fips-nostr-tsan`) deleted after the runs; the
  `third_party/nostrdb` submodule is de-initialised at the end of the session.

## Follow-up dispositions — `nostrc-nlekg`, `nostrc-tukt2`, `nostrc-tbcl6` (2026-11-18)

Branch `fix/fipsd-rendezvous-robustness` (commits `c8093c927`, `b6ec74508`, `9009f4ad9`).
The `nostr-protocol-smells` skill was run over that diff (18 files: `nostr/{runtime,driver}`,
`transport/{watcher,tcp}`, the libsoup fixture, the three Nostr tests,
`tor_interop.py`/`advert_interop.py`, CMake test inputs and docs), every hunk read against
the taxonomy.

Result: **no must-fix smell introduced.** Verified findings:

| # | Severity | Category | Finding | Disposition |
| --- | --- | --- | --- | --- |
| F1 | Low | C | `transient_refusal()` recovers the NIP-01 reason by matching libnostr's `relay rejected event <id>: ` error envelope; libnostr exposes no structured OK reason. Matching the envelope stops an untrusted reason from spoofing a prefix; if libnostr changed it, every refusal would be classified transient (retried on the ladder, capped at 5 min), never silently dropped. | **accepted**; a structured OK-reason accessor in libnostr would remove the coupling |
| F2 | Low | I | An `auth-required:` advert refusal is retried at the relay's next ladder deadline rather than on the NIP-42 AUTH `OK` that the actor already handles. Bounded backoff, not a substitute for delivery. | **accepted**; AUTH-driven republish is an optional refinement |
| F3 | Low | B | A missing `OK` within the existing 1 s `publish_and_wait` window counts as a transient refusal and is retried; a late `OK` makes that an idempotent replaceable republish. Before this change the advert was simply reported failed. | **accepted** |
| F4 | Medium (pre-existing, outside the diff; code-level, not runtime-observed) | H / I | libnostr's SimplePool redial worker dials any registered relay that is not connected, not self-managed (`message_loop_active`) and not in BACKOFF (`simplepool.c` redial loop; `relay.c` `nostr_relay_reconnect_is_self_managed`, `nostr_relay_get_next_reconnect_ms`). The fresh relay `retry()` registers matches all three, so the pool can redial it before fipsd's `retry_at`, undercutting the "one retry owner" ladder. The new NIP-11 re-probe does not register a probing relay, so it is unaffected. | **follow-up** (needs a bead; not filed from this session) |

Not smells: `observe()` in `tor_interop.py` is control-socket (not relay) observation of a
tick-published Rust view, deadline-bounded with a ≤ `POLL` pause that new daemon output
ends early, the shape of `udp_interop.py`'s `Scenario.wait`, replacing back-to-back query
loops. An advert whose dial is declined (active link) stays undelivered and is offered again
at later EOSE catch-ups; the re-offer is idempotent (the link check declines again).

Good patterns: every advert `OK false` is observed and classified by NIP-01 prefix; one
per-relay ladder (connect retries, NIP-11 re-probes, transient advert refusals) on the
actor's existing select deadline, with no new timer; a policy verdict is logged once and
held until the endpoint set changes, across reconnect EOSE republishes; NIP-11 "fetch
failed" is not a verdict and an unverified relay is never admitted; a refetch never infers
absence while an advert relay's capabilities are unknown; listener bind *and* unbind
invalidate the advert from the transport watcher after the state store (event-driven,
race-free against `BOUND`); no traversal or dial toward a peer with an active link
(upstream's open-discovery `skip` over `self.peers`), read from the handlers' control
snapshot that is republished before every link notice. Tests use fixture counters ordered
by the main-context command queue, refetch FIFO barriers and event waits; no sleeps (one
elapsed-time lower bound proves the republish waited out the backoff).

Behaviour versus upstream: upstream's runtime has no NIP-11 gate (nostr-sdk connects
without it), so the re-probe is fipsd's own fail-closed posture made recoverable; the
active-link skip mirrors `run_open_discovery_sweep`'s `skip` set; upstream's advert publish
does not classify `OK false`, so the prefix policy is fipsd's.

Verification (macOS/arm64, Homebrew clang, `-DGO_WARNINGS_AS_ERRORS=ON`): Debug build clean;
`ctest -R FipsdNostr --repeat until-fail:10` green under ASan+UBSan and under TSan (8/8 each,
the Finding 1 libnostr race did not reproduce); full `ctest -R '^Fips' -E 'Soak|Perf'` once
per sanitizer: 212/212 passed (12 opt-in interop/privileged tests skipped). Under ASan at
`-j6` one run of `FipsdNostrTraversal_stun` failed its STUN-retransmit count assertion
(`stun_io` does not exercise any changed code); it passed in the `-j3` full run and in all
repeat runs. `FipsdAdvertTorInteropTest` (local `tor`, pinned Rust daemon
`6aa80504`) passed twice. Static checks (`check-unsequenced-args.py --root .`,
`check-gui-test-gdk-frame.py`, `check-linux-ci-packages.py`, `check-errno-after-park.py`,
`git diff --check`) pass. Version: fipsd stays 0.1.0 Unreleased (manifest line
`nostrc-nlekg, nostrc-tukt2, nostrc-tbcl6`).
