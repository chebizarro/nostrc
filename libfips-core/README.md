# libfips-core

The C11, sans-IO protocol core of FIPS. This component inherits the boundary
and rules of upstream FIPS `core/README.md`, `core/src/README.md` and
`core/src/lib.rs`: the same protocol computation should work unchanged in a
daemon, an application or an embedded host. This is an incremental C port,
not a Rust FFI wrapper or a complete working protocol implementation.

## Boundary and rules

- **No I/O or OS calls.** No sockets, devices, files, configuration parsing,
  logging, signal handling or platform-specific branches. Those belong to
  `fipsd/` or another host.
- **No threads or runtime dependency.** Calls are synchronous and return
  promptly. Return actions (bytes to send, deadlines to schedule, events or
  errors), rather than waiting, polling or starting background work.
- **Time comes from the host.** Accept timestamps as inputs; never read a
  clock or run a timer. Tests use synthetic timestamps, not sleeps.
- **Entropy comes from the host.** Never read an OS, global or thread-local
  RNG. Keys, nonces and context-blinding seeds must use host-supplied
  cryptographically secure bytes. Merely linking a crypto library does not
  authorize its random-source APIs or implicit initialization side effects.
- **Pass state explicitly.** Avoid mutable global state, locks and platform
  atomics that exclude smaller targets. Hashes keyed by peer-controlled input
  must be bounded and, when appropriate, keyed with host-supplied entropy.
- **Peer input is untrusted.** Check lengths, arithmetic and buffer bounds;
  return errors instead of aborting. Bound allocations by local policy, never
  solely by a peer's length field. Wipe secrets on all error and release paths.
- **Keep dependencies narrow.** Only libsecp256k1, libsodium and OpenSSL
  `Crypto` (EVP SHA-256/HKDF) are wired here. No libgo, libfiber, libnostr,
  GLib or host integrations. Current dependencies target hosted builds;
  bare-metal portability is an architectural goal, not yet verified.

## Layout and build contract

- `include/fips/core.h`: public version function; generated export and version
  headers are installed alongside it.
- `include/fips/host.h`, `src/host.c`: host interfaces and time arithmetic;
  caller-owned time and entropy.
- `include/fips/proto/`, `src/proto/`: protocol errors and bounds-checked codec.
- `tests/`: linked version, host-interface, codec and byte-compatible fake tests.
- `include/fips/host.h`, `src/host.c`: host interfaces and time arithmetic.
- `include/fips/proto/rate_limit.h`, `src/proto/rate_limit.c`: bounded
  per-address interval gate and saturating millisecond backoff.
- `include/fips/node/`, `src/node/`: handshake/session-setup/lookup-signing
  budgets and dual-tier stats history, with explicit instants and caller-owned
  bounded storage. Stats queries expose JSON-neutral Series metadata and a
  walk hook that marks non-finite samples as null.
- `include/fips/cache/coord_cache.h`, `src/cache/`: the node-address to
  tree-coordinate cache (hint/verified/stated writes, funded placement, LRU
  eviction that spares live-verified entries, probation segment, TTLs) over
  caller-owned slots and index words, with instants converted against an init
  epoch. Coordinates are held inline pending the proto coord codec.
- `tests/`: linked version, host-interface and byte-compatible fake tests.
- `libfips-core.pc.in`: installed pkg-config contract, including private crypto
  requirements for static consumers.

Enable with the top-level `BUILD_FIPS` option (default ON). The `fips_core`
target follows `BUILD_SHARED_LIBS`: OFF produces an archive, ON a versioned
shared library. Install headers under `include/fips` and query pkg-config as
`libfips-core`. `CMakeLists.txt` owns the component's initial version, 0.1.0.
Warning flags honor nostrc's shared `GO_WARNINGS_AS_ERRORS` option; sanitizer
flags use its shared `GO_ENABLE_*` helpers.

Future protocol tests must drive real state machines with fixed entropy and
synthetic time, checking returned actions and upstream differential vectors.
Higher-level protocol modules are left to later tasks. Upstream provenance
is maintained separately in `UPSTREAM.md`.
The remaining wire-protocol state machines are left to later tasks. Upstream
provenance is maintained separately in `UPSTREAM.md`.

## Host interfaces and ownership

`fips/host.h` mirrors the pinned Rust `EntropySource`, `Instant` and `Clock`
traits. The host reads `fips_clock.now(ctx)` and passes copyable `fips_instant`
values into core calls; the core never reads a clock or RNG itself. An instant
holds two opaque words and an immutable arithmetic vtable (comparison,
saturating difference, checked add/subtract). Only instants from one clock
domain may be compared or combined. Duration is normalized seconds plus
nanoseconds, retaining Rust's full `u64`-second range. `fips_deadline` caps at
100 years, then halves an overflowing step until it fits; `fips_backoff`
saturates its doubling factor at `UINT32_MAX`, its product at `Duration::MAX`,
then caps the result. Neither helper reads the clock.

This establishes the convention for later core APIs: caller-provided buffers
with explicit capacities and lengths, borrowed caller-owned contexts, and
status-code returns with out-parameters unchanged on failure. Copying a vtable
does not transfer ownership. Retained contexts/vtables must outlive their core
users, and callers provide synchronization. Future allocating constructors must
provide matching explicit destroy functions; allocations must never cross
allocator boundaries. These host interfaces allocate nothing and need no destroy.
Required pointers, normalized durations and compatible clock domains are caller
preconditions. Entropy fill and clock reads are infallible trait operations;
a host unable to fulfill them must fail at its boundary, never invent a value.

`fipsd/src/host_os.c` supplies `CLOCK_MONOTONIC` (including modern macOS) and
libsodium `randombytes_buf`. Construction performs no I/O; actual entropy use
initializes libsodium and actual clock/RNG failures terminate at the host edge.
The skeleton main links the adapters without reading them. Test-only adapters
in `tests/fakes/` use whole milliseconds starting at zero (advance explicitly),
fixed bytes, and seeded SplitMix64. Seeded fills emit little-endian words and
discard the final partial word's unused bytes on each call, just like Rust.
They are predictable and must never be used as production entropy.

## Differential oracle

The test-only [differential harness](tests/diff/README.md) validates the pinned
Rust fixtures and replays implemented families. Run `ctest -R '^FipsDiff' -V`
from the build directory; explicit SKIPs are not protocol parity.

## Proto codec

`fips/proto/error.h` mirrors every pinned Rust proto Error variant and payload,
including nested coordinate/bloom errors; `fips_proto_error_name()` returns the
Rust variant name. `fips/proto/codec.h` supplies allocation-free LE Reader/Writer
primitives. Reader borrows input and reports cumulative `MessageTooShort`
offsets. Writer borrows a fixed-capacity destination rather than growing a Rust
Vec, returning `MessageTooLong` on insufficient capacity. Failures preserve the
cursor and result buffers; success preserves the optional error out-parameter.
`advance` is checked in C, and overflowing required offsets reject with a
saturated `SIZE_MAX` diagnostic. Byte operations have no implicit length prefix.

## Shared utilities and deterministic math

`utils/index.h` wraps a 32-bit SessionIndex with LE conversion and eight-digit
hex display. One IndexAllocator per node keeps a caller-owned sorted u32 set;
allocation draws four bytes in one entropy fill per attempt, interpreted LE,
with the upstream default of 100 attempts. Collision exhaustion, absent free
and duplicate reserve are distinct results. Caller capacity is an additional
local policy refusal, reported before drawing entropy (not random exhaustion).
Reserve/free/clear do not draw entropy. The caller owns all storage/lifetimes.

`utils/hash.h` supplies raw-byte SipHash-1-3 and an open-addressing map for
peer-chosen byte keys. Each construction draws a fresh 128-bit key in one host
entropy fill, split into two LE words as upstream. Never use predictable test
entropy in production. Keys/values are borrowed; keys must stay immutable.
Composite keys require unambiguous caller serialization; raw-byte hashes do not
claim Rust's type-specific `Hash` framing. clear retains the secret key; deinit
releases borrows and wipes it, without freeing caller storage.

**Table decision:** libgo's GoHashMap has allocation, nsync locks and no public
keyed-hasher hook; reusing it would violate this core's dependency/runtime
boundary. Use a local bounded linear-probing table with byte equality and
cached SipHash instead. Like hashbrown, power-of-two tables retain 1/8 spare
buckets (4 buckets allow 3 entries). Upstream hashbrown grows dynamically with
no fixed numeric entry ceiling; C requires caller storage and returns RANGE at
its live-entry limit. The host may double storage through explicit transactional
rehash, up to its own memory budget and checked size_t bounds. Rehash reuses
the key and cached hashes without drawing entropy. Tombstones are reusable;
all walks are bounded by bucket count. This is not a hashbrown layout/iteration
compatibility claim. The randomized secret prevents a peer predicting buckets,
including on targets without ASLR; an unkeyed default/foldhash is not suitable.

`utils/onlink.h` consumes network-order IPv4/IPv6 octets, prefix lengths and
optional interface indexes. It canonicalizes IPv4-mapped IPv6 before clamping,
discards/counts zero-length prefixes, and answers contains/contains_on/share_link
and read_suspect exactly as upstream. Empty and failed reads hold nothing;
failed-read metadata is separate from read_suspect. The host, not this core,
reads interfaces (`read_system`); constructors copy into bounded caller storage.

`proto/math.h` implements the upstream multiply-then-square `powi(base,u32)`.
Each multiply rounds to binary64, with strict floating-point source flags;
there is no host pow call in the core. The host must preserve round-to-nearest,
gradual-underflow IEEE arithmetic. Tests port all 20 upstream utils/math tests,
plus 64 siphasher SipHash-1-3 reference vectors, entropy-call sequencing,
adversarial bucket chains, storage/refusal edges and 65 pinned-Rust powi outputs
for exponents 0..64 (base 0.5469). Utils/math has no differential harness family
yet; tests/diff/cases.c is intentionally unchanged.

Version decision (nostrc-u50yr.3): these APIs fold into libfips-core **0.1.0
Unreleased**, with no published version/tag change.
## Link catalog, session datagram and framing

`fips/proto/link.h` mirrors `core/src/proto/link.rs`: the `LinkMessageType`
catalog (12 values, exact upstream bytes and names), the FSP
`SessionMessageType` catalog (5 values, from `fsp/wire.rs`; the FSP frame codec
is a separate task), `from_u8` returning `InvalidMessageType{byte}`, name
helpers, and the SessionDatagram header. The C datagram is upstream's borrowed
`SessionDatagramRef`: decode requires the 35-byte body after the msg_type byte
(`MessageTooShort {expected: 35, got: len}` from a body-start reader) and
borrows the rest as payload; encode reserves capacity first and so rejects
atomically with `MessageTooLong`. TTL helpers follow `ttl_after_hop`: only a
TTL of 2 or more survives a hop, and delivery is never TTL-gated.

`fips/proto/framing.h` defines the Noise/FMP/FSP size constants by value with
their upstream sources, `FIPS_OVERHEAD` (106, static-asserted from its parts)
and `fips_framing_payload_limit(mtu)` =
`mtu.saturating_sub(106).saturating_sub(4)`: zero through 110, `mtu - 110`
above it, 1170 at 1280. `FIPS_IPV6_OVERHEAD` (77) belongs to the host IPv6
shim, not fips-core, and is deliberately absent. `tests/diff/proto_link.c`
carries the differential checks for the proto constants, both catalogs and
the `session-datagram` wire case; `tests/test_link.c` replays them through the
harness loader until `cases.c`'s per-name dispatch wires them in.

Version decision (nostrc-82g9n.6): these additive APIs fold into libfips-core
**0.1.0 Unreleased**, with no published version/tag change.

## Identity primitives

`fips/identity/{mod,key,local,peer,node_addr,address,encoding,auth,prefix}.h`
mirror upstream identity modules. `fips_identity_init_secp` creates an explicit
context and blinds it with exactly 32 host entropy bytes; there is no global
context or unblinded fallback. The context must outlive its identities.
Constructors allocate opaque identity storage once; destructors wipe it with
`sodium_memzero`. Public structs contain public information only. Explicit
secret exports, decoded secrets and nsec output are caller-owned sensitive
buffers and must be wiped by the caller. Signing takes 32 fresh auxiliary
bytes from the retained host entropy adapter on every call. Invalid generated
scalars return `InvalidSecretKey` without retrying or aborting.

The address derivation has one implementation in `key.c`: NodeAddr is the
first 16 SHA-256 bytes of the x-only key; IPv6 prepends `fd` to its first 15
bytes. Public-only PeerKey accepts off-curve bytes; conversion validates them.
PeerIdentity caches the full compressed public key, preserving supplied parity
or using even parity for x-only input. Public value bytes are directly available
for bytewise equality/ordering; validated objects must not be modified.

A small local streaming bech32 codec avoids `nip19`'s transitive dependency on
libnostr (and its runtime/host dependencies). Both decoders accept ASCII case
variations, including mixed case, but only NIP-19 original bech32 checksums and
zero padding. This deliberately also tightens nsec decoding to the requested
NIP-19-only contract (upstream's November 13 fix explicitly canonicalizes npub).
Malformed input is bounded to 1023 text bytes, with constant stack storage;
all failure outputs remain unchanged. Encoding always emits lowercase.

Signing, verification, derivation, hashing, formatting and encoding allocate
nothing. SHA-256 uses libsodium's streaming primitive, which requires neither
`sodium_init` nor an RNG. Authentication hashes `fips-auth-v1`, the 32 challenge
bytes and the timestamp in big-endian order, assembled with the proto Writer.
AuthResponse is a value, not a wire struct: upstream defines no serialization
for it. The host owns freshness, replay and expected-peer authorization policy.

`test_identity.c` ports 64 named behavioral tests from the 77 upstream tests,
plus 7 C-specific boundary/negative groups (71 total). The 13 Rust-only tests
not directly ported cover five Debug renderings, four From/error-source
conversions, two global OnceBox invariants, HashSet insertion and the RAII
ErasingKeypair guard. C uses opaque objects, explicit destruction, bytewise
values and explicit contexts instead; status names/conversion boundaries are
checked separately. No secret-bearing debug API or raw keypair copy is exposed.
The identity differential family replays all three seeded fixtures, including
the exact BIP-340 signature and the wrong-challenge error discriminant.

Version decision (nostrc-u50yr.2): additive identity APIs fold into initial
**libfips-core 0.1.0 Unreleased**; no version or release-tag change.

## Noise AEAD

`fips/noise/aead.h` wraps libsodium IETF ChaCha20-Poly1305 with caller-owned
32-byte keys (explicit destroy wipes them), counter nonces (four zero bytes
then LE u64), associated data and ciphertext followed by a 16-byte tag.
`fips/noise/mod.h` reserves the shared Noise constants: tag 16, nonce 12,
maximum message 65535 including tag. CipherState and session/replay policy are
described below; primitive callers must ensure counters never repeat under a key.
All calls are synchronous, with no allocations, RNG or clock in seal/open.
Capacity/length errors preserve output; authentication failure zeroes the
plaintext region, and failed calls leave the output length unchanged.

Key construction calls thread-safe, idempotent `sodium_init()` lazily without
a mutable core once flag. This requested hosted initialization is an explicit
exception to the no-implicit-OS/entropy rule above: hosts enforcing a strict
sans-IO boundary must initialize libsodium themselves before core entry.
Initialized keys may be borrowed concurrently; callers synchronize lifecycle
and destroy every explicitly owned copy.

RFC 8439 section 2.8.2 (nonzero nonce prefix) tests the shared private full-nonce
primitive; Appendix A.5 also exercises the public counter API byte for byte.
No differential family exercises AEAD directly yet; `tests/diff/cases.c` is
unchanged. `fips_bench_noise_aead` uses upstream's 64/1400-byte payloads and
16-byte AD, but measures AEAD only, not future session/replay/worker overhead.
Version decision (nostrc-pwo3h.1): APIs fold into **0.1.0 Unreleased**; no
published version/tag change.

## FMP wire and lifecycle

`fips/proto/fmp/{wire,core,state,silent,limits,types,mod}.h` ports the pinned
`core/src/proto/fmp/` modules, excluding `rekey/`. Sizes come only from
`framing.h`, with compile-time layout/Noise-size assertions in `wire.c`.
The `*_parse` functions preserve upstream's structural Option semantics;
`validate_frame` checks the complete envelope (including declared length).
`receive` uses that gate for **both pending and established** ingress: a short
frame returns only `DropFrame`, never a decrypt/dispatch action. Consumers
must not bypass this gate on a pending-session path. After AEAD, the separate
`receive_plaintext` event rejects lengths 0..4 before delivering a link message,
including on the pending-session path (the November 12 short-plaintext fix).
It does not itself authenticate, commit replay state or run rekey cutovers.
The future rekey profile must retain that inner-header gate on pending delivery.
The C diagnostics use
proto Error variants; malformed `flags`/`payload_len` are static field names.
Noise payloads and established ciphertext remain opaque. Header assembly does
not authenticate; pass the exact 16 header bytes as AEAD associated data.

Fmp itself is stateless, as upstream. Caller snapshots feed `poll_timeouts`
(retry before teardown), `poll_resends` (borrowed msg1 and a `fips_instant`
deadline), and `establish_inbound`/`establish_outbound` classifications. Action
lists append into caller storage atomically or return RANGE. The host supplies
stale/due snapshots, executes effects, and calls `record_resend` **only after a
successful send**. Returned bytes borrow their inputs. `ConnectionState` owns
bounded msg1/msg2 copies and pure identity/index/transport/statistics fields;
wall times are display provenance, while idle/timeout decisions use monotonic
instants. Noise phase/crypto and allocator ownership stay host-side. Optional
index set/take returns the old value for explicit release/transfer rather than
copying a future owning IndexLease.

**Rekey attachment:** `IN_Rekey` hands the verified `WireOutcome` to the held
link's future FMP rekey profile; its replacement request re-enters classification
with `replace=true`. No rekey implementation or Noise handshake is supplied.

`SilentSessions` uses caller-sized, sorted identity slots and eight ephemeral
digests per identity, preserving replay deduplication, epoch reset/adoption,
refusal counts, doubling/cap and horizon pruning. Full live storage returns
RANGE: the host must treat that as a resource refusal, not permission to promote.
Only an authenticated frame calls `heard`; unauthenticated receipt never clears
a refusal. Time is saturated whole milliseconds from an explicit origin. C
resend counters/exponents saturate instead of overflowing, float casts follow
Rust saturation, and deadline overflow follows the shared host ceiling policy.

`test_fmp.c` ports all 81 non-rekey upstream FMP tests by name, plus five C
boundary groups: all truncations and per-byte mutations, atomic codec capacity
failures, action/state bounds and extra transitions, silent-storage/digest
bounds, and AEAD AAD integration. `test_fmp_vectors.c` calls the four new
differential entrypoints directly (the orchestrator owns `cases.c` registration).
Msg1/msg2 payloads come independently from `noise.json`; encrypted checks cover
outer/inner framing with opaque ciphertext, **not** Noise or session-key parity.
The FMP libFuzzer harness exercises all codecs and the ingress short-frame gate.

Version decision (nostrc-82g9n.7): additive APIs fold into **libfips-core 0.1.0
Unreleased**; no published version/tag change.
## Spanning tree (nostrc-82g9n.2)

`fips/proto/stp/{mod,core,state,declaration,wire,limits}.h` mirror the pinned
STP modules. `ParentDeclaration` is a public value with an optional opaque
64-byte signature. Its only standalone encoding is the **48-byte signing
preimage**, not a link message. `TreeAnnounce` carries that declaration and
metadata-bearing ancestry: encode includes `0x10`, decode starts at version
1 after the dispatcher strips it. Reader/Writer calls are transactional,
use upstream proto errors, preserve trailing input and expose consumed bytes
through the Reader cursor. Semantic ancestry validation is separate from
wire parsing and from host signature/known-peer verification.

`TreeState` uses caller-owned sorted peer slots; both coordinates and each
slot retain the full existing u16 coordinate domain (~2 MiB each). Allocate
these on the host heap or in static storage, not an event-loop stack. Slot
capacity is a host policy, not a new protocol peer limit. Delivery-only slots
also consume capacity; the upstream `peer_count`/`peer_ids` accessors exclude
slots without a received declaration. Wire decoding accepts all 65535 entries;
the action driver locally refuses that terminal depth (including a stored
terminal-depth peer) because prepending self cannot fit. Local capacity,
depth and sequence-exhaustion refusals preserve state/output. Low-level
`set_parent` deliberately does not recompute; callers must check
`recompute_coords` before signing, just as upstream separates those operations.

`Stp::classify_announce`/`classify_periodic` remain pure decisions. The added
`receive_announce`, `periodic` and `remove_peer` drivers take host instants,
wall seconds and ranking facts, mutate core state and return a bounded
10-entry `fips_stp_actions` value. Actions request signing, parent/root/path
notifications with cache invalidation/backoff flags, a normal coalesced send
(to one peer or all), forced/content-delta filter refresh, or a reasoned drop.
They execute in order before another input. A failed signing action requires
retaining/retrying the batch from that action, or terminating/discarding the
state; mutation is not rolled back. No send can precede signing a newly
mutated declaration. The host enforces its normal per-peer send cooldown and
records actual sent counters separately: successful transport acceptance is
not delivery confirmation.

The STP-local `acks.c` adapter preserves the pinned `mmp::delivery` rule used
by `TreeState`: receiver-report evidence, sequence lineages, per-session
loss/unchecked budgets and bounded backoff via `proto/rate_limit`. It does not
implement MMP wire/transport. Flap limits quantize instants against the init
epoch, keep wall timestamps separate, and retire an expired dampening episode
before counting switches in the next one. No clocks, entropy, I/O or allocation
are read by any STP core entrypoint.

Tests port all **89** STP declaration/state/classify/limits/wire/delivery tests
by name, plus **12** C/action-driver tests. The other **13** upstream STP
coordinate tests already live in `test_coord.c` and are not duplicated here.
Explicit regressions cover loop-free recovery despite hold-down/hysteresis,
root fallback with forced filter refresh, rejected/stale announce side-effect
gates, accepted child-membership flips, sequence-only non-propagation,
mid-chain propagation, echo/sign ordering and sequence exhaustion. The new
`proto_stp` checks replay both pinned wire cases through the existing harness
loader; `cases.c` dispatch ownership remains with the orchestrator.

Version decision: these additive APIs fold into **libfips-core 0.1.0
Unreleased**; no published release/tag change.
## Noise SymmetricState

`fips/noise/symmetric.h` mirrors the `SymmetricState` of
`core/src/noise/handshake.rs`: `ck`, `h` and the handshake-phase cipher (an
`fips_aead_key`, its counter and `has_key`) as one caller-owned plain struct,
so the Fork copy (`fips_symmetric_copy`) is a struct assignment and both copies
are destroyed. `init` zero-pads a protocol name of 32 bytes or fewer and
SHA-256 hashes a longer one (both pinned 36-byte names are hashed); `mix_hash`
is SHA-256(h || data); `mix_key` is HKDF(ck, ikm, 2) with the second output
keying the cipher at counter 0; `encrypt_and_hash`/`decrypt_and_hash` pass the
bytes through without a key and otherwise seal/open with empty associated data
(upstream wire behaviour) and hash the ciphertext; `split` is HKDF(ck, "", 2) as
two caller-owned transport keys. A failed open has consumed its counter and
left `h` unchanged, exactly as upstream, so a read that may fail runs on a
copy. Length/capacity errors are `RANGE` and consume nothing; exact in-place
calls are supported.

HMAC-SHA256/HKDF-SHA256 (RFC 5869; the `hkdf` crate's semantics with empty
info) live in the private `src/noise/hkdf.h` over libsodium's allocation-free
`crypto_auth_hmacsha256`. NIP-44's KDF was not reused: `nostr_nip44_core` links
libnostr, which this library excludes. Every HMAC state, PRK and derived block
is wiped on all paths; `destroy` wipes the whole state.

Tests: RFC 4231 cases 1-4 and RFC 5869 cases 1-2 through the private header;
`init` plus the normalized `<- s` pre-message reproduce the `constructed` step
`h` of both `noise.json` patterns (the fixture captures the hash after the
constructors mix the responder's static key with its prefix forced to 0x02, so
SHA-256(name) alone is not the recorded value); MixKey/MixHash/Split sequences
pinned from `tests/tools/noise_symmetric_vectors.py` (`python3 -I`; hashlib and
hmac, plus the checked-in affine secp256k1 reference for the seeded key); round
trips, in-place calls, tamper/fork behaviour, split determinism and destroy
zeroization. `post_handshake.ck` is absent from the fixture (recorded gap), so
split has no differential check; `tests/diff/cases.c` is unchanged and the
handshake tasks own the remaining steps.

Version decision (nostrc-pwo3h.3): additive APIs fold into **0.1.0
Unreleased**; no published version/tag change.

## Bloom reachability (nostrc-82g9n.3)

`fips/proto/bloom/{core,wire,state,mod}.h` ports the pinned bloom module:
8,192 bits (1,024 bytes), five hashes, v1 size class 1. The two base hashes
are SHA-256 digest bytes 0..8 and 8..16 interpreted as little-endian u64s;
index i is `(size_t)(h1 + i*h2)` (wrapping u64) modulo the bit count, with
LSB-first packed bits. SHA-256 uses libsodium's fixed-storage hash primitive,
without RNG, initialization or allocator calls. Custom byte-aligned filters
up to 4,096 bytes cover all four upstream wire classes. Raw filter wire form
has no header; its hash count is out of band. FilterAnnounce encode includes
0x20, decode starts after it. Decode requires exactly the v1 body length,
but permits nonzero k != 5 just as upstream does; receive policy refuses it.

### Deterministic arithmetic and the log decision

FPR is `fips_powi(fill, k)`, **not pow**. The estimate follows upstream's exact
`-(m/k) * log(1 - X/m)` evaluation order. **The C log we rely on is the private
fdlibm kernel in `src/proto/bloom/log.c`, translated from Rust libm 0.2.16
`src/math/log.rs`**, not the platform C library's `log`. Upstream Cargo's
`libm` default features (`arch`) remain the reference. The bounded,
unsaturated filter guarantees a positive normal logarithm argument <= 1,
so the kernel needs no negative/subnormal/infinite-input branches. IEEE
binary64, nearest/ties-even, gradual underflow and no FMA/fast-math are
required, as for `fips_powi`. The Sun permission notice is retained.

This distinction matters: on the tested macOS arm64 host, platform
`math.log` and Rust libm estimates differed for 4,592 of 73,792 unsaturated
populations. The C kernel matched Rust libm on all 73,792. Parameter pairs
`(bits,k)` were `(64,3)`, `(1024,5)`, `(1024,7)`, `(2048,7)`, `(4096,5)`,
`(8192,1)`, `(8192,4)`, `(8192,5)`, `(8192,6)`, `(8192,7)`,
`(8192,255)`, `(16384,7)`, covering upstream core/wire custom tests.
`tests/tools/bloom_vectors.py` runs with `python3 -I`, computes the independent
estimate with `math.log` and the identical formula, and generates
`tests/bloom_pins.h`. The header separately pins the protocol's exact Rust
libm bits (including populations where platform log differs). The Rust
reference generator is `tests/tools/bloom_log_oracle.rs`; use a scratch
Cargo project with `libm = "=0.2.16"` and default features. The Python
`math.log` column is an approximate cross-check, not a bit-exact oracle;
its last bits can vary across hosts. Four address-index vectors are pinned
independently with Python hashlib SHA-256.

For 8192 bits/k=5, the estimate pins include:

| Set bits | FPR binary64 bits | Estimated count binary64 bits |
|---:|---|---|
| 0 | `0000000000000000` | `8000000000000000` (negative zero) |
| 4480 | `3fa90b5c98000000` | `409443bf08844f4d` |
| 4512 | `3fa9f3a229ba0000` | `40947c7cda265110` |
| 5936 | `3fc991e79a548c00` | `40a081a54e128031` |
| 5944 | `3fc9be22352580e0` | `40a08d4946820702` |

Saturation always refuses an estimate, even with an infinite cap. Otherwise
only FPR **above** the supplied cap refuses, leaving the result unchanged.

### State, ownership and actions

State borrows caller-owned peer slots, leaf slots and an action list. Sizes
are local policy, not peer-controlled allocation requests. Capacity failures
preserve state; no live-peer eviction is hidden. A peer slot uses roughly
21 KiB because it holds an inbound filter and four distinct sent filters of
up to 4 KiB each. Allocate large tables/actions off small event-loop stacks.
Aggregation uses about 64 KiB of bounded stack scratch on 64-bit hosts.
The caller supplies monotonic milliseconds, link receiver-report snapshots,
peer tree/child roles and authenticated-frame readiness. Hold announces to
unauthenticated peers. Keep peer pointers only until add/remove; feed send
results back synchronously before processing another state change.

Connect/disconnect, leaf or tree-role changes, accepted inbound replacement,
and a responder rekey/restart trigger refresh, never periodic filter polling
or an independent stale-filter timeout. The shell calls `refresh` on its
send pass and `check_announce` when delivery evidence/deadlines are evaluated.
Tree peers alone contribute to outgoing unions, with split horizon; mesh
filters remain available for local membership queries. Batched computation
uses per-bit multiplicities, equivalent to upstream's prefix/suffix ORs,
and coalesces inbound bursts. It marks only changed outputs; debounce is
500 ms and permits/restamps backwards clock readings as upstream does.

SEND actions are proposals, **not** transport acceptance or delivery.
Accepted results alone record sent history/debounce and outstanding delivery.
Link evidence confirms every counter since the base, with reorders and
cross-session/regressed/ambiguous reports handled as upstream. Each content
lineage/session has three loss and one unchecked resend, 1..60 s backoff,
120 s quiet reset, and 30 s fallback. C additionally coalesces a pending
retry before spending another credit, so long debounce/MTU refusal cannot
consume credits without sends. Refresh capacity preflight counts actual due
actions, including deferred changes, and is atomic on insufficient space.

Inbound FPR cap (default .20), v1 parameters and child echo checks report
REFUSED actions without wire NACKs or changing the accepted filter/sequence.
Echo checks inspect four distinct sent filters, require >=128 sent bits,
and compare overlap to `fill + threshold*(1-fill)` (default threshold .8;
>=1 or NaN disables rejection). Every refusal is observable; echo WARN is
once per peer per 300 s, the remainder DEBUG. Send refusal carries the actual
transport MTU and packet size, matching the 2026-11-12 upstream change
`42c26c19`: first MTU refusal WARN once per peering, every retry still reported
at DEBUG. A peer restart forgets sent history but preserves delivery,
debounce and MTU warning state; removing the peer clears all of them.

### Verification surfaces

`test_bloom.c` ports all 93 upstream bloom core/state/wire test names and
intents, including the delivery models, sent history and warning pacing;
Rust's Debug-format test is adapted to its public numeric diagnostics (C
has no Debug trait or logging API). Five additional groups cover pinned
arithmetic/indices, exhaustive codec length/class/capacity edges, real action
and refusal paths, refresh/bounds, and retry/action coalescing. The separate
`test_bloom_vectors.c` loads pinned `proto.json` and requires CHECKED from
both new `proto_bloom.c` differential entrypoints. It independently rebuilds
seeded identities/filters and compares wire bytes and error payloads. Mutated
population, address and sequence records fail, rather than passing on
round-trip integrity alone. `cases.c` is unchanged: its two dispatch rows
remain for the coordinator to wire after merge. `fips_fuzz_bloom` covers both
codecs plus merge and no-false-negative properties; CI lists remain sibling
owned. This is core/local evidence, not a running routing daemon claim.

Version decision: additive APIs fold into **libfips-core 0.1.0 Unreleased**;
no published version or release-tag change.

## FSP session layer (nostrc-82g9n.8)

`proto/fsp/{wire,core,quorum}.h` mirrors upstream `fsp/{wire,core,quorum}.rs`.
Wire readers borrow packed address-only coordinates and opaque Noise payloads;
no 2 MiB `TreeCoordinate` temporaries or allocation are needed. The entire u16
coordinate-count domain is readable. Encoders reject body/Noise lengths that
cannot be represented by u16 rather than truncating them. Shared transit
readers remain lenient about reserved flags/trailing bytes; endpoint checks and
strict body readers are separate. The 12-byte raw header is the AEAD AAD;
CP coordinates lie after that header, before ciphertext, and are excluded from
`payload_len`. The DataPacket port header is two LE u16s (IPv6 shim port 256).

The stateless plans append caller-buffer `InvalidateCoords`, `DemoteCoords`,
`TightenPathMtuLookup` and `InitiateLookup` actions atomically. The host executes
them and retains its session/cache/MTU tables. `LinkQuorum` stores at most one
vote per destination (quorum two), keyed by authenticated link peer, with an
inclusive 15000 ms window and non-refreshing repeats. Caller-owned capacity
refusal preserves state and verdict; no live votes are evicted. A positive-capacity
pending queue drops the oldest packet when full; zero capacity rejects unchanged
(the Rust helper unintentionally keeps one packet at a zero limit).

`receive` is an endpoint envelope gate, not transit policy: it emits a checked
handshake/encrypted/routing dispatch or `DropFrame`, never authenticated success.
Routing bodies still require their routing decoder. `receive_plaintext` must be
called only after AEAD/replay admission and checks short/reserved/unknown inner
headers before delivery. DataPacket consumers decode the port header separately.
The named XK handshake attachment (`nostrc-pwo3h.5`) consumes opaque payloads from
setup/ack/msg3; `FspRekeyProfile` (`nostrc-f12v2.2`) owns keys, replay, K-bit epochs,
crossing handshakes, resends, cutovers and drains. Every key path must retain the
post-AEAD plaintext gate. FSP dispatches by SessionDatagram source address, not
FMP's `SessionIndex`; no session-key state is faked by this port.

`test_fsp.c` covers upstream non-rekey core/quorum/wire behavior plus C byte-offset,
capacity, coordinate and port-header edges. The dedicated vector test invokes
all four checks directly (including negative input mutations), while `cases.c`
remains unchanged for the orchestrator's per-name registration after integration.
Vector parity covers wire framing with independently supplied opaque XK messages,
not the concurrent XK handshake implementation or encrypted-payload authentication.
Version decision: these additive APIs fold into **0.1.0 Unreleased**; no bump/tag.
## Routing decisions and error signals

`proto/routing/{core,wire,state,limits}.h` mirrors the pinned routing module.
`fips_select_best_candidate` borrows active peer coordinates and aggregated
bloom filters, requires strict progress, and orders by cost, distance, address.
A bloom miss returns None; the upstream shell's direct/greedy-tree resolution
is outside this module. `fips_router_route` takes that resolved `NextHop`,
delivers locally before testing TTL, otherwise applies `ttl_after_hop`, folds
the outgoing MTU, ORs CE, and appends one encoded datagram for Forward.
No-route and TTL-drop remain distinct outcomes. The host executes sends.

Wire encoders include the FSP cleartext prefix/type; decoders take the body
following them, refusing all reserved flag bits and trailing bytes. PathBroken
borrows caller coordinate storage (heap/static, not a small stack); the shared
coordinate codec discards metadata in its address-only wire form. As upstream,
the prefix casts body length to u16; the host enforces its frame/MTU budget.
Synthesis returns a SendError action toward the original source, with an
Admit/AdmitAtCapacity/Suppress verdict from the shared bounded rate limiter
(100ms interval, 10s age, 4096 entries). PathBroken synthesis deliberately
omits cached coordinates to avoid disclosing them to an unbound source address.
MTU and routing errors share one gate. Time is passed as `fips_instant`, never
read; capacity refusals preserve buffers, outputs and limiter state.

`test_routing.c` ports all 49 named Rust routing tests (22 core, 16 wire,
11 limiter), plus C buffer/refusal, bloom-miss handoff and classification
checks. Routing has no generated wire vectors at this pin; see the exporter
gaps list. `fips_bench_routing_next_hop` measures the real borrowed selector
versus a former allocating snapshot at 8/32/128/256 peers, eight-entry paths
and half-reachable filters, for nostrc-f66sr.4. The baseline is a C array view,
not Rust's HashMap/allocator; cross-language timings are not interchangeable.

Version decision (nostrc-82g9n.4): these additive routing APIs fold into
**libfips-core 0.1.0 Unreleased**; no published version/tag change.
## Noise IK/XK handshakes and Fork

`fips/noise/handshake.h` ports the pinned IK (FMP, 106/57 bytes) and XK
(FSP, 33/57/73 bytes) state machines. Constructors borrow a blinded secp
context and static identity, take the startup epoch and host entropy, and
mix the even-parity responder-static pre-message. The initiator draws its
32-byte ephemeral at write1; the responder draws at write2. The static
identity and entropy context must outlive the state and every fork.

Pattern-specific write/read entrypoints reject wrong roles, patterns and
progress before doing work. Raw reads can advance state on failure, as in
Rust. Retained initiator msg2 and responder XK msg3 states must use
`fips_handshake_fork_msg2`, `fips_handshake_fork_xk2` or
`fips_handshake_fork_xk3`: they read on a temporary copy, publish the output
copy only on success, and leave the entire original byte-identical on both
success and failure. Destroy the displaced original before explicitly
committing a successful fork, and destroy every unused copy. Source and
output must be distinct. `finish` splits the send/receive keys according to
role, exports final h, remote static and epoch, then wipes the state. The
caller owns the returned keys. Destroy wipes the entire struct (including
ephemeral secret, cached es and symmetric state), not the borrowed identity.

These are **pinned FIPS v1 bytes**, not a correction of upstream's security
properties: AEAD uses empty AAD, so h is not an authenticated channel
binding. IK msg2 repeats msg1's es, not Noise IK's se; the initiator caches
es so an authentication-failing msg2 costs one DH (an invalid compressed key
is rejected before DH). XK msg1 is unauthenticated. A parity-only mutation
of an ephemeral leaves x-only DH and the encryption keys unchanged, even
though it changes h. Do not silently change these wire properties here.

`test_handshake.c` flips bit 0 at **every byte offset** in all five fixture
messages and asserts the precise accepted set (zero-based offsets):

- IK msg1, IK msg2 and XK msg2: only offset **0**, the 02/03 parity byte.
- XK msg1: **0, 1, 2, 3, 4, 5, 8, 9, 11, 12, 13, 15, 19, 20, 22, 23,
  27, 29, 31, 32**. These encode valid curve points; the other offsets do not.
  Changed x-coordinates subsequently fail msg2 authentication at the initiator;
  only the parity change retains the keys (but not matching transcript hashes).
- XK msg3: **none**. Every mutation of encrypted static/epoch or their tags
  in any message must fail authentication.

The tests compare full original structs for every fork read, check failed
output preservation and destruction, wrong keys/role/pattern/progress,
lengths/capacities, invalid host scalars, and directional split metadata.
`FipsDiffNoise` checks each role after every exporter event, each emitted
message and final hashes, and any non-empty exported ck. Transport frames
and replay are checked by the session data path described below.

## Noise transport sessions

`noise/cipher.h`, `noise/replay.h` and `noise/session.h` complete the transport
path. `fips_noise_session_from_handshake` consumes the two directionally split
keys from `fips_handshake_finish`, without another split/swap, and retains the
final hash, remote static key, startup epoch and role. All storage is inline,
caller-owned, with no allocation, clock, RNG, locks or threads. Destroy wipes
the entire value (including padding); typed `take_cleared` moves to a distinct
destination and wipes the abandoned slot. Every copy has its own wipe obligation.

Auto-counter seal reserves a nonce before crypto and refuses `UINT64_MAX`;
explicit-counter open does not advance the cipher's local counter. Both support
AAD and the existing AEAD buffer/status conventions. `open` checks the 8192-bit
sliding replay window, authenticates, then marks ONLY on success. A corrupt
frame cannot move or consume the window. `open_unmarked` neither checks nor
marks replay; rekey callers check, trial-open, decide, then recheck/mark only
on acceptance. The replay gate rejects MAX, duplicates and counters of age
>= 8192. C's mark also safely refuses invalid inputs rather than indexing
outside the bitmap.

**Threading contract:** clones are per-worker independent cipher states; they
do not share counters. The session's window is single-writer. The dispatcher
serializes send-counter reservation across ALL clones of a key, and workers
seal with those explicit counters, NEVER independently auto-sealing from the
same snapshot. Decrypt workers authenticate; the sole window owner rechecks
and marks only on acceptance. Lifecycle/mutation and racing reads require host
synchronization. See `session.h` for the full ownership contract.

The Noise differential family now checks both completed-handshake sessions,
all 16 frames in each direction-pair pattern (including nonempty AAD on odd
frames, corrupt-open/nonconsumption and duplicate rejection), and the 16-step
replay trace: IK 31 + XK 36 + replay 16 = 83 checks, zero skips.
Version decision (nostrc-pwo3h.7): additive APIs fold into **libfips-core 0.1.0
Unreleased**; no published version or release-tag change.

## MMP metrics and reports

`proto/mmp/{wire,algorithms,metrics,sender,receiver,delivery,path_mtu,limits,state,core,mod}.h`
mirror the pinned MMP modules. All time is injected monotonic `u64` milliseconds,
never a clock call. `Mmp` is a stateless C namespace. The snapshot planners append
`ReapPeer`, `Heartbeat`, `SendLinkReport`, `LogLink`, `SendSessionReport` and
`LogSession` to caller-owned action storage. Capacity refusal is atomic. Logs
precede sends; link sends are phase-grouped SR then RR, session sends are
per-destination SR/RR/MTU. Backoff updates OR-fold successful sends per address
and emit in address order. The host builds reports only when driving an action,
marks heartbeat success only after send success, and owns all I/O/registry work.

Role state is public for by-value storage, read-only to callers after init.
The session aggregate holds common roles in `roles` plus `path_mtu`. OWD windows
clamp to 2..4096 samples, backed by caller storage with explicit capacity and
lifetime; Acks uses caller-sized peer slots. No allocator or implicit ceiling
peer eviction is involved. Insufficient storage preserves state and returns
RANGE. Optional results return false and preserve the result. No pointers may
overlap inputs/state/output, and required pointers must be valid.

The five codecs preserve the upstream body/type-byte split, ignore reserved
bytes, accept trailing bytes, and leave cursor/output unchanged on errors.
Link SR/RR encode 48/68 bytes including type; decode starts after type and
requires 47/67 bytes. Session SR/RR encode/decode 46/66-byte bodies; path MTU
is a two-byte LE body. Link/session conversions copy every named field.

Arithmetic keeps upstream integer jitter/SRTT, saturating interval/ECN/reorder
fields, wrapping sender timestamps, and bit-shift send backoff. Binary64 EWMA,
ETX, regression, goodput and burst-mean casts preserve operation order with
`-fno-fast-math -ffp-contract=off`. No MMP function needs `powi` or libm at this
pin. Rust float-to-integer saturation is explicit (NaN -> 0); out-of-domain
clock deadline/log interval products saturate rather than overflow. SRTT
samples must be nonnegative (normal wire-derived samples fit u32 milliseconds).
The host preserves IEEE round-to-nearest and gradual underflow. Spin RTT is
returned to the caller, never implicitly fed into timestamp-echo SRTT.

`test_mmp.c` groups all 152 tests in upstream `mmp/tests/` by behavior, with
C-specific bounded-storage, error-atomicity and delivery evidence tests. Rust's
serde name test becomes exact C parse/name pairs; the two private
`fresh_diff`/`dirtied` test-helper tests become complete field carry/clear
assertions without adding a test-only product API. File mapping:

| Upstream tests | Count | C groups |
| --- | ---: | --- |
| algorithms | 15 | algorithms, arithmetic_pins |
| core | 29 | decisions (all predicates plus multi-peer ordering) |
| wire | 20 | wire_codecs (all prefixes/capacities and conversion boundaries) |
| state | 55 | modes_and_sender, receiver_cycles_and_gaps, timing_owd_reorder, metrics_cycles, reset_field_inventory |
| path_mtu | 12 | path_mtu_flow |
| owd_trend | 6 | timing_owd_reorder |
| reorder_count | 2 | timing_owd_reorder |
| etx_trend | 3 | etx_once_per_report, arithmetic_pins |
| rekey_reset | 8 | reset_and_spin (both aggregate forms and roles) |
| spin_rekey | 2 | reset_and_spin |

`python3 -I tests/tools/mmp_reference.py` generates `tests/mmp_pins.h`; its
`--check` mode is a CTest. The 34 binary64 sequence values and 12 integer
SRTT/variance values were also checked against the actual pinned Rust
algorithms (compiled read-only), including OWD **16361 us/s** and jitter
**4626 us** for the sampled stream. ETX smoothing takes one step per accepted
report, never on reverse refresh or stale reports. Rekey resets RX baselines
and spin state, but carries sender state, lifetime RX totals, schedules,
SRTT/trends, reverse delivery, report counters and session path MTU, as upstream.

`FipsCoreMmpVectorsTest` directly runs all five differential checks (and rejects
five input mutations) while `cases.c` remains coordinator-owned. The sweep gate
now expects **25 checked / 3 skipped** and allows only `nostrc-f12v2.6`; exclude
`FipsProtoSweep` locally until the coordinator wires the five MMP rows.
The pre-existing STP/bloom private delivery adapters are not rewired here.
`fips_fuzz_mmp` fuzzes all five codecs and short-buffer atomicity, seeded from
the pinned report fixtures. Workflow lists also reserve sibling targets
`fips_fuzz_probe` and `fips_fuzz_rekey`; those targets are not created here.

Version decision (nostrc-f12v2.4): additive MMP APIs fold into **libfips-core
0.1.0 Unreleased**; no published version/tag change.
