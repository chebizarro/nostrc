# Groundhog W10 integrated peer review — 2026-09-27

## Context / scope

This is an independent review of exactly `f3a27ff90106b352f44e6e709fb063c37fc64297..78cb3930e978c23d5fe3098aaeccaa68a60dda06`:

- `71736c5d`: libmarmot standalone Meson repair (nostrc-ltx4), libmarmot 0.3.4.
- `a7238685`: live ProposalRef Commit regression and staged-clone release on every reject path (nostrc-qp24.5.1.2), libmarmot 0.3.5.
- `d4506d10`: per-relay signed-event publication outcomes (nostrc-qp24.4.4).
- `de4e3148`: GTK-free NIP-29 relay-authoritative group state and templates (nostrc-qp24.12.1).
- `b0b4a57a` + `ffe02e16`: signer-only NIP-17 inbound unwrap and restart-safe seen-set (nostrc-qp24.10.2).
- `78cb3930`: Groundhog 0.5.2 PATCH.

This review approves the code and tests only. It does not cover Groundhog release readiness, live relay or signer acceptance, or UI wiring. None of the three new Groundhog cores (NIP-17 unwrap, relay publish, NIP-29) has a production caller yet. I checked the specs against the current [NIP-17](https://github.com/nostr-protocol/nips/blob/master/17.md) and [NIP-29](https://github.com/nostr-protocol/nips/blob/master/29.md). I changed no code or beads.

## Findings

### Blocking

None.

### Reviewed areas — no blocking finding

- **NIP-17 / NIP-59 binding**
  - **Wrap.** `gh-nip17-inbox.c:164-186` bounds the wrap (128 KiB) before parsing. It requires a strict signed parse, and `nostr_event_validate` checks the lowercase-hex id against the canonical hash and verifies the Schnorr signature (`libnostr/src/event.c:612-662`). The event must be kind 1059, and it must carry exactly one `p` tag equal to the lowercase hex of the *active* account.
  - **Seal.** `:199-215` bounds the seal to the NIP-44 maximum plaintext and requires kind 13 with a valid id and signature and an empty tag list.
  - **Rumor.** `:253-296` does the following:
    - It parses the rumor with `nostr_event_deserialize_unsigned`, which rejects a `sig` key (`event.c:437-440`).
    - It accepts only kind 14.
    - It enforces the NIP-17 MUST `rumor.pubkey == seal.pubkey`.
    - It validates a declared id, or computes the canonical id when none is declared. The canonical id commits to the verified sender.
    - It requires lowercase, unique, bounded `p` tags (at most 128).
    - It requires the account to be the sender (self-copy) or a recipient.
  - **No secret key.** Both decrypts go through `gh_account_controller_nip44_decrypt_with_cancellable_async` (`gh-account-controller.c:497-509`), so the app never holds an nsec.
  - **Staleness.** `current()` checks the generation, the active npub and the caller's cancellable after each signer call, before the result is built, and in `_finish` (`:66-75,198,252,307,333`). A stale failure is reported as `G_IO_ERROR_CANCELLED` (`:79-89`). The caller's cancellable and the generation cancellable are both wired to revoke the pending signer call (`gh-account-controller.c:417-441`).
  - **Tests.** They build a real wrap → seal → rumor chain through the mock D-Bus signer. They assert both the rejection code and the number of signer calls made before rejection, for 14 tamper cases plus the bounds cases (`tests/app/test_account_controller.c:1445-1520`). They also cover signer denial at each decrypt, and cancellation or account switch at each stage, including revocation of the held approval.
- **Seen-set**
  - **File format and limits.** The file is bound to the account through its header. Its size is bounded by `(2·capacity+1)` lines, and a malformed or foreign file is refused rather than partly trusted (`gh-nip17-inbox.c:382-445`).
  - **Torn tail.** A torn tail is ignored, and the next record triggers an atomic rewrite (`:426-430,537-538`), which fixes the glue-on-fragment bug described in `ffe02e16`.
  - **Permissions.** Creation and rewrite use mode 0600: `g_file_set_contents_full(CONSISTENT, 0600)` for rewrites, and `open(…, 0600)` plus `fchmod` for appends (`:478-479,493-496`). The `seen-torn-tail` and `seen-file-mode` tests exercise both behaviours against real files.
- **Relay publish**
  - **Validation and URL set.** The event is validated as signed, with a matching id and a verifying signature, before any I/O (`gh-relay-publish.c:107-136`). URLs are validated and bounded to 16, with no fallback relays.
  - **Terminal outcomes and OK matching.** `finish_endpoint` (`:277-312`) is terminal-once per URL, because `pending_endpoint` refuses non-PENDING URLs and any delivery after cancellation. OK frames are matched on the exact canonical event id (`:400`).
  - **Lifetime.** `finish_endpoint` holds a reference across the transport close and the user callbacks. That makes it safe for an update callback to drop the caller's last reference or to cancel. `deadline_expired` touches only a stolen local `GSource` after `finish_endpoint` returns.
  - **Transport threading.** `gh-relay-publish-gnostr.c` uses a private `GNostrRelay` per URL and disables auto-reconnect. Signal handlers on the global default context only copy their arguments and queue them to the owning context (`:104-147`). There, an atomic `closed` flag is checked before the borrowed publish pointer is used. `close_publish` sets `closed` and disconnects the handlers before dropping its reference.
  - **Handle references.** Refcounts cover the pending connect, both signal closures and each queued delivery.
  - **Tests.** The wire test runs against a real local libsoup relay. It covers partial success, a mismatched-id OK and a repeated OK, a relay that is down, a bystander relay that is never contacted, cancellation before the OK, an OK arriving after cancel (via an emission hook), and callbacks delivered on a non-default owner thread (`tests/relay/test_relay_publish_wire.c:148-395`). This is not mock-theater.
- **NIP-29**
  - **Admission.** `gh_nip29_group_admit` (`gh-nip29-group.c:375-426`) accepts an event only when all of these hold:
    - it passes full id and signature validation;
    - its author equals the caller-supplied NIP-11 `self` key;
    - its *first* `d` tag equals the group id;
    - it is newest per kind, with ties broken by the lexically lowest id.
  - **Rejections leave state untouched.** The rejection checks all run before `merge_snapshot`, and the nips/nip29 merge helpers replace state wholesale rather than accumulating it (`nips/nip29/src/nip29.c:478-740`).
  - **Authorization is conservative.** `gh_nip29_group_can` is TRUE only for `ALLOWED`. Missing 39001 or 39003 data, roles not advertised in 39003, and roles the policy does not cover all yield DENIED or UNKNOWN (`:692-736`). Members from 39002 are reported as `PARTIAL` or `UNAVAILABLE`, never as a complete list.
  - **Templates.** They match the current NIP-29:
    - `h` tag first on every template;
    - 9000 `p` plus optional roles, 9001 `p`, 9005 `e`;
    - 9009 and 9021 `code`;
    - 9002 carries the full metadata set: name, picture, banner, about, private, restricted, hidden, closed, livekit, supported_kinds, parent and child;
    - `previous` holds 8-character prefixes drawn from the last 50 events, excluding the author's own (`gh-nip29-template.c:14-50,113-360`).
  - **Tests.** They use real Schnorr-signed events, including lifted-signature, relabelled-pubkey, second-`d`, and tie-break-in-both-arrival-orders cases (`tests/nip29/test_nip29_group.c:256-377`).
- **libmarmot cleanup paths.** After the staged clone is created (`mls_group.c:2398-2413`), all 35 failure exits `goto staged_fail` (`:2899-2905`). None of them frees `pre_gc` or `wire_msg` early, so there is no double free. Each exit clears `commit` exactly once before the jump, and `staged_fail` does not clear it again. No bare `return` remains between staging and the success swap at `:2890-2897`. The staged group is an independent serialize/deserialize copy, so freeing it cannot alias the live group.
- **Does the regression prove authentication?** Yes; the negative cases are differential. `build_add_ref_commit_for_test` produces a Commit that the live `mls_group_process_commit_ex` accepts for the genuine proposal (`test_mls_group.c:1410-1474`). That test checks the exact epoch, tree hash, transcript hashes, epoch secrets and Charlie's leaf, and that a reload converges. Each negative variant changes only the stored proposal:
  - wrong group;
  - previous epoch;
  - non-member sender;
  - bad signature;
  - bad membership tag.

  The Commit references exactly that proposal (`:1476-1511`). An implementation that skipped proposal authentication would therefore resolve the reference and apply the Add, and the test would fail. Every rejection also asserts that the whole serialized group is byte-identical to its pre-Commit state.

  The double-reference case is attributable to consumption. `proposal_store_resolve` (`mls_group.c:2091-2106`) marks the stored proposal consumed during resolution (`:2420`), which runs before any Add validation.
- **Versioning is consistent.** The manifest matches both build systems:

  | Component | `VERSION_MANIFEST.md` | Build sources |
  | --- | --- | --- |
  | libmarmot | 0.3.5 | `libmarmot/CMakeLists.txt:16-18` and `libmarmot/meson.build:2` |
  | groundhog | 0.5.2 | `gnome/groundhog/CMakeLists.txt:2` |

  Each bump updated the manifest in the same commit. Both are PATCH changes: the build correction and the reject-path fix for libmarmot, and the shipped internal addition for Groundhog. The CMake and Meson libmarmot source lists are identical.

### Non-blocking (follow-up suggested)

1. **A failed seen-set append can still corrupt the log (`gh-nip17-inbox.c:488-510,537-540`).**
   - **Cause.** `ffe02e16` fixed a torn tail left by a crash. A *reported* append failure, however, does not set `needs_rewrite`. That failure can be a short `fwrite`/`fflush` from ENOSPC or EIO that leaves a partial line on disk.
   - **Effect.** The next `gh_nip17_seen_record` in the same process sees that the file exists and appends again, gluing a new entry onto the fragment. The next `gh_nip17_seen_open` then refuses the whole file as malformed. This fails closed, but the dedup state is lost, which is the same failure class `ffe02e16` addressed.
   - **Suggested fix.** Set `seen->needs_rewrite = TRUE` whenever `seen_append` fails.
   - **Impact today.** None, because there is no production caller yet.
2. **Write failures in the publish transport surface only through the deadline (`gh-relay-publish-gnostr.c:159-169,95-100`).**
   - **Cause.** `nostr_relay_publish` returns `void` (`libnostr/src/relay.c:1494-1571`), so a failed or timed-out EVENT write is only logged.
   - **A second path to the same result.** A `DISCONNECTED` delivery can run before `on_connected` sets `established`, for example when a connection is lost immediately after the handshake. That delivery is then dropped (`:98`).
   - **Effect.** In both cases the URL still reaches a correct terminal `CONNECTION_FAILED`, but only when the per-relay deadline fires (default 30 s) or a later state change arrives.
   - **Suggested fix.** Report the write result from the worker thread, for example with a libnostr publish variant that returns an error, and queue it as a `DELIVER_LOST`.
3. **The version-impact note in `d4506d10` is inaccurate (`gnome/groundhog/CMakeLists.txt:150-155`).** It says `groundhog-relay` is "not linked into the shipped app". It is linked, through `groundhog-account-relays`, and the change to `gh_relay_scope_add_url` (rejecting an empty `ws://` host) changes shipped behaviour. The 0.5.2 PATCH in this range already covers it, so no action is needed beyond noting it.
4. **One regression assertion is attributed to the wrong check (`libmarmot/tests/test_mls_group.c:1461-1466`).** The comment says "consumed parent-epoch reference cannot be replayed", but the replay is rejected by the epoch gate (`mls_group.c:2366`) before reference resolution. The assertion proves there is no cross-epoch replay, not consumption. Consumption is already proven by the double-reference case (`:1533-1542`), so this only needs a comment fix.
5. **Unwired code and scope notes.**
   - `gh-nip17-inbox.c` is compiled into the production executable (`gnome/groundhog/CMakeLists.txt:85-90`) but has no caller. The publish core has none either, and the NIP-29 core is not linked into the app. Track the inbox REQ and UI wiring so this does not stay dead code.
   - The unwrap rejects kind-15 file messages and kind-7 reactions, which NIP-17 also allows. This is documented and conservative.
   - The seen-set `capacity` counts keys, and each message records two keys. With `capacity == 1` the wrap key is evicted immediately. Document this, or require `capacity >= 2`.

## Verification

- `git submodule update --init third_party/nsync third_party/nostrdb`, then an out-of-tree `cmake -S . -B /tmp/gh-w10-review -G Ninja -DBUILD_GROUNDHOG=ON -DBUILD_APPS=OFF -DBUILD_NOSTR_GTK=OFF && cmake --build /tmp/gh-w10-review -j8`: configure and build succeeded (1103/1103 targets).
- `ctest --test-dir /tmp/gh-w10-review -R 'groundhog-|marmot|nip29' --output-on-failure`: **38/38 passed**, with 1 platform skip (`groundhog-launch`). This includes `marmot_test_mls_group`, `groundhog-relay-publish`, `groundhog-relay-publish-wire`, `groundhog-nip29`, `groundhog-account` and `test_nip29`.
- Standalone libmarmot Meson (the `71736c5d` repair): `meson setup /tmp/gh-w10-meson -Dnostrc_build_dir=/tmp/gh-w10-review && meson compile && meson test` gave **20/20 OK**. The default pkg-config path, which needs installed sibling libraries, was not exercised.
- macOS `leaks --atExit` reported **0 leaks** for `test_mls_group` (every reject path above exercised), `test-groundhog-nip29` and `test-groundhog-relay-publish`.
- `git diff --check f3a27ff9..78cb3930` passed.
- The integrator reported the full opt-in CTest at 288/288 (2 platform skips). This review did not re-run the full suite.

**APPROVED**: scoped to integrated commit `78cb3930e978c23d5fe3098aaeccaa68a60dda06` versus `f3a27ff90106b352f44e6e709fb063c37fc64297`. There is no blocking finding; the five non-blocking items above are recommended follow-ups.
