# Groundhog W17 review: NIP-17 rooms, encrypted attachments core, Tor fail-closed without G09, libnostr REQ/COUNT (2026-09-29)

## Context / scope

This is the independent peer review that AGENTS.md requires. It covers the commits below on local `master`, reviewed on branch `groundhog/w17-review` at `5893b836`. File:line references are to `5893b836` unless a commit is named.

| Commit | Bead | What |
|---|---|---|
| `8535de5a` | `nostrc-ptwq` | libnostr: REQ and COUNT keep their closing bracket; EVENT sub-id escaped; `test_envelope`; fuzz round trip; `gh-relay-soup` uses the library serializer |
| `45bd56fb` | `nostrc-6v0i` | Relay guard: `network-mode=tor` connects to nothing in builds without G09; banner, Preferences note, `GROUNDHOG_WITHOUT_TOR` |
| `89ac973b` | `nostrc-qp24.78` | NIP-17 multi-recipient send (rooms of up to 10) |
| `ae2f8115` | `nostrc-qp24.39` | G21: encrypted attachments core (kind 15, AES-GCM, stripping, Blossom, encrypted media cache) |
| `fd656c3f` | `nostrc-5x5b` | Cached attachment plaintext is purged with its message |
| `5893b836` | `nostrc-qp24.89` | e2e-dm waits for the self-copy |
| `3529f809`, `c02df3e5`, `1be25634`, `bc51e3ae` | — | Skimmed only (reviewed separately, see below) |

- **Ignored.** `chore(beads)` commits and `.beads/` hunks.
- **Normative references.**
  - `docs/designs/groundhog-privacy-ux-charter-2026-09-28.md`: §3.6, §3.7, §4.3–§4.5, §6, D6, AT-1…AT-9 and PT-4;
  - NIP-17, NIP-59, NIP-44 and Blossom BUD-01/02;
  - `docs/reviews/groundhog-w16-review-2026-09-29.md` and its addendum.
- **Changes.** I changed no code or beads. Scratch programs live in `/tmp/w17scratch` only.

## Summary

- **Multi-recipient send (`89ac973b`) is correct and well tested.**
  - One rumor names every recipient. It is sealed and wrapped once per recipient plus the self-copy.
  - Ephemeral keys, seal times and wrap times are checked distinct and redrawn on a collision.
  - Each wrap goes only to its receiver's 10050 relays, on its own connection with its own ephemeral AUTH.
  - Outbox state, retries and restarts are per recipient, and the copy is honest.
  - One privacy nit: the S4 spacing holds only for the first round, not for retries (non-blocking #1).
- **The attachments core (`ae2f8115`) is sound in its crypto and transport.**
  - AES-256-GCM uses a fresh key and nonce from `RAND_bytes`. `x` is checked before the cipher runs, the tag inside OpenSSL, then `ox`.
  - Upload auth uses a fresh throwaway key per upload attempt. The account signs only with per-server consent, and its answer is re-verified.
  - Tor mode fails closed. No production code can fetch or upload yet.
  - The metadata stripper survived 3 million ASan/UBSan mutations with no memory error and no invariant broken, and it accepts every real JPEG/PNG I fed it.
- **The decrypted-attachment cache is bound to the wrong thing.** Its rows are keyed, served and kept alive by `x` alone: the SHA-256 of the ciphertext, which every Blossom operator knows.
  - A kind-15 message that only *names* someone else's `x`, with any key, is served that file's plaintext from the cache without a GCM or `ox` check.
  - The same message keeps the plaintext alive after the original disappearing message expires, which defeats `fd656c3f`'s guarantee.
  - Reproduced. This is **B1 (blocking)**.
- **The relay guard (`45bd56fb`) is right, but its test breaks CI.**
  - It fails closed in every case I tried, and a mutant that ignores the mode fails its tests (10 tripwire connections).
  - But `groundhog-ci.yml` never builds `test-groundhog-relay-guard`. Its ctest step selects `^groundhog-` and fails on "Not Run", so the required Linux job goes red as soon as this is pushed. This is **B2 (blocking)**.
- **The rest is clean:**
  - `8535de5a` (libnostr);
  - `fd656c3f`, apart from the binding in B1;
  - `5893b836`.

## Findings

### Blocking

#### B1. Medium: the attachment cache serves and retains plaintext by `x` alone, so a message that merely names another file's hash gets its plaintext and pins it past its expiry

- **Where.**
  - `gnome/groundhog/src/media/gh-attachment.c:379-387`: `gh_attachment_download_async` looks up `gh_store_media_get(cache, x)`. On a hit it returns the bytes as this message's file, without its key, nonce or `ox` playing any part.
  - `gnome/groundhog/src/store/gh-store-media.c:116-131` and `:181`: `put` admits, and `get` serves, a row for any stored kind-15 message whose `x` tag matches.
  - `gnome/groundhog/src/store/gh-store-media.h:35-41` and `gnome/groundhog/src/store/gh-store.c:3239-3242` (`MEDIA_FORGET`): a row survives a deletion while *any* other kind-15 message names the same `x`.
- **Why it matters.**
  - `x` is not a secret. It is the Blossom address of the ciphertext (`<server>/<x>`), known to every server the file was offered to, including a fallback server that received the PUT and answered badly (`gh-blossom-client.c:377-384`).
  - The charter lists Blossom operators under A1.
  - What binds a file to a message is the message's key and nonce (and `ox`). The cache drops that binding.
- **Reproduced.** `/tmp/w17scratch/repro_cache.c` is linked against the build's `libgroundhog-media`/`-store`.
  1. Bob sends the account a disappearing (1 day) kind-15 file. The account downloads it, so it is cached.
  2. Mallory, who knows only `x`, sends a kind-15 message with `x` equal to Bob's, a key of `0x42…`, no `ox`, and her own URL.
  3. Output:
     ```
     control decrypt with Mallory's key: This file was changed or damaged.
     download of Mallory's message: cached=1 error=(none) bytes=BOBS-PRIVATE-PHOTO-PLAINTEXT
     after Bob's message expired: n_expired=1 n_media=0 cached=STILL PRESENT
     ```
- **Failure scenarios.** Mallory is any Blossom operator who can message the victim; her message may sit in Requests.
  1. **Disappearing messages defeated (P3, §3.7, `fd656c3f`).** Alice sends Bob a 1-day disappearing photo, and Bob downloads it. Mallory's message naming its `x` keeps the decrypted photo in Bob's store for as long as her message exists (retention "forever" by default, D10). Bob has no way to know. Mallory needs no key and gains no content; she simply defeats the deletion.
  2. **Content misattributed, integrity skipped (AT-2).** When G22 ships, Download on Mallory's message shows Alice's private photo as Mallory's attachment ("look what I have"), although decrypting with Mallory's key fails. The "x, then tag, then ox" chain never runs for that message.
  3. **Cache oracle that undoes Tor.** Mallory's URL points to her own server. If Bob taps Download and no request arrives, Bob has `x` cached, so he received that file. In Tor mode this links an anonymous download of `x` to Bob's account.
- **Not caught.** `/groundhog/media/cache` and `groundhog-media-purge` only ever name a file from one message, or from a genuine copy.
- **Suggested fix.** Bind a row to the decryption parameters, not only to `x`.
  - For example, key or tag each row with the file's `decryption-key` and nonce, or with SHA-256(x ‖ key ‖ nonce). Then:
    - `get` needs the requesting message's parameters;
    - the put guard and `MEDIA_FORGET`'s survivor test compare `x` *and* key.
  - The key tag sits in the canonical rumor just as `x` does, so the existing `instr` extraction extends to it.
  - A genuine forward carries the same key and nonce, so "a forwarded copy keeps it" still holds.
  - On a hit, also check `ox` when the message gives one.
- **Suggested tests.** Two cases in `groundhog-media-purge` / `groundhog-media`, mirroring the repro:
  1. A second message with the same `x` and another key gets no cached bytes. It goes to the network and fails as damaged.
  2. Expiring the first message deletes the row despite the second.
- **Severity.** Medium, because `GH_FEATURE_ATTACHMENTS` is 0 and no production code calls `gh_attachment_download_async` yet (grep). It blocks because the fix changes the cache's key and what the purge compares. That is cheap now, it is needed before G22 builds on it, and `fd656c3f`'s stated guarantee is false without it.

#### B2. Medium: the new `groundhog-relay-guard` test is never built by the Groundhog CI job, which then fails it as "Not Run"

- **Where.**
  - `.github/workflows/groundhog-ci.yml:48-69` builds an explicit target list without `test-groundhog-relay-guard`.
  - `:113-121` runs `ctest -R '^(groundhog-|nostrc-test-bus-selftest$)'` and exits 1 on `Not Run`.
  - The test is registered in every configuration (`gnome/groundhog/CMakeLists.txt:1621-1628`). `ninja -t query` shows its executable feeds only `all`.
  - `45bd56fb` did not touch the workflow, and the later commits' list edits did not add it.
  - It is also missing from the required set (`:80-102`) and from `GROUNDHOG_SANITIZER_TESTS` (`:173-186`), though it is display-free.
- **Reproduced.** In `/tmp/w17rev`, with the executable moved aside as it would be in CI, `ctest -R '^groundhog-relay-guard$'` reports `groundhog-relay-guard (Not Run)`, which the job's grep turns into a failure.
  - Of every `^groundhog-` test, this is the only one whose executable is off the list: a script cross-checked `ctest --show-only=json-v1` against the list.
  - The last pushed run (`36561983479`, `d9f44a1d`) predates these commits, so no CI run has seen them.
- **Scenario.** The session-close protocol pushes `master`. The required "Groundhog opt-in CI" job then fails on every push until the list is fixed, and the guard's own regression tests never run in CI.
- **Why it was missed.** `scripts/groundhog-linux-ci.sh` (`3529f809`) says it mirrors the job, but it builds *all* targets (`cmake --build /build`), so the gap cannot show there.
- **Suggested fix.** Add `test-groundhog-relay-guard` to the build list, `groundhog-relay-guard` to the required set and to the sanitizer list. Optionally make the script build the job's target list, so it really mirrors CI.
  - A CI cell with `-DGROUNDHOG_WITHOUT_TOR=ON` would also keep the no-G09 configuration honest; today it is checked by hand only. I built it: it defines `GROUNDHOG_HAVE_TOR=0` and `GROUNDHOG_HAVE_RELAY_GUARD=1`, and `groundhog-relay-guard`, `-preferences` and `-shell` pass.

### Non-blocking (follow-up suggested)

1. **Low: S4 room spacing applies to the first round only; retries publish every due wrap at once, in "p" order** (`gnome/groundhog/src/app/gh-outbox.c:901-921`, `:930`, `:1279-1297`).
   - **Why.** `room_schedule` stores `not_before` once, at T-seal. A retry round's legs all have `not_before` in the past, so `round_start_legs` starts them in the same main-loop iteration, in stored order, which is the rumor's "p" order.
   - **Scenario.** Relay R is on Bob's and Carol's lists and is down when Alice writes to the room. On the 15 s retry, R receives both wraps back to back on two connections from the same IP, in "p" order. That is the correlation S4 exists to blur, in its most likely case: a shared relay that was down.
   - **Suggestion.** For rooms, draw a fresh in-memory shuffle and U(0, 3) s gaps for each automatic or manual round, and extend `test_wire_spacing` to a retry.
   - **Nit.** Gaps are whole seconds from an inclusive range (`gh_clock_random_range(0, 3)`), so a quarter of gaps are 0 s.
2. **Low: a kind-15 message's raw Blossom URL is shown as the conversation-list preview and as a request's first message.**
   - **Where.** `gh-conversation.c:154-165` feeds `gh-conversation-row.c:255`; `gh-requests-view.c:122`.
   - **What works.** The row (`gh-message-row.c:109-122`) and the notifier (`gh-notifier.c:443-445`) say "Photo"/"File".
   - **Scenario.** A received photo shows `https://blossom.example.com/7d86…` under the contact's name in the sidebar and in Requests. It is not a link and fetches nothing, but it contradicts `ae2f8115`'s "never … its URL" and shows the ciphertext address in the UI.
   - **Suggestion.** One helper for the text of a message (Photo/File for kind 15), used by all four sites.
3. **Low: a sender-chosen kind-15 URL can point Download at loopback or LAN services** (`gh-nip17-file.c:101-114` accepts any http(s) host; `gh-net-http.c:152-178` allows `http://` to loopback in every mode, meant for fixtures, and `https://` to any host).
   - **Scenario.** Once G22 has a Download button, a stranger's message names `http://127.0.0.1:631/…` or `https://192.168.1.1/…`. The tap makes Groundhog send a GET to a local service in System or No Proxy mode. The response is not returned to the sender, so the risk is CSRF-style side effects, not disclosure.
   - **Suggestion.** For attachment downloads, refuse literal loopback, link-local and private addresses (and the loopback `http` exemption) outside test builds.
4. **Low (test): AT-2's "x is checked before the cipher runs" cannot fail** (`gnome/groundhog/tests/media/test_media.c:485-491`).
   - **Why.** With the key zeroed, a tag-first implementation also returns `GH_ATTACHMENT_ERROR_DAMAGED` with the same message. The code's order (`gh-attachment.c:208-223`) is right, but a reordering would pass.
   - **Suggestion.** A test-only counter or seam around `aes_gcm`, asserted 0 on an `x` mismatch.
5. **Low (copy): a guard build's onboarding still says "through Tor"** (`gh-onboarding-view.c:1373-1379`).
   - **Scenario.** A build without G09, with a stored `tor`, shows "Checking connects to the ticked relays through Tor", while the guard connects to nothing.
   - **Mitigation.** The per-relay results do show the guard's reason (`gh-inbox-setup.c:443-447`).
   - **History.** The W16 addendum (note 2) asked `nostrc-dod2` to fix this. `nostrc-dod2` was closed as a duplicate of `nostrc-6v0i`, and `45bd56fb` does not touch onboarding.
   - **Suggestion.** Read "tor allowed" from the build (the guard's `gh_relay_guard_mode_allowed`) as well as from the setting.
6. **Nits.**
   - **Stripper keeps some free-form bytes.**
     - Kept whole: APP2 ICC profiles (multi-chunk unvalidated, `gh-metadata-strip.c:171-177`), PNG `iCCP` (a 79-byte name plus profile), DAC segments of any even length (`:191-192`), and the reserved JPG marker `0xC8`, which passes the SOF shape check (`:196-198`, while `jpeg_sof` excludes it).
     - A camera or phone ICC profile's `desc`/`dmdd` tags can name the device model.
     - Acceptable for v1 since decoders need colour, but worth a line in the header comment.
   - **Plaintext residue in heap.**
     - `sha256_hex` hashes plaintext through a `GChecksum` that is freed unwiped (`gh-attachment.c:41-49`, up to one 64-byte block).
     - For non-JPEG/PNG files `prepared->plaintext` is the caller's own `GBytes`, which the core cannot wipe.
     - The file's key survives in freed `NostrTag` strings and the unwiped JSON in `rumor_new_kind` (`gh-nip17-envelope.c:409-419`).
     - None of this is at rest (P3), but the header's "wiped when freed" is broader than true.
   - **Borrowed store.** `gh_attachment_download_async` borrows the store and must be cancelled before it closes (`gh-attachment.h:111-117`). G22 should own that with the account generation.
   - **Room linkability.** All N wraps of a room message have the same size (same rumor, NIP-44 padding) and leave from one IP within seconds. This is inherent to NIP-17, not a defect. Say so in Conversation Info's "What others can see" for rooms, next to "everyone in the conversation can see who else is in it".

## Per-commit notes

### `8535de5a` libnostr REQ/COUNT (`nostrc-ptwq`)

- **The fix.** Both branches (`libnostr/src/envelope.c:241-243`, `:290`) now leave `snprintf`'s terminator after `]`. The buffers were already sized for the bracket.
- **EVENT sub-id.** `event_envelope_marshal_json` escapes the id with `json_escape_string_min`, which includes the quotes. The size is exact: 9 + |sid| + 1 + |event| + 1, plus the NUL.
- **Tests.** `test_envelope`, which was an orphan, is built and round-trips every type. `test_fuzz_envelope_roundtrip` checks fixed points over 11 new seeds. Both pass here.
- **`gh-relay-soup.c`.** It uses the library serializer again, and the frames are byte-identical: ids are alphanumeric, so escaping is a no-op. The relay wire suites, which parse strictly, pass.
- **Unchanged behaviour.** Both transports share the pre-existing behaviour that filters beyond `nostr_limit_max_filters_per_req()` are trimmed with a WARN.

### `45bd56fb` relay guard (`nostrc-6v0i`)

- **Fails closed.**
  - Only "system" and "none" dial (`gh-relay-guard.c:37-41`). "tor", unknown modes, and no guard installed (`:127-148`) all refuse.
  - `.onion` is refused in every mode before any open, so it is never resolved.
  - A publish is refused synchronously.
  - A scope URL stays registered and connects when the user leaves Tor. The registry is joined before the mode is read, so a change always reaches it.
  - Teardown and rebuild run on the owning context, and `rebuild` re-checks `closed` after the DISCONNECTED notice.
  - System ↔ No Proxy is a no-op.
- **Mutation check.** A guard whose `mode_allowed` returns TRUE fails:
  - `decision`;
  - `tor-refuses` (the refusal never comes);
  - `tripwire` (`tripwire.accepted == 0`: 10 ≠ 0).
  The tests are genuine.
- **No-G09 build.** `-DGROUNDHOG_WITHOUT_TOR=ON` (fresh `/tmp/w17rev-db07e6a2-notor`) configures "network-mode tor connects to nothing" and builds `groundhog`. The guard, preferences and shell tests pass.
- **UI.**
  - The banner ranks above Offline with [Network Settings].
  - Preferences swaps the direct-connection note for `tor_unavailable_note` only while the mode is refused.
  - The `.blp` and `.ui` are in sync (`groundhog-blueprint` passes).
  - The metainfo gains the 0.9.1 release.
- **CI.** See B2.

### `89ac973b` NIP-17 multi-recipient send (`nostrc-qp24.78`)

- **One rumor.**
  - `gh_nip17_rumor_new_room` validates 1–10 distinct recipients; the sender is allowed only alone, as a note to self.
  - It adds one "p" per recipient, and one recipient gives the old rumor byte for byte.
  - `gh_nip17_rumor_dup_recipients` re-validates a stored rumor.
- **A wrap per recipient plus the self-copy** (`gh-nip17-envelope.c:198-370`).
  - Each destination gets its own NIP-44 encryption and seal signature.
  - Each seal is re-verified: kind, pubkey, content, time, and exactly the drawn expiration.
  - Each wrap gets a fresh ephemeral key (`nostr_nip59_wrap(…, NULL)`, or an own key re-signed when an expiration is added).
  - A repeated outer key, seal time or wrap time is redrawn (`wrap_linkable`, `draw_seal_time`, at most 16 times).
  - The generation and cancellation are checked after every signer step.
- **Expirations.** `gh_expiry_draw_room` draws each recipient's seal and wrap and the self-copy's independently (§3.7).
- **Per-recipient routing and state.**
  - `store_sealed` writes one event per recipient with that recipient's targets only, in one T-seal.
  - `on_resolved` adds new relays to that recipient's wrap only.
  - `leg_start` makes one `GhRelayPublish` per wrap, so a relay on two lists gets two connections with two ephemeral AUTHs (R1, R2).
  - A recipient without a 10050 keeps a targetless wrap. Nothing is published for them; they are NO_INBOX when known absent and RETRYING while unknown.
  - "Sent" needs every recipient; anything less is "Sent to some people" (§3.6).
- **Tests are wire-level and genuine.** They cover:
  - `wire/room`: the shared relay's two connections and two distinct AUTH keys, each receiver opening only its own wrap, distinct outer keys and times, and signer calls = 3 × 2 + 1;
  - `wire/partial-retry`: a restart, only Carol's stored wrap republished, never re-sealed;
  - `wire/missing-inbox`, `wire/lookup-and-nobody`;
  - `wire/spacing`: scripted order and gaps;
  - `wire/disappearing`;
  - `/groundhog/privacy/pt4-room`: Bob's wrap on exactly A and B, the self-copy on C as Alice, nothing for Carol and no 10002 fallback, no frame naming another member, the three-person room id, H7 clean.
- **Copy.**
  - New Message now says each message is encrypted separately and everyone sees who else is in it.
  - The details name who has it, who is still pending and who has no message relays.
  - Try Again is offered for "Sent to some people", with a tooltip saying only the others get it.
  - "Sent" stays relay acceptance (UX-5).
- **Open items.** Non-blocking #1 and the room-size nit.

### `ae2f8115` G21 encrypted attachments core (`nostrc-qp24.39`)

- **Kind-15 parser and builder** (`gh-nip17-file.c`).
  - Exactly one of each file tag. The URL must be http(s) with a host and no user info or control bytes. `aes-gcm` only (anything else is NOT_SUPPORTED).
  - 32-byte hex key; 12- or 16-byte nonce; 64-hex `x`/`ox`, normalised to lowercase; canonical `size` and `dim`.
  - The inbox and `GhMessage` admit kind 15 only through it.
  - The builder is the one canonical tag list for both rumor paths, and it wipes key strings.
  - The Amethyst/0xchat 16-byte-nonce vector decrypts.
- **AES-GCM** (`gh-attachment.c:118-233`).
  - A fresh key and nonce per file from `RAND_bytes`, so a nonce is never reused under a key.
  - `SET_IVLEN` before the key. The tag is set before `Final` and checked inside OpenSSL (constant time).
  - The plaintext output goes into a wiped buffer that is dropped on failure.
  - `x` (public) is compared with `g_ascii_strcasecmp`. Neither `x` nor `ox` needs a constant-time comparison.
- **Metadata stripping** (`gh-metadata-strip.c`).
  - Every read is bounds-checked and output never exceeds input (`out_append` asserts it).
  - Kept segments must be structurally exact, and kept PNG chunks must pass their CRC.
  - Damaged files are refused, never passed on.
  - **Fuzzing.** An ASan+UBSan mutation fuzzer ran 3,000,000 inputs from 24 seeds: bit and byte flips, 0xFF/0x00 pokes, BE16/BE32 length pokes, marker insertion, run insertion and deletion, and truncation. Results: 580,293 accepted, 2,327,664 refused, 92,043 not images. There was **no sanitizer report**, and every accepted output was:
    - no larger than its input;
    - a fixed point of a second strip;
    - free of APP1/APP13/COM header segments and of `tEXt`/`iTXt`/`zTXt`/`eXIf`/`tIME` chunks.
  - **Real files.** The seeds were 11 system JPEGs (one with 4.7 MB of EXIF), 11 PNGs, and two files injected with EXIF, XMP, IPTC, COM, ICC, a trailer, text/eXIf/tIME/iCCP and a private chunk. All were accepted, so real-world files are not refused.
  - For the bytes it deliberately keeps, see nit 6.
- **Decode guard.** PNG/JPEG by magic bytes only. The PNG `IHDR` and the first JPEG SOF (after a full structural walk) must be at most 8192×8192 and non-zero, so a DNL height of 0 is refused. AT-4 covers 50,000 × 50,000.
- **Blossom** (`gh-blossom-client.c`).
  - **Uploads.** `PUT /upload` goes over GhNetHttp only, so the mode, isolation, no-redirect and no-resumption policy all hold.
    - The 24242 auth has `t`/`x`/`expiration(+5 min)`/`server`, from a fresh key per server attempt that is wiped after signing.
    - With consent, the account's answer must match exactly (`auth_verify`).
    - 401/403 stops the upload and nothing goes elsewhere.
    - Other failures move to the next server.
    - The descriptor must match `x` and the size, and the URL is built as `<server>/<x>`. No server-chosen address is used.
  - **Downloads.** The size is checked before any request. The body is capped at min(size, cap + 16) + 1 KiB, by Content-Length and by a read of `max + 1`. Status must be 200 and no redirects are followed.
  - AT-3, AT-6, AT-8 and AT-9 are genuine (distinct throwaway keys, zero signer calls, the lying-signer case, SOCKS ATYP 3 with fresh credentials per request, fail-closed with Tor down, `.onion` refused outside Tor).
- **No network before consent.**
  - `blossom-servers` defaults to `[]`, which gives NO_SERVER before any contact.
  - No `src/` caller of the upload or download APIs exists (G22), so today's app cannot fetch or upload.
  - Kind-15 rows render as "Photo"/"File" with no link or preview (`test_file_message`), and PT-2 admits kind 15 with zero fetches.
- **Plaintext at rest.** The cache lives only inside SQLCipher, and AT-5 with attachments (`test_privacy_e2e.c:2048-2142`) runs send, receive and download end to end. Afterwards:
  - the transient directories are empty;
  - the data directory holds only the two stores;
  - H7 is clean in files, frames, settings and logs.
- **Open items.** B1, non-blocking #2–#4 and nit 6.

### `fd656c3f` purge cached plaintext with its message (`nostrc-5x5b`)

- **Every message-deleting statement in `src/` is covered.**
  - Expiry and retention (`gh-store.c:3409`) and forget, which includes block-and-forget and delete-request (`:3529`), run `MEDIA_FORGET` in the same transaction, before the messages go.
  - So does outbox delete (`:3253`).
  - The NIP-29 delete (`gh-store-nip29.c:479`) is irrelevant: NIP-29 admits only kinds 9–12.
- **Deletion is real.** `secure_delete` and each path's WAL checkpoint apply. The test decrypts every page and WAL frame with the store key and scans them, with a secure_delete-off control.
- **The SQL string match is safe.** The inbox stores the rumor re-serialized compactly (`gh-nip17-inbox.c:370-388`). In compact JSON every `"` inside a string is escaped, so `["x","` can only be the structural `x` tag, and the strict parser allows exactly one. `lower()` covers upper-case senders.
- **The put guard is right.** A late download keeps nothing.
- **Remaining gap.** What "a message names this file" should mean is B1.

### `5893b836` e2e-dm self-copy wait

It is correct. SENT is recipient acceptance, so waiting (bounded) for the self-copy and then asserting exactly one wrap removes the race without weakening the check.

### Skimmed

- **`3529f809`.** Test and script only, and sound. The script's build of all targets is why B2 went unnoticed.
- **`c02df3e5`, `1be25634`** (libgo/libnostr `nostrc-75rv`) and **`bc51e3ae`** (libmarmot Welcome path secret). Approved in their own reviews. Nothing here contradicts that, and their suites pass in the full run below.

## Verification

- **macOS 15 (Darwin 24.6, arm64).** Submodules initialised (`third_party/nostrdb`, `third_party/nsync`).
- **Build.** `cmake -S . -B /tmp/w17rev -G Ninja -DBUILD_GROUNDHOG=ON && ninja -C /tmp/w17rev` completed all 2,334 build steps. There were no new Groundhog warnings, only the known `ld` duplicate-library notices. `apps/gnostr/data/ui/dialogs/gnostr-profile-edit.ui` was rewritten by the build and restored.
- **Tests.**
  - `ctest --test-dir /tmp/w17rev -j6 --timeout 300`: **423/423 passed**, with five platform skips (`test_nip5f_tcp`, `groundhog-launch`, `-store-key-keyring`, `-background-gui`, `-notifier-gui`).
  - Stress: multi-send, media, blossom, media-purge, relay-guard, privacy-e2e, e2e-dm, composer, net, outbox, dm-send, preferences and shell with `--repeat until-fail:5 -j4`, alongside a Docker build: **65/65 runs passed**.
- **Linux container.** `scripts/groundhog-linux-ci.sh` with a private build volume (Ubuntu 24.04, `dbus-run-session -- xvfb-run`, the CI's ctest regex): **67/67 passed**, `groundhog-relay-guard` among them.
  - It builds every target, so it cannot show B2.
  - It ran concurrently with the macOS stress run, which is extra load.
- **Reproductions and checks (scratch, `/tmp/w17scratch`).**
  - B1: `repro_cache.c`, output above.
  - B2: ctest with the unbuilt executable gives "Not Run". A script cross-checked registered `^groundhog-` tests against the CI build list; `ninja -t query` gives the target's dependents.
  - Stripper fuzzing: `fuzz_strip.c`, 3M iterations under ASan/UBSan, as described.
  - Relay-guard mutant: `decision`, `tor-refuses` and `tripwire` fail.
  - No-G09 build: configured fresh, built, three tests pass.
- **Checks.** `git diff --check` over the reviewed commits (excluding `.beads`) is clean.
- **Not run.** A real Tor daemon, a real Blossom server, real public relays, and the hosted CI (the commits are not pushed).

**REQUEST CHANGES**

- **Blocking.**
  - **B1.** The decrypted-attachment cache is keyed, served and kept alive by `x` alone. A message that only names another file's `x`, with no key, is served its plaintext without the GCM or `ox` check, and pins it past its expiry. Bind rows to the key and nonce, in `get`, in the put guard and in `MEDIA_FORGET`, and add the two regression cases.
  - **B2.** `groundhog-ci.yml` does not build `test-groundhog-relay-guard`, so the required job fails with "Not Run" once pushed. Add it to the build, required and sanitizer lists.
- **Everything else is non-blocking.** Highest priority first:
  - S4 on retries (#1);
  - the URL in previews (#2);
  - loopback/LAN download targets before G22 (#3);
  - the AT-2 ordering test (#4);
  - the guard-build onboarding copy (#5).
- **Approved as is.** `8535de5a`, `89ac973b` (with #1 as a follow-up), `5893b836`, and the crypto, stripper, Blossom and Tor parts of `ae2f8115`.

---

## Addendum: fix confirmation (`58308eb8`, `363b9d03` on local `master`)

**Scope.** This pass covers:
- `58308eb8`: ci(groundhog), which builds and requires `groundhog-relay-guard` and runs it under the sanitizers;
- `363b9d03`: fix(groundhog), which binds the attachment cache to the file key (W17 review B1). It was `208e451a` before a rebase.

My branch was rebased onto `master` (`363b9d03`). File:line references are to that commit. I changed no code or beads.

### B2: closed

- **The workflow.** `test-groundhog-relay-guard` is in the build list, `groundhog-relay-guard` in the required set and in `GROUNDHOG_SANITIZER_TESTS`.
- **The new test.** `363b9d03` adds `groundhog-media-cache-binding` to all three lists at once.
- **Cross-check.** I reran my script against `ctest --show-only=json-v1 -R '^(groundhog-|nostrc-test-bus-selftest$)'`:
  - every registered test's executable is in the build list;
  - every registered test is in the required set;
  - both new tests are in the sanitizer set, whose build step builds `test-<name>`.
  No registered test can be "Not Run" in the job any more.

### B1: closed

- **The fix.**
  - A row is kept under `gh_store_media_file_id`. That is SHA-256 over a domain tag (NUL included), `x`, the 32-byte key, a nonce-length byte and the 12- or 16-byte nonce (`gh-store.c:1048-1070`).
  - `gh_store_media_put`, `_get` and `_remove` take the `GhNip17File`, so no `x`-only entry point is left.
  - The store registers `gh_media_file_id(raw_json)` as `SQLITE_DETERMINISTIC | SQLITE_DIRECTONLY` (`:1139-1146`). It is used by the put guard and by `MEDIA_FORGET` (`:3349-3352`) for expiry, retention, forget, block and outbox delete.
  - A hit also re-checks `ox` (`gh-attachment.c`, the cache branch of `gh_attachment_download_async`).
- **C and SQL compute the same identity.**
  - The SQL side reads the `x`, `decryption-key` and `decryption-nonce` tags from the canonical rumor, with the same `["name","` argument as before. In compact JSON an unescaped `["` can only open a tag, and the strict parser admits exactly one of each.
  - It lowercases `x` and decodes key and nonce from either case, as `gh_nip17_file_from_rumor` does.
  - It requires the tag to close right after its value. A 3-element tag, which the parser tolerates, yields NULL. That is fail-safe: no row can exist for it, and none is ever pinned.
- **My repro against the new code** (`/tmp/w17scratch/repro_cache2.c`: the same attack, adapted to the new signatures). Mallory's URL is `https://127.0.0.1:1/<x>` with the test seam on, so it passes the new address policy and reaches a closed port. Output:
  ```
  control decrypt with Mallory's key: This file was changed or damaged.
  direct cache lookup for Mallory's file: miss
  download of Mallory's message: cached=0 error=Could not connect to 127.0.0.1: Connection refused bytes=
  after Bob's message expired: n_expired=1 n_media=1 cached=gone
  ```
  This covers all three B1 scenarios:
  - no plaintext for a message that only names `x`;
  - no pinning past expiry;
  - no cache oracle, because a miss goes to the network whatever the account holds.
  A genuine forward carries the same key and nonce and still keeps the row, as intended.
- **The committed regression test.** `groundhog-media-cache-binding` is my repro as a test, through Tor mode to a `.onion` via the SOCKS5 fixture. With `groundhog-media`'s other-key, other-nonce and put-guard cases and `groundhog-media-purge`'s same-x-other-key case, it covers the defect from both the cache and the purge side.

### Non-blocking items

| Item | Status |
|---|---|
| #1 S4 on retries | Filed as `nostrc-yp69` |
| #2 URL in previews | Fixed. `gh_message_dup_display_text()` ("Photo"/"File") is used by the sidebar preview (`gh-conversation.c`), Requests, the notifier and the row. Only `gh-message-row.c` (non-file bodies) and the store's `body` column still read the raw content. `groundhog-conversations` gains `file-preview` |
| #3 download targets | Fixed (policy review below) |
| #4 AT-2 ordering | Fixed. **Mutation check:** a `gh-attachment.c` that runs the cipher before the `x` check fails `/groundhog/media/at2-tampering` at `test_media.c:492` (decrypt runs 2 ≠ 1); the real code passes |
| #5 guard-build onboarding copy | Filed as `nostrc-4nur`, with an accurate plan (a `tor_available` config field and a test for both builds) |

### The download address policy (`gh-blossom-client.c`, `download_url_allowed`)

- **The path.** It must be `<origin>/<x>[.ext]`, with no query, fragment or user info. That also stops a sender from aiming Download at arbitrary paths (`/admin`, `?x=1`).
- **Host checks.** The host is checked after GUri parsing.
  - **IDN is normalised before the check.** GUri (ENCODED, without NON_DNS) applies IDNA to non-ASCII hosts, so `host_public()` sees the ASCII form.
  - **Tested empirically.** `/tmp/w17scratch/ssrf_probe.c` drives the real `gh_blossom_client_download_async` in System mode against a listener on every local address and counts TCP connections.
    - The positive control (seam on, `127.0.0.1`) got one connection.
    - These got **0 connections and a policy refusal**:
      - `127.0.0.1` and `127.0.0.1.`;
      - `localhost`, `127.1`, `2130706433`;
      - `[::1]`, `[0:0:0:0:0:0:0:1]`, `[::]`, `0.0.0.0`;
      - `[::ffff:127.0.0.1]` and `[::ffff:7f00:1]`;
      - `127.0.0。1` (U+3002), `127．0．0．1` (U+FF0E), `127.0.0.１` and `１２７.0.0.1` (fullwidth digits);
      - `ⓛⓞⓒⓐⓛⓗⓞⓢⓣ.`, which normalises to `localhost`.
  - **Other numeric forms.** `0x7f.0.0.1` and `0177.0.0.1` are refused on Linux by the numeric-last-label rule, since glibc's `inet_pton` rejects leading zeros. On macOS, `inet_pton` reads `0177.0.0.1` as decimal 177.0.0.1, public, and GIO's resolver uses the same parser. So the check and the connection agree, and nothing local is reached.
  - **Zone IDs** (`%`) are refused.
  - **Redirects** are never followed, so a public server cannot bounce Download onward.
- **Not refused: IPv6 forms that embed an IPv4 address** (new note 1 below):
  - IPv4-compatible `[::127.0.0.1]` / `[::7f00:1]`;
  - SIIT `[::ffff:0:127.0.0.1]`;
  - NAT64 `[64:ff9b::7f00:1]`.
  All four passed the policy and failed here only with "No route to host".
- **Regressions.**
  - The test seam (`gh_blossom_client_set_allow_private_hosts`) has no caller in `src/`. Only the Blossom, e2e and multi-send tests set it, for their loopback fixtures.
  - AT-9 now runs without the seam and still passes.
  - The deliberate cost: a sender's URL that is not a Blossom blob (NIP-96-style names, presigned URLs with a query) or that is on a LAN or Tailscale (100.64/10) server cannot be downloaded, even by a recipient on that network. The refusal says why. `nostrc-qi5e` records the NIP-96 question along with DNS rebinding. I agree this is the right default before G22.

### New non-blocking notes

1. **Low: IPv4-embedding IPv6 prefixes are not checked for a private embedded address** (`gh-blossom-client.c`, `address_private`).
   - **What's covered.** v4-mapped `::ffff:0:0/96` is unwrapped.
   - **What isn't.** `::/96` (IPv4-compatible), `::ffff:0:0:0/96` (SIIT), `64:ff9b::/96` and `64:ff9b:1::/48` (NAT64) are not.
   - **Scenario.** On an IPv6-only network with NAT64, common on mobile carriers, a message naming `https://[64:ff9b::c0a8:101]/<x>` asks the NAT64 gateway for 192.168.1.1. RFC 6052 forbids translating non-global addresses with the well-known prefix, but that depends on the gateway.
   - **Suggestion.** Unwrap the embedded IPv4 for those prefixes (or refuse the compatible, SIIT and NAT64 ranges outright), and add them to `download-address-policy`. This fits `nostrc-qi5e`.
2. **Nit: rows cached before `363b9d03` are keyed by `x` and no longer match anything.** No purge can reach them; only LRU eviction removes them. This affects only developer stores (no production caller has ever run), but `gh_store_media_prune(store, 0)` once at open, or a schema bump, would clear them.
3. **Nit: the identity is recomputed per row.**
   - **Where.** The put guard and `MEDIA_FORGET`'s survivor test compute `gh_media_file_id` over every stored kind-15 message: a JSON scan and a SHA-256 each, per put and per doomed row.
   - **Cost.** Fine at expected sizes (a purge touches few rows), but it is O(kind-15 messages).
   - **Suggestion.** A stored generated column or an index on the identity would make it O(log n) if attachments become common.

### Verification

- **Build.** `cmake -S . -B /tmp/w17rev -G Ninja -DBUILD_GROUNDHOG=ON && ninja -C /tmp/w17rev` (incremental, 627 steps) at `363b9d03` succeeded. There were no Groundhog compiler warnings and the source tree was left clean.
- **Tests.** `ctest --test-dir /tmp/w17rev -R 'groundhog-' -j6 --timeout 300`: **65/65 passed**, with four platform skips (`groundhog-launch`, `-store-key-keyring`, `-background-gui`, `-notifier-gui`). `groundhog-media-cache-binding`, `-media`, `-media-purge`, `-blossom`, `-relay-guard`, `-conversations` and `-privacy-e2e` are among them.
- **Scratch programs (`/tmp/w17scratch`).**
  - `repro_cache2.c`: the B1 repro, above.
  - `ssrf_probe.c`: 21 address forms and a positive control.
  - An AT-2 mutant.
  - The CI list cross-check.
- **Checks.** `git diff --check` for `58308eb8` and `363b9d03` (excluding `.beads`) is clean.
- **Not run.** A real NAT64 network, DNS-rebinding names (`nostrc-qi5e`), and the hosted CI (not pushed).

**APPROVED**

- **B1 is closed.** The cache is bound to the file's key and nonce in lookup, admission and every purge. My repro now misses, goes to the network and purges. The committed regression test covers it.
- **B2 is closed.** Every registered Groundhog test is built, required and (where display-free) sanitized.
- **Non-blocking #1–#5.** Fixed (#2, #3, #4) or filed (`nostrc-yp69`, `nostrc-4nur`).
- **The address policy.** It resists the IDN and non-canonical numeric bypasses I tried. The IPv4-embedding IPv6 prefixes (new note 1) and DNS rebinding (`nostrc-qi5e`) are follow-ups before G22 enables Download.
