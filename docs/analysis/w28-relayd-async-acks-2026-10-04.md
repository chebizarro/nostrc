# W28 B: asynchronous relayd commit acknowledgements

Bead: `nostrc-hja5`. Fixes review finding W28-R2-1 against `806fecc2`.

## Completion path

The direct nostrdb backend now exposes an optional, in-tree asynchronous storage
interface. EVENT handling validates and reserves the replay ID, enqueues the
write, and returns to libwebsockets without waiting for a commit. Each connection
owns a bounded FIFO of pending acknowledgements (256 entries), including the
canonical event ID, replay reservation and monotonic deadline.

A catch-all nostrdb subscription receives post-commit notifications. Its writer
thread callback only wakes libwebsockets with `lws_cancel_service`; it never
queries the database, unsubscribes, or writes a WebSocket frame under nostrdb's
monitor lock. On the service thread, the relay drains that subscription and
checks fresh read transactions for pending IDs. Visible events become successful
ACKs, emitted from writable callbacks in FIFO order. Pending ACKs do not prevent
REQ/EOSE handling, including on their own connection.

An LWS timer resolves an unconfirmed write as
`OK false, "error: storage commit unconfirmed"` after the monotonic deadline
(default ten seconds). Visibility is checked again at expiry. This is a commit
failure deadline, not a timer used to infer successful persistence. Disconnect
cleanup settles replay reservations and frees the queue. Shutdown unregisters
and quiesces storage notifications before destroying the LWS context.

The ordinary synchronous storage vtable remains available to other callers.
The optional session-relay federation tee has not acquired this async capability;
its remaining synchronous path is tracked separately in `nostrc-7rdm`.

## Regression evidence

Existing test binaries were extended; no new CI registration is needed.

- `relayd_req_contract` holds client A's fake storage commit using an explicit
  condition/notification fixture. Client B must receive EOSE in under 500 ms;
  same-client REQ also remains serviceable. Restored implementation: B took
  **2 ms** while A was stalled, and **5 ms** during the no-notification case.
  With a two-second test deadline, the latter returned `OK false` at **2,000 ms**.
- Temporarily routing EVENT back through synchronous `put_event` made the test
  fail: B waited **3,001 ms** with no EOSE, and the same-client REQ was blocked.
  The mutation was removed, rebuilt, and the test passed. Logs:
  `/tmp/w28b-red-test.log`, `/tmp/w28b-green-test.log`.
- The fake backend accepts a 100-event burst before releasing any commits and
  requires all ACK IDs in order. It queued in 3 ms and acknowledged in 4 ms.
- `relayd_session_relay_storage` uses real nostrdb: both a single EVENT and a
  100-event burst are followed immediately by REQ after OK, with no sleeps or
  query retries. All ordered burst ACKs and immediate readbacks passed.
- `groundhog-relayd-publish`: **20/20** serial repetitions passed, in addition
  to the **7/7** focused relayd/Groundhog tests.

## Throughput observations

The existing real-storage test pre-signs 100 distinct events, sends the whole
burst, then measures time to receive all ordered ACKs (signing is excluded).
Three resumed-session async runs took **86, 94, and 60 ms**, approximately
**1,064–1,667 acknowledged events/s**. Log: `/tmp/w28b-async-benchmark.log`.

The recovered prior-session queue-only control logs measured **4, 3, and 3 ms**
for the same test's 100-event burst (`/tmp/w28-queue-bench-{1,2,3}.log`), but those
runs failed immediate-read assertions: their ACKs did not wait for persistence.
The independent review's separate queue-only probe measured 5.9–9.1 ms versus
123–183 ms for the synchronous commit-wait implementation.

These are directional local observations, not a controlled throughput benchmark:
the shared host was running other gates, and enqueue-only ACKs and committed ACKs
measure different work. The async path removes per-EVENT service-loop waits and
allows nostrdb batching; it does not promise the old premature-ACK completion
rate. The deterministic independent-client test establishes responsiveness even
when commits never arrive.

## Build and gate results

- macOS CMake/Ninja full build: passed after sourcing
  `/tmp/nostrc-macos27-env.sh`.
- Full macOS CTest: 472 registered, five expected skips. The sole initial
  failure was the existing `groundhog-about-gui` skipped-frame SIGTRAP
  (`nostrc-i93e`); it passed when rerun serially. Logs:
  `/tmp/w28b-resume-full-ctest.log`, `/tmp/w28b-serial-rerun.log`.
  The subsequently strengthened ACK test was rebuilt and passed in the final
  focused run and restored-fix run.
- `scripts/check-unsequenced-args.py` and `git diff --check`: passed.
- `scripts/linux-gate.sh <worktree>`: all default targets built; **462** smoke
  tests passed after the gate's serial rerun of the load-sensitive
  `groundhog-two-instance` startup timeout (`nostrc-1sxx`). Log:
  `/tmp/w28b-linux-gate.log`.
- `scripts/linux-gate.sh --sanitizers <worktree>`: **60/60** passed with no
  sanitizer failure or rerun. The input-coverage check passed for all 973 build
  inputs, including the new async header. Log: `/tmp/w28b-sanitizer-gate.log`.
  This CI-derived sanitizer job disables relayd; relayd itself is covered by
  the macOS and Linux smoke builds/tests, not these sanitizer tests.

## Version and review status

No new bump: relayd and the nostrdb storage adapter have no declared component
versions. The async hook is in-tree and not an installed libnostr API. The
existing libnostr 1.1.3 PATCH remains unchanged; Groundhog changes are test-only.
`VERSION_MANIFEST.md` records the decision.

An Oracle review was attempted. The first request received no source because the
workspace export rendered selected files as zero tokens; the continuation with
code supplied directly failed with provider-at-capacity. This is **not** a peer
approval. Independent re-review is still needed before landing. No push was made.

Exploration handoff: Jev `find-lines` narrowed the nostrdb notification/enqueue,
ingress completion and gate-lock reads (presence 0.78–0.96); 41 requests,
291,131 input tokens, reported cost $0.012227. No failed calls or observed
threshold misfires; load-bearing code was read directly afterward.
