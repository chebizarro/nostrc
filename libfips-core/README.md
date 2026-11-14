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
maximum message 65535 including tag. Handshakes/CipherState/replay policy are
not implemented here; callers must ensure counters never repeat under a key.
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
