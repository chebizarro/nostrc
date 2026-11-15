# Shared rekey core

Port of `core/src/proto/rekey/` at the pin in `libfips-core/UPSTREAM.md`.
The upstream README is normative for transition, expiry-order, trigger, and
ownership rules. This directory implements the shared machine. Production IK/XK profiles live
in `fmp/rekey` and `fsp/rekey`; the bounded host binding is `node/session`.
Legacy 0.5.x models remain upstream; twelve captured compatibility traces
are replayed by `FipsDiffRekeyLegacy` (see `../../../tests/REKEY.md`).

## API and state ownership

Include `<fips/proto/rekey/mod.h>`. Names and file boundaries follow the pinned
shared module; C symbols have a `fips_rekey_` prefix. C's shared type/function
namespace requires `get_view` and `close_machine` instead of `view` and `close`.
The profile trait is the `fips_rekey_profile` vtable in `profile.h`; `hooks.h`
contains only `FIPS_TEST_HOOKS`-gated test controls (including `start_now`).

The machine owns its state. Construct with `established`, `opening_outbound`, or
`opening_inbound`; drive with `step(now, input, env, outputs)`. Schedule `Poll`
at `next_wake`, rescheduling after inputs. Do not add a polling thread, wall-clock
reads, or timeout-based subscription logic. A late poll processes stored
deadlines in order, not one resend per missed tick. `view`, `bindings`, and
`access.h` are observational. Params must come from `params_new` or the defaults
and remain immutable. All instants must be present, monotonic, and from the
same clock domain; optional absent instants have `ops == NULL`.

**The migration guide's legacy `ActivePeer` state moves are explicitly removed
at this pin.** They are not safe public mutators of the shared machine. The
mapping is:

| Former host operation | Shared API / input |
|---|---|
| `noise_session_mut` | `seal` reserves a counter/key; `Frame` authenticates and marks replay; `keys(slot)` is const inspection only |
| `replace_session` | `PeerMsg1` / `Reply` / `Final` / `PeerDone`; execute `Installed`, `Retired`, `PeerRestarted`, or `ReplaceRequested` |
| `session_start` | `started(Current)` / `view.current.started` |
| `is_rekey_dampened`, `record_peer_rekey` | internal start gate; authenticated answer/completion opens dampening; test-only `dampened` observes it |
| `pending_new_session_mut`, `set_pending_session` | authenticated `Reply` / `Final` / `PeerDone` commits a pending; `view.pending` and const `keys(Pending)` inspect |
| `cutover_to_new_session` | `Poll` for proof/delay/confirmation or authenticated `Frame`; `Installed` identifies evidence |
| `handle_peer_kbit_flip` | authenticated `Frame` / trusted `WorkerFrame`; `view.current.k` / `k()` observes |
| `drain_expired`, `complete_drain` | `next_wake`, `Poll`, `Retired`, `Unbind`, `Revoke`, `Release`; `drain_deadline()` observes |
| `abandon_rekey`, `set_rekey_state` | machine-owned transitions; `Abandoned` output; `close_machine` for host shutdown/replacement |
| `awaits_msg2`, `complete_rekey_msg2` | `view.cycle == Init`, `Reply` |
| `needs_msg1_resend`, `rekey_msg1`, `rekey_msg1_resend_count` | `next_wake`, `Poll`, byte-identical `Send`; ladder/count belong to the attempt, never host timers |

There are no unchecked public state setters. `start_now` is a test-only bypass,
not the production trigger. `trigger` is also test-only.

## Bounded C ownership

- Caller allocates the machine, Env, scratch, and outputs. No heap or OS calls.
- **Do not copy or move a live machine.** Three fixed Noise session slots hold
  current, pending, and previous. Epoch moves transfer metadata only, so session
  addresses survive cutover, demotion, and revert. `take_cleared` consumes a
  completed session; every retirement/drain/abandon/destroy uses `clear_slot`.
- Four candidates, at most two per carrier in the XK shape; 256-byte handshake
  messages; sixteen ladder rungs; 256 replay digests plus the origin; 48 output
  slots. Three expiry passes, four candidate expirations, at most three epoch
  retirements, and output deduplication bound one call below that capacity.
- `IndexLease` is linear by contract. Only `allocate` mints it. Execute every
  `Release` by consuming the lease and returning the index to its source.
  Dropping an unreleased lease increments the leak counter; it does not return
  an arbitrary wire index. `TagSource` is local, monotonic, and not a wire index.
- Start outputs at `{0}` and consume/clear them before reuse. `Delegate` owns a
  receive-key clone. Clear every output list, even on shutdown. `destroy` cannot
  reclaim clones or leases already transferred to outstanding outputs.
- `seal` returns a **bare** cipher clone and its separately reserved counter.
  Use `fips_cipher_state_seal_with_counter`, never auto-seal. Then call
  `sealed_clear` / `sealed_prompt_clear`. Reserving burns the counter even if
  the host later drops the datagram. Only `seal_prompt` may use an evidence-waiting
  initiator pending; application data always seals under current.
  A Prompt is not a guarantee of a successful seal: handle `Exhausted` without
  emitting a datagram, including for a pending epoch at its hard limit.
- `close_machine` retires and releases all held state and returns carry facts
  when established. It returns false for an opening machine even though that
  machine has been closed. `destroy` is emergency secret cleanup, not a
  replacement for returning leases. After either, step refuses, seal refuses,
  test hooks cannot resurrect state, and a new constructor is required.
- Profile/local/route/transport backing stores outlive calls and outstanding
  outputs. Transport/route values are opaque `uintptr_t` handles, not owned
  pointers; the binding owns coordinate storage, without truncating the wire's
  u16 coordinate domain. `ReplaceRequested` borrows the authenticated token;
  consume it before its owner expires. Input `Frame` is decrypted in place only
  after successful authentication; `Deliver.begin/end` index that input buffer.
- `PeerMsg1`, `PeerDone`, `WorkerFrame`, and delegation reports are trusted shell
  inputs, not raw wire entry points. Construct them only after authentication
  or worker-ownership checks. Parse hints from the authenticated header; the
  machine cannot verify a dishonest hint independently of the binding.
- Profile callbacks must clear their temporary handshake forks on every exit,
  preserve retained handshake state on a failed read, and return bounded
  outputs. The machine clears completed/candidate temporaries on every path.
  The vtable is trusted, not an untrusted plug-in ABI.

## Binding `nostrc-f12v2.2` and the differential vector

Implement the pinned `FmpProfile` / `FspProfile` constants and callbacks in their
own `fmp/rekey` and `fsp/rekey` homes. Use `fips_rekey_noise_keys` for transport.
The XK shape is: Fsp, three messages, AtFinal, OnEvidence / AtProof, four
candidates / two per carrier, dampening at Completion, trial slot selection,
silence and setup-RTT enabled, no indices. FMP uses indexed two-message AtAnswer,
OnProof / OnFrame, dampening at Answer; its node-level msg1 read produces the
trusted token. Use the pinned profile's actual AAD, plaintext floor, prompt
codec, and probe detection, not the test stub's framing.

For `tools/fips-vectors/src/rekey.rs` / `tests/vectors/rekey.json`:

1. Build production XK callbacks over the existing handshake read-on-Fork APIs
   with `SeededEntropy`; do not wrap the stub transcript.
2. Consume the two `noise.json` sessions into `established`, Params::fsp, the
   pinned identities, and setup facts (including `last_rx=0` and peer epoch).
   Reproduce the trace's jitter draw and local tag sources.
3. Dispatch Setup/Ack/Msg3/Frame/Poll to `step` using TestClock instants. Compile
   the shared sources with `FIPS_TEST_HOOKS` for the initial `start_now` input.
4. Execute outputs in order. For Prompt, immediately `seal_prompt(tag)` and
   encode the real FSP header/AEAD with the returned counter/key. Application
   traffic uses `seal`. Clear keys and output owners after execution.
5. Serialize `view`: identity, peer_epoch, current/previous/pending
   tag/k/role/started/base/recv, cycle + wait, candidates, last_rx, link_since;
   supplement with `k`, `sent`, and `next_wake`. These cover the vector's
   `initial_state`, per-event before/after, and `final_state` fields. Serialize
   the typed output enums and fields; no machine reshape is needed.

`check_rekey()` now executes the real `.2` profile over all 16 vector events,
reporting one checked trace without a SKIP. Production binding and unit/fuzz
evidence are described in `tests/REKEY.md` and `include/fips/node/session.h`.
Legacy compatibility traces are captured/replayed by `.3` in
`tests/vectors/rekey-legacy/`; unit/fuzz evidence alone is not legacy replay.
