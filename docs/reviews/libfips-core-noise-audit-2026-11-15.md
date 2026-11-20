# libfips-core Noise negative tests, constant-time review and zeroization audit — 2026-11-15

**Verdict: APPROVE-WITH-FIXES** for `libfips-core/src/noise/` (bead `nostrc-pwo3h.8`,
branch `feature/fips-noise-audit` on `origin/master` `105159c0d`). One medium API
hardening was applied; no critical or high finding. The differential oracle stays
100% green (`FipsDiffNoise` 83 checked / 0 skipped / 0 failed), so no wire behaviour
changed. This is the exit gate of the Noise epic and gates Tier A4 (rekey) and the
Tier B host.

## Scope and method

Reviewed every line of `src/noise/{aead.c,aead_primitive.h,ecdh.c,hkdf.h,symmetric.c,
handshake.c,cipher.c,replay.c,session.c}` and `include/fips/noise/*.h` (1,440 lines,
seven authors), plus the identity/secp context code they borrow
(`src/identity/{key,local,peer,mod}.c`, `internal.h`). Oracle: upstream
`core/src/noise/*.rs` at the `UPSTREAM.md` pin `6aa80504`, its `README.md`,
`docs/design/fips-security.md`, `docs/reference/security.md`, Noise rev 34 §5, §7.4–7.5,
§11, and libsodium's `sodium_memzero`/`crypto_verify_*` semantics. Adversarial, not a
port: the positive space is the differential harness; this report owns the negative
space. Tools: clang 21 (Apple) ASan/UBSan `-Werror`, clang source coverage
(`-fprofile-instr-generate -fcoverage-mapping`, Xcode `llvm-cov`), libFuzzer built from
a sparse `compiler-rt/lib/fuzzer` checkout behind a disposable clang resource-dir
overlay, and a dudect-style timing probe.

## Findings

Severity: critical / high / medium / low / info. "Upstream" is the pinned Rust.

| ID | Severity | Location | Issue | Upstream behaviour | Resolution |
| --- | --- | --- | --- | --- | --- |
| N-1 | **Medium** | `src/noise/session.c:80-94` (pre-fix) | `fips_noise_session_{send,recv}_cipher_clone` copied the whole `fips_cipher_state`, counter included. A worker that called `fips_cipher_state_seal` or `take_counter` on its clone would reissue counters 0, 1, … that the session had already issued under the same key: ChaCha20-Poly1305 nonce reuse (keystream reuse, Poly1305 key recovery, forgery). Only a header contract prevented it; `test_session.c` even pinned the hazardous counter semantics ("Do NOT seal these locally reserved counters"). | `CipherState::cipher_clone()` returns a bare `AeadKey`; the type makes auto-seal impossible, the worker can only `seal(key, counter, …)` with a dispatcher-reserved counter. | **Fixed.** Clones copy the key only and pin the counter at the refused ceiling (`UINT64_MAX`), so auto-seal and `take_counter` on a clone return `NonceOverflow`; `seal_with_counter`/explicit-counter `open` are unchanged. `session.h` contract rewritten; `test_session.c` updated; regression `clone_counters()` in `test_noise_negative.c`. `fuzz_session.c` and `tests/diff` already used explicit counters and are unaffected. |
| N-2 | Info | `src/noise/cipher.c:64-67`, `aead.c:44` | Explicit-counter open refuses `len > 65535` with `MessageTooLarge` (and insufficient `out_cap` as `MessageTooLarge`). | `CipherState::decrypt_with_counter[_and_aad]` has no upper bound: an oversized input is authenticated and fails as `DecryptionFailed` (no conforming peer seals more than 65535 bytes). | Accepted divergence, stricter than upstream; the variant differs only for input no conforming peer produces (and that the u16 FMP/FSP framing and UDP datagram limits cannot carry), no wire change. Documented in `handshake.h`/`cipher.h`; pinned by `transport_length_matrix()`. Upstream note for `nostrc-f66sr.6`. |
| N-3 | Info | `src/noise/cipher.c:59-71`, `aead.c:40-55` | `fips_cipher_state_open` and the raw AEAD accept counter `UINT64_MAX`; only the replay window (`check`) and the senders (`take_counter`, `seal_with_counter`) refuse it. | Same: `decrypt_with_counter` does not refuse; `seal()` does not either (C `seal_with_counter` is stricter). | Parity; pinned by `nonce_overflow()` (ceiling open succeeds at the cipher layer, `ReplayDetected` at the session layer). No fix. |
| N-4 | Info (upstream) | `src/noise/replay.c:16-18` | `fips_replay_window_mark` re-checks and returns `false` for too-old, seen or `MAX` counters, leaving the window byte-identical. | `ReplayWindow::accept` trusts the caller: for `counter <= highest` with `diff >= 8192` it indexes `bitmap[diff / 64]` out of bounds and panics; reachable through `NoiseSession::accept_replay` misuse. | C is strictly safer; keep. Upstream flaw note for `nostrc-f66sr.6`. |
| N-5 | Info | `aead.c:30,44`, `cipher.c:27,64` | `ad_len > ULLONG_MAX` is a tautology on every supported ABI (`size_t` ≤ `unsigned long long`): dead guard around the libsodium cast. | n/a | Harmless; left as documentation of the cast. No fix (churn). |
| N-6 | Info | `aead_primitive.h:33` | On tag failure the wrapper zeroes `len - 16` output bytes; libsodium's `crypto_aead_chacha20poly1305_ietf_decrypt_detached` already `memset(m, 0, mlen)`s on `crypto_verify_16` mismatch. | ring/RustCrypto leave "unspecified" contents. | Redundant by design: keeps the `aead.h` no-plaintext-on-failure contract independent of libsodium internals. Keep. (2026-11-19: now in `aead_primitive.c`, and required on the OpenSSL path, which decrypts before verifying: B-3.) |
| N-7 | Info (parity) | `src/noise/symmetric.c:76-85` | A destroyed (all-zero) `fips_symmetric_state` has `has_key == false`, so `encrypt_and_hash`/`decrypt_and_hash` pass bytes through unencrypted. Unreachable from the handshake: `ready()` refuses a destroyed state before any symmetric call, and every handshake encrypt follows a `mix_key`. | Identical: `CipherState::empty()` passes plaintext through. | Pinned explicitly in `after_destroy()` so the behaviour is visible. Follow-up F-3 (optional test-hooks poison). |
| N-8 | Info | `src/noise/handshake.c:131,140,155,162,185-187,206-208,259-261,275-277` | Offsets/lengths 33, 49, 82, 24 are literals; the `_Static_assert`s pin totals against `framing.h` but not intermediate offsets. | n/a | Readability only, wire-pinned file; deferred (F-4). |
| N-9 | Info | `handshake.c` read paths | `InvalidPublicKey` (0 DH) and `DecryptionFailed` (1–2 DH) are timing-distinguishable to a network attacker, but only reveal whether the attacker's own ephemeral bytes are a curve point — public information. No secret-dependent branch exists before the AEAD; the AEAD tag check is `crypto_verify_16`. | Identical structure. | Documented; cost model pinned per message and region by `tamper_matrix()`. |
| N-10 | Info | `src/identity/mod.c:28-32`, `src/noise/ecdh.c` | Context blinding (`secp256k1_context_randomize` with 32 host bytes, no unblinded fallback) protects `ec_pubkey_create` (ephemeral keygen) and Schnorr signing. `secp256k1_ecdh` runs `ecmult_const`, a constant-time ladder that does not use the context's blinding — by libsecp256k1 design. | Rust `secp256k1` `shared_secret_point` is the same ladder; its global context is randomized once. | Verdict: blinding applied where libsecp256k1 applies it; ECDH relies on `ecmult_const`. No fix. |
| N-11 | Info (parity) | `handshake.c:196-213`, `read_xk_message_1` | A forged IK msg1 with a valid ephemeral costs the responder one DH (es) before the static fails to open; any XK msg1 with a valid point costs one DH and is accepted (no authentication at all). Unauthenticated DoS surface. | Identical; upstream `security.md` records that only handshake msg1 is unauthenticated. | Documented and pinned (XK msg1 allow-list below). Host rate limiting is the mitigation (Tier B). |
| N-12 | Info | `src/noise/session.c:53` | `fips_noise_session_open` ignores the result of the post-authentication `mark` (`(void)`). Under the single-writer contract it cannot fail; a contract violation (mutation between check and mark) would accept a frame without recording it. | Same shape (`accept` returns nothing). | Contract documented in `session.h`: the single writer must own the whole check → open → mark and fork → validate → commit sequences, including rekey/reset/destroy; no fix. |
| N-13 | Info | `handshake.c:343`, `finish` | `remote_static` returned to the initiator is the *configured* bytes, so an npub-derived 0x03 prefix is preserved while the responder's own serialization is 0x02; hosts must compare x-only. | Identical (`remote_static` is the configured `PublicKey`). | Pinned (`wrong_static()` parity case, `test_handshake.c:278`). |
| N-14 | Info (upstream design) | `symmetric.c:100,123` (empty AAD), `handshake.h:103-105` | Pinned FIPS v1 passes an EMPTY associated-data field to the handshake AEAD where Noise `EncryptAndHash` uses `h`. Anything mixed only into `h` (pre-message parity, a future prologue or negotiated parameter) is therefore not authenticated and `h` is not a channel binding; the accepted 02↔03 ephemeral parity flips demonstrate the malleability. | Identical and documented upstream (`docs/reference/security.md`, "Deviation: Empty Associated Data"; `handshake.rs:53-57`). | Must not be "fixed" in C: restoring transcript binding is a versioned interoperability change. Documented in `handshake.h`; Tier B must not build an exporter/channel binding on `h`. Note for `nostrc-f66sr.6`. |

A second-model review of the findings and the fix (code inlined, no worktree access) agreed the fix is sound, recalibrated N-2/N-3 from low to info, and raised the transcript-binding and single-writer caveats now recorded as N-14 and in N-12.

No `memcmp`/byte-equality on any tag, key, hash or public key exists in `src/noise`
(grep + read); authentication is libsodium's `crypto_verify_16` inside the AEAD. Every
secret temporary is wiped on every path: HMAC states, PRK and T blocks (`hkdf.h`), the
64-byte HKDF output in `mix_key`/`split`, the DH output and exported static scalar in
`mix_dh`, the ECDH `x`/hash/parsed point, decrypted static and epoch, `next_h`, both split
keys in `finish`, every handshake message buffer, every fork copy (`fork_read` destroys on
every path), and whole structs on `destroy`/`clear_slot`/`take_cleared`.

## Audit checklist

| Item | Verdict | Evidence |
| --- | --- | --- |
| (a) constant-time comparisons | **PASS** | No `memcmp` in `src/noise`; AEAD open → `crypto_aead_chacha20poly1305_ietf_decrypt` → `crypto_verify_16` (OpenSSL backend from 2026-11-19, payloads ≥ 256 B: the provider's `CRYPTO_memcmp`, addendum B-1). The only equalities are pointer aliasing guards (`k1 == k2`, `slot == out`) and the public `secp256k1_ec_pubkey_parse`. `ct_probe` below. |
| (b) zeroization on every path | **PASS** | Read above; `zeroization()` proves every secret range (ck, h, cipher key, ephemeral secret, es, remote keys, epoch, borrowed pointers) is nonzero live and zero after destroy, for a mid-handshake fork, a discarded fork, consumed `finish` keys, `take_cleared`/`clear_slot`, AEAD key and SymmetricState. `sodium_memzero` is an external libsodium call (not elidable without LTO) and the post-destroy zero checks pass under `-O0`/ASan and the `-O2` probe build. |
| (c) IK one DH on failed msg2; invalid pubkey costs zero DH | **PASS** | `tamper_matrix()` pins the DH cost of every mutation: invalid point 0 DH on every ephemeral-carrying message; valid-point-wrong-x 1 DH; IK msg2 auth failure exactly 1 DH (es cached); IK msg1 1 DH for the static region, 2 for the epoch region; XK msg3 0 DH for the static region, 1 for the epoch. |
| (d) nonce/counter | **PASS after N-1** | `nonce_overflow()`: refusal at `2^64-1` before sealing in cipher, session and SymmetricState (both directions, state unchanged) and mapped through the handshake (`NonceOverflow` on XK msg3 write and fork read, originals untouched). Clones cannot issue counters (N-1). `open_unmarked` takes `const` and only calls `fips_cipher_state_open`. |
| (e) replay window | **PASS** | `replay_matrix()`: 18 highest-counter scenarios (0, 1, 63/64/65, 127/128, 8190–8193, 16383/16384, 2^32, 2^63, MAX-2, MAX-1) × probes at ±1, ±2, ±63/64/65, ±8190–8193 and +2^40 against an independent set oracle; failed marks leave every byte unchanged; far-future reset, shift by exactly 8192, counter 0 accepted once and aged out at 8192; after a failed open the counter stays acceptable (`frame_matrix()`, matching the vectors' `failed_counter_still_acceptable`). Combined word+bit shift verified equivalent to upstream's two-pass shift; no shift by 64 (the carry `>> (64u - bits)` runs only when `bits != 0`, `replay.c:31-32`). |
| (f) length handling | **PASS** | Every handshake read requires the exact length: `length_matrix()` tries 0..65536 and `SIZE_MAX` for all five message types, raw and fork, with no DH, no entropy and byte-identical state. `MAX_MESSAGE_SIZE` enforced on seal (`len > 65519`) and open (`len > 65535`, N-2); `transport_length_matrix()` opens every length 0..65536 of a maximum frame. No overflow: every `len + 16` follows a `len > MAX - 16` check and every `len - 16` follows `len < 16`; `framing.h` constants are plain ints compared under `_Static_assert`, no `uint16_t` arithmetic in `src/noise`. |
| (g) pubkey parsing | **PASS** | Invalid prefixes 0x00/0x01/0x04–0x07/0x82/0xff, all-zero and all-0xff remote statics are `InvalidPublicKey` with the output untouched; odd parity accepted and the pre-message is forced to 0x02 (`wrong_static()`: equal `h` for 0x02/0x03 remote); decrypted statics are parsed after the AEAD, as upstream. Blinding: N-10. |
| (h) error-variant parity | **PASS** | All eleven `NoiseError` variants present; `WrongState` precedes length checks, which precede `InvalidPublicKey`/`DecryptionFailed`, as upstream. Divergences are all stricter: N-2 (`MessageTooLarge` on oversize open/capacity), N-3 (`seal_with_counter` refuses MAX), `WrongState` where upstream `expect`s. Network-indistinguishability: every read failure is a silent drop; timing separates only public point validity (N-9). |
| (i) API misuse hardening | **PASS** | `after_destroy()`: every writer/reader/fork/finish on a destroyed handshake refuses (`WrongState`/`HandshakeNotComplete`) with no entropy, DH or output; double `finish`; `send == recv`; destroyed cipher and session (`WrongState`, window untouched, clones refused); unknown progress names "Invalid". Detection relies on the zeroed state failing `ready()`; no separate poison is needed. |
| (j) `_Static_assert`s vs `framing.h` | **PASS** | `handshake.c:6-14` pins TAG, PUBKEY, EPOCH, EPOCH_ENCRYPTED, IK msg1/msg2 and XK msg1/2/3 (XK as framing expressions, since `framing.rs` defines none); `aead_primitive.h` (now `aead_primitive.c`, addendum B-1) pins key/nonce/tag against libsodium; `symmetric.c` pins protocol-name sizes, SHA-256 size and status pass-through. |

## Negative tests (`libfips-core/tests/test_noise_negative.c`, `FipsCoreNoiseNegativeTest`)

Compiled with `FIPS_TEST_HOOKS` (handshake, ECDH and cipher sources) and the test's own
libsecp256k1 context as an independent point-validity oracle. Deterministic fixtures
(seeded entropy 0x494b IK, 0x584b XK); every accepted mutation must match one explicit
allow-list rule, otherwise the exact variant and DH count are asserted:

| Message | Mutations (offset × {xor 1, xor 0x80, 0x00, 0xff}) | identity | parity (02↔03) | xk1 unauthenticated | InvalidPublicKey (0 DH) | DecryptionFailed, valid e, wrong x (1 DH) | DecryptionFailed, AEAD |
| --- | --- | --- | --- | --- | --- | --- | --- |
| IK msg1 (106) | 424 | 0 | 1 | — | 64 | 67 | 292 |
| IK msg2 (57) | 228 | 0 | 1 | — | 58 | 73 | 96 |
| XK msg1 (33) | 132 | 0 | — | **71** | 61 | — | — |
| XK msg2 (57) | 228 | 1 | 1 | — | 63 | 67 | 96 |
| XK msg3 (73) | 292 | 0 | — | — | — | — | 292 |

The 71 accepted XK msg1 mutations are exactly the bytes the oracle parses as a point; the
count is pinned in the test and was cross-checked by an independent pure-Python Euler's
criterion over the printed fixture bytes (71 valid, 61 invalid, 0 identity). Each accepted
XK msg1 is followed up: the tampered responder's msg2 authenticates at the real initiator
only for the parity-only mutation (offset 0), fails `DecryptionFailed` after 1 DH for all
70 x-changing ones. Every raw read runs on a copy with the snapshot proven byte-identical;
every fork read proves the destination sentinel untouched on failure.

Also covered: writer capacities 0..n-1 (`MessageTooLarge`, no entropy/DH/output) and
n/n+1/`SIZE_MAX`; transport frames at 0/1/15/16/17/64/1000 bytes × {no AD, 3-byte AD} with
every ciphertext, tag and AD byte × 4 mutations, counter +1/+2/+8191/+8192/+2^40 and the
ceiling (9,927 and 9,929 rejections, plaintext range zeroed exactly, `out_len`/window
untouched, counter still acceptable, genuine frame then opens once); wrong direction key;
every transport length 0..65536 on open and 0..4096 ∪ every 997th ∪ boundary on seal;
nonce overflow at all layers; wrong remote static for both patterns and both roles;
forced DH failure (`Secp256k1`, counted, originals untouched); misuse after destroy;
zeroization; clone counter hardening. Runtime 0.9 s under ASan/UBSan.

## Coverage of `src/noise/` (clang source coverage, all `^Fips` tests)

| | Lines | Regions | Branches | Functions |
| --- | --- | --- | --- | --- |
| Before this task | 99.03% (610/616) | 95.95% | 85.19% | 100% |
| After (`test_noise_negative.c` + fix) | **99.35% (615/619)** | **96.76%** | **87.83%** | 100% |

Remaining uncovered lines are exactly the four `sodium_init()`-failure mappings that cannot
execute once libsodium is initialized: `aead.c:35` (`FIPS_AEAD_ERR_CRYPTO` from seal),
`handshake.c:74,75,77` (`ERR_RANGE`/`ERR_CRYPTO`/default cases of `symmetric_status`; the
handshake uses fixed lengths so `ERR_RANGE` is unreachable too). The `NonceOverflow`
mapping and the `"Invalid"` progress name were previously uncovered and are now exercised.

## Constant-time check (`libfips-core/tests/tools/ct_probe.c`, target `fips_core_ct_probe`)

Method: dudect-style fixed-vs-random classes, interleaved pseudo-randomly, 16 opens per
measurement (lifts samples above `clock_gettime` quantisation), slowest 1% of each class
cropped, Welch's t-test, |t| ≥ 4.5 = distinguishable. Built standalone at `-O2` against
`src/noise/aead.c` + libsodium 1.0.22 (no sanitizer/coverage overhead), Apple M4 Max,
100,000 measurements per experiment, 1024-byte plaintext, 16-byte AD:

| Experiment (class 0 vs class 1) | mean ns / 16 opens | t | Result |
| --- | --- | --- | --- |
| ciphertext byte 0 vs random ciphertext byte | 7938 / 7939 | −0.06 | indistinguishable |
| tag byte 0 vs random tag byte | 7985 / 7976 | +1.66 | indistinguishable |
| first ciphertext byte vs last tag byte | 8116 / 8122 | −1.12 | indistinguishable |
| AD byte 0 vs random AD byte | 7865 / 7867 | −0.85 | indistinguishable |
| wrong counter vs last tag byte | 7822 / 7827 | −1.74 | indistinguishable |
| ciphertext byte 0 vs random byte anywhere | 8048 / 8053 | −1.10 | indistinguishable |
| **control**: accept vs reject, same length | 25223 / 8252 | **+1373** | detected (probe has power) |

Result: AEAD-open failure time does not depend on which byte was tampered; the only
observable is accept-vs-reject (one ChaCha20 pass), which the response already reveals.
Static argument for the rest of the stack: handshake reads branch only on public point
validity and on the AEAD result (N-9); `ecmult_const` is constant time (N-10). The probe is
built in the CI closure but deliberately not a ctest (timing is machine dependent).

## Verification

| Check | Result |
| --- | --- |
| Configure as FIPS CI: Debug, `GO_WARNINGS_AS_ERRORS=ON`, `GO_ENABLE_ASAN=ON`, `GO_ENABLE_UBSAN=ON`, `BUILD_FIPS=ON`, other components off (clang 21, macOS arm64) | PASS, no warnings |
| `ctest -R '^Fips'` (40 tests incl. new `FipsCoreNoiseNegativeTest`, `FipsdSkeletonTest`, `FipsCoreSkeletonTest`) under `ASAN_OPTIONS=detect_leaks=0:strict_string_checks=1:halt_on_error=1`, `UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1` | **40/40 PASS** |
| `FipsDiffNoise` | **83 checked, 0 skipped, 0 failed** |
| `fips_fuzz_aead` 30 s, seed corpus copy, ASan/UBSan | 873,143 execs, 141 new units, 0 findings, exit 0 |
| `fips_fuzz_handshake` 30 s | 3,116,612 execs, 13 new units, 0 findings, exit 0 |
| `fips_fuzz_session` 30 s | 2,414,510 execs, 788 new units, 0 findings, exit 0 |
| `fips_fuzz_identity` 30 s | 10,394,720 execs, 382 new units, 0 findings, exit 0 |
| `fips_fuzz_session` / `fips_fuzz_handshake` 30 s each, rerun on the final committed `session.c` | 2,931,976 / 3,097,240 execs, 0 findings, exit 0 |
| libFuzzer runtime | built from a sparse `llvm-project/compiler-rt/lib/fuzzer` checkout (`build.sh`), exposed as `libclang_rt.fuzzer_osx.a` through a disposable `-resource-dir` overlay of symlinks to Apple's runtimes; deleted afterwards |
| `scripts/check-unsequenced-args.py --root .`, `check-gui-test-gdk-frame.py`, `check-linux-ci-packages.py`, `git diff --check` | PASS |
| Build/runtime scratch (`/tmp/nostrc-noise-audit`), submodule (`git submodule deinit -f --all`) | removed after commit |

## Addendum 2026-11-19 — OpenSSL AEAD backend (nostrc-u19cz.2)

A CPU profile of fipsd under loopback load put ChaCha20-Poly1305 at ~30% of the busy
thread, in libsodium's portable reference ChaCha20 (libsodium has no NEON ChaCha20 on
arm64). `src/noise/aead_primitive.c` now has a configure-time backend,
`-DFIPS_AEAD_BACKEND=openssl|sodium` (default `openssl`). The public AEAD, CipherState,
SymmetricState and session APIs, their bounds checks and the Noise counter/Fork
semantics are untouched: only the private full-nonce seal/open changed.

| ID | Severity | Location | Issue | Resolution |
| --- | --- | --- | --- | --- |
| B-1 | Info (design) | `aead_primitive.c` | Backend and residue. `openssl`: plaintext ≥ 256 B runs on OpenSSL EVP (≥ 3.0), shorter on libsodium; `sodium`: libsodium throughout (the pre-change path). The EVP cipher is fetched once from a private `OSSL_LIB_CTX` holding only the built-in default provider (a host `openssl.cnf` with `fips=yes` default properties cannot remove ChaCha20). `fips_aead_key`/`fips_cipher_state` stay pointer-free value types; one `EVP_CIPHER_CTX` per thread lives in a pthread key (destructor at thread exit, `atexit` for the exiting thread, winpthreads on MinGW; hosts join AEAD-using threads before `exit`/`OPENSSL_cleanup`) and is re-keyed per op, so the hot path allocates nothing. That cached context outlives the op: OpenSSL's provider keeps the ChaCha key words and a 64-byte keystream buffer (block 0, the Poly1305 one-time key, when the length is a multiple of 64; otherwise the last partial block, equivalent to that plaintext given the ciphertext). `Poly1305_Final` cleanses the MAC state itself. | After every op the context is re-keyed with an all-zero key and nonce and fed one AAD byte, which overwrites the key words and the keystream buffer with values derived from the public zero key; if that fails the context is freed (`OPENSSL_clear_free`) and recreated. `FipsCoreAeadResidueTest` owns OpenSSL's allocator and scans every live OpenSSL block for any 8-byte window of the key, keystream block 0 or the message keystream after seal and after (forged) open, lengths 0–65519; a positive control must see an unscrubbed context. Mutation: without the scrub, or with a key-only re-init, the test fails. libsodium leaves no heap state (stack `sodium_memzero`), so the residue bound now matches the pre-change path. |
| B-2 | Info | `aead_primitive.c`, open | OpenSSL 4.x raises `PROV_R_BAD_DECRYPT` on a tag mismatch (3.x does not). Each forged packet would leave an error-queue entry on the thread, visible to other OpenSSL users there (e.g. TLS `SSL_get_error`). | `ERR_set_mark` before `EVP_DecryptFinal_ex`, `ERR_pop_to_mark` on mismatch (≈22 ns per ≥ 256 B open). `test_aead.c` pins an empty queue after forgeries and that a host's pending error survives. |
| B-3 | Info | `aead_primitive.c` | Failure semantics. OpenSSL writes plaintext before `Final` checks the tag (libsodium verifies first). | The primitive zeroes the `len - 16` plaintext bytes on every open failure, as the N-6 wrapper already did; nothing else is written (`final` output goes to a local block). A backend error (not a tag mismatch) on open now maps to `FIPS_AEAD_ERR_CRYPTO` instead of `ERR_AUTH`; it is unreachable unless OpenSSL fails to allocate. A failed seal zeroes `len + 16` output bytes so no partial ciphertext under a reserved nonce escapes. |

Measured alternatives (Apple M4 Max, OpenSSL 4.1.0, libsodium 1.0.22, `-O2`, best of 5,
ns/op seal, AD 16 B; machine shared with other benchmarks):

| Variant | 64 B | 1400 B | Notes |
| --- | --- | --- | --- |
| libsodium (pre-change) | 202 | 2100 | no residue |
| EVP, per-op `EVP_CIPHER_CTX_new`/`free` | 340 | 1006 | allocates per op |
| EVP, cached per-thread ctx, no scrub | 216 | 886–926 | key + keystream linger: rejected |
| EVP, cached ctx, key-only zero re-init | 255 | 915 | keystream lingers (residue test fails): rejected |
| EVP, cached ctx, `EVP_CIPHER_CTX_reset` per op | 298 | 977 | clean, but allocates per op |
| **EVP, cached ctx, zero-key scrub (chosen)** | 318–338 | 988–990 | clean, no per-op allocation |

Seal crossover (scrubbed EVP vs libsodium) is 224–256 B (256 B: 416 vs 466 ns; 512 B: 502
vs 842 ns), hence the 256 B split, which keeps 64 B at libsodium speed. The split branches
on the public length only.

`fips_bench_noise_aead` (Release, medians of 3 runs, ns): `session/seal/64` 204 → 213,
`session/open/64` 301 → 303, `worker/seal/64` 210 → 217, `worker/open/64` 302 → 304 (all
libsodium in both builds; within noise), `session/seal/1400` 2229 → 1045 (2.13×),
`session/open/1400` 2310 → 1173 (1.97×), `worker/seal/1400` 2180 → 1027 (2.12×),
`worker/open/1400` 2370 → 1181 (2.01×).

Constant time: the OpenSSL tag check is `CRYPTO_memcmp`. `ct_probe` (Release, 1024-byte
plaintext, so the OpenSSL path, 100,000 measurements): every tamper class indistinguishable
(t = +1.04, −2.68, +0.40, −1.06, −0.06, −0.88), control detected (t = −328). Unlike
libsodium, a reject is now *slower* than an accept (13,248 vs 11,992 ns per 16 opens,
≈80 ns per forgery: OpenSSL 4's error raise plus the B-2 mark/pop); it depends only on
accept-vs-reject, which the response already reveals, and bounds a forgery's cost.

Verification: `FipsDiffNoise` 83 checked / 0 skipped / 0 failed and `FipsDiffRekey*` green
with both backends; `test_aead.c` adds a byte-for-byte comparison against libsodium for
every length 0..300 and 511–65519 edges, nine AAD lengths, in-place and forged inputs,
and three rounds of 8 concurrent threads; `fips_fuzz_aead`/`handshake`/`session` 30 s each (Homebrew
clang 23 libFuzzer) with no findings; fipsd two-node and C↔Rust UDP/TCP interop green.

## Addendum 2026-11-20 — count-bounded rekey output wipe (nostrc-02p6s)

`fips_rekey_outputs` is 57,992 bytes (48 × 1,208-byte items plus `count`), and
`fips_rekey_outputs_clear` used to `sodium_memzero` all of it whatever `count` said.
`fips_session_execute` clears once per executed step, so every authenticated arrival
paid two 58 KB wipes (link and session `AuthenticatedFrame`): ~10% of the receiving
dispatcher's samples as `__bzero` under `hs_session_datagram_received`. Items carry key
material (`Delegate`: a receive-key copy) and plaintext offsets, so the "wipe at end of
tenure" rule (L4, `nostrc-f66sr.10`) still applies to every item that ever held data.

| ID | Severity | Location | Issue | Resolution |
| --- | --- | --- | --- | --- |
| R-1 | Info (performance, wipe semantics preserved) | `src/proto/rekey/mod.c`, `fips_rekey_outputs_clear` | Full-struct wipe per step although only `items[0, count)` can hold data. | Clear now runs the existing Delegate/Release releases over `items[0, count)`, then `sodium_memzero`s exactly those items and `count`. A `count` above 48 (a broken invariant) asserts in debug builds and falls back to the full-struct wipe. Debug builds also assert the slot at `count` is still zero (a write one past the end). |

Writer audit (why `items[count, 48)` is always zero for a list that started as `{0}`,
the documented contract in `machine.h`):

- **Appends.** `rk_output` (`mod.c`) is the only appender: it asserts capacity, takes
  `items[count++]` and `memset`s that item before filling it. `rk_send`, `rk_reject`,
  `rk_abandoned`, `rk_prompt` and every `rk_output` caller in `machine.c`, `cycle.c`
  and `epoch.c` write through the returned pointer.
- **In-place edits.** `rk_cycle_end` (`cycle.c`) writes `items[count - 1].close`;
  `poll`'s dedup loop and the compaction in `fips_rekey_step` read below `count`.
- **Compaction.** `fips_rekey_step` drops demoted `Prompt`s by moving survivors down
  (`items[write] = items[i]`, `write <= i`). Every slot below the final `write` is
  overwritten with a live item, so stale copies can exist only in `[write, count)`,
  which it already wiped (`machine.c`, `sodium_memzero(items + write, …)`) before
  lowering `count`.
- **Hosts.** `fips_session_execute` and its callbacks get item pointers below
  `count` (`send`, `effect` and `prompt` all take `const`; only its own Release
  handling releases a lease in place). fipsd (`handlers/rekey.c`, `session.c`,
  `probe.c`, `dataplane/dispatch.c`) only reads items below `count` and releases
  their leases in place (`fips_rekey_index_lease_release`); none writes `count` or
  items directly, copies a whole list, or declares a list on the stack. Its three lists
  live in the calloc'd handler struct, which `fipsd_node_handlers_destroy` wipes
  whole. libfips-core tests, the fuzzer and `test_peer_link.c` start lists as `{0}`
  or inside a zeroed fake node.
- **No other decrement.** Nothing lowers `count` except compaction (above) and clear.

Wiping `items[0, count)` and `count` therefore leaves the struct byte-identical to the
old full wipe (`{0}`), on every path. `machine.h` now states the rule hosts must keep
(act on `items[0, count)` in place, never write past `count`). A list that was never
zeroed is a caller contract violation, as before: the old clear already walked its
garbage `count`.

Guards: `outputs_clear_wipes_whole_list` (`test_rekey.c`) fills a list to every level
0–48 with every output kind and non-zero payload bytes, clears, and requires the
*whole* struct to read zero; the fake rekey node (`tests/fakes/rekey.c`,
`rk_test_clear`) now asserts the whole struct is zero after every clear, so every
machine-driven FMP/FSP/crossing/next case (93 cases) re-proves the invariant.
Mutation: making `rk_cycle_end` write `items[count]` fails `FipsCoreRekeyTest` at the
debug assertion.

Cost (Apple M4 Max, Release, hot loop): `fips_rekey_outputs_clear` on a one-item list
≈300–330 ns → ≈13 ns; in fipsd the wipe also evicted 58 KB of L1 per call. Under 100
Mbit/s loopback, `sample` shows the `__bzero` frames under `fips_session_execute` gone.
Remaining step-scratch wipes (`fips_rekey_step`'s ≈3.2 KB `change`, zeroed on entry and
wiped on exit) are an order of magnitude smaller and unchanged; see the follow-up below.

## Version assessment

`libfips-core` stays **0.1.0 Unreleased**: the N-1 clone semantics change is a
PATCH-class correction folded into unreleased 0.1.0 (no published release or tag);
the negative test, probe and this report are test/documentation-only. No other component
is affected. Recorded in `VERSION_MANIFEST.md`.

## Follow-ups (beads-ready)

- **F-1 (for `nostrc-f66sr.6`, upstream)** — `core/src/noise/replay.rs` `ReplayWindow::accept` indexes `bitmap[diff/64]` without bounding `diff < REPLAY_WINDOW_SIZE`; a caller marking a counter older than the window panics. C refuses instead (N-4).
- **F-2 (for `nostrc-f66sr.6`, upstream)** — `CipherState::decrypt_with_counter[_and_aad]` has no `MAX_MESSAGE_SIZE` bound, unlike `encrypt`; C refuses oversize input before authenticating (N-2).
- **F-3 (libfips-core, optional, low)** — add a `FIPS_TEST_HOOKS`-only poison so a destroyed `fips_symmetric_state` is detected as misuse instead of acting as upstream's pass-through empty cipher (N-7).
- **F-4 (libfips-core, info)** — replace the literal offsets 33/49/82/24 in `handshake.c` with named expressions of `FIPS_NOISE_PUBKEY_SIZE`/`TAG_SIZE`/`EPOCH_ENCRYPTED_SIZE` (N-8).
- **F-5 (Tier A4/B design note)** — worker clones now carry no issuable counter: the dispatcher reserves with `fips_noise_session_take_send_counter` and workers seal with `fips_cipher_state_seal_with_counter`; record this in the rekey/host design when porting `core/src/proto/rekey`.
- **F-7 (libfips-core, low, nostrc-02p6s)** — `fips_rekey_step` zero-initialises and wipes its ≈3.2 KB `change` scratch (input, read, candidate, wire, proof) on every step, plus `completed_clear`/`hs_clear` on fields an `AuthenticatedFrame` judge never touches. Bounding those wipes by `change.kind` needs the same per-kind writer audit as R-1.
- **F-6 (CI, optional)** — run `fips_core_ct_probe` on a quiet dedicated runner as a non-gating nightly, since wall-clock t-tests are unsuitable for shared PR runners.
