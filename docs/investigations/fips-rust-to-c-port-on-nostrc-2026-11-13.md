# Investigation: Porting the FIPS runtime from Rust to C on top of nostrc (libgo / libfiber / libnostr)

**Date:** 2026-11-13
**Repos measured:** `fips` master `6aa80504` (2026-11-14, v0.6.0-dev) · `nostrc` master `120ca9672` (2026-11-09) · `metadesk` master `6c5983b7` (2026-10-10)
**Status:** assessment only — no code written, no decision taken

## Summary

**A full Rust→C port of FIPS is a 12–24 month, 1–2 engineer project even with nostrc; the "4–6 months" figure from the earlier chat assessment is withdrawn.** The earlier estimate was built on a stale snapshot of `fips` (≈191 files / ≈107k LoC, which matches the repo as of **2026-06-29**). Today `fips` is **792 Rust files / 436k LoC** (≈218k non-test), 553 commits landed in the last six weeks alone, and the crate has been split into a `#![no_std]`, sans-IO `fips-core` plus a Tokio-hosted root crate.

Two things did survive re-verification, and they still matter:

1. **nostrc removes the three foundations the original estimate feared most** — the async runtime (libgo + libfiber), the Nostr client with NIP-17/44/59 (libnostr + `nips/`), and the crypto primitives (libsecp256k1 ECDH, libsodium ChaCha20-Poly1305, HKDF-SHA256). Those are real, tested, and already linked by metadesk.
2. **fips's own restructuring did the hardest architectural work of a port for us.** `fips-core` is 41k lines of pure protocol with no std, no async, no OS calls and no `cfg(target)`; it is generic over `Clock`/`EntropySource` traits that the host supplies. That is exactly the shape a C core needs, and it is also exactly the shape a `cbindgen` C ABI needs — which makes **a C-hosted daemon over a Rust `fips-core` FFI** the strongest middle option.

The pragmatic ranking is unchanged from the earlier conversation but the gaps are wider: (1) keep FIPS as the external daemon metadesk already targets, optionally via the new native datagram API; (2) C host on libgo/libfiber over a `libfips_core` C ABI; (3) full port. A full port also means tracking a wire protocol that moves at ~90 commits/week.

> **Staleness warning.** Both `fips` and `nostrc` are moving quickly (553 and 2,142 commits since 2026-10-01 respectively). Every figure below was re-measured on the commits named above with the commands in [Appendix A](#appendix-a--how-the-numbers-were-measured). Before acting on any number, re-run the command; before relying on any API claim, re-open the header or file cited. Where the earlier chat assessment was wrong, the correction is called out inline.

## Question as asked

> How complicated would it be to port the fips runtime from rust to c? What sort of engineering effort would be required?
>
> The `nostrc` repo now in the workspace contains an extensive nostr library in C — `libnostr`, an async library modeled on goroutines — `libgo` and a C implementation of fibers — `libfiber` as well as other utilities and is already used by metadesk — how would this change the porting assessment calculus?

The answer was given in chat only. This document is the on-disk version, re-checked against the current trees.

## 1. What the earlier assessment got wrong (and why)

| Claim in the chat assessment | Reality at `fips@6aa80504` | Consequence |
|---|---|---|
| 189 Rust files, ~107k LoC | **792 files, 436,367 LoC** (306k root crate + 128k `fips-core` + 0.4k `testing/ble`). 191 files is the count at `793f8444` (2026-06-29) — the snapshot was ~4½ months old. | Every size-derived estimate was ~4× low. |
| ~13k test lines, 1,176 tests | **≈216k test lines** (`tests/` dirs, `tests.rs`, `*_tests.rs`, `faults.rs`, `testutil.rs`), **6,198 `#[test]`/`#[tokio::test]` functions**, 246 inline `#[cfg(test)]` modules. Roughly half the repo is tests, including simulators and model explorers for rekey, settlement and multipath. | The test corpus is an asset (oracle for differential testing) not a porting liability — but you cannot "port the tests". |
| 2,248 async sites, Tokio everywhere | **14,455** `async fn`/`.await` sites in total; **4,292 in non-test root code; 9 in `fips-core`**. 233 `tokio::spawn`, 68 `select!`, 309 `mpsc::channel` + 34 unbounded, 29 `oneshot`, 20 `watch`, 2 `broadcast`, 8 crossbeam. | Async is confined to the host crate. The core is already sans-IO. |
| Modules: `noise/`, `tree/`, `bloom/`, `protocol/`, `mmp/`, `discovery/`, `upper/`, `node/`, `transport/`… | `discovery/`→`src/nostr/` (2026-07-09); `upper/`→`src/ipv6tun/` (2026-10-05); `tree/bloom/protocol/mmp` → `core/src/proto/{stp,bloom,fmp,fsp,mmp,lookup,routing,path,rekey,settle,probe}`; `core/` crate created 2026-10-11. New: `src/native/` (2026-08-20), `transport/{nym,socks5,ethernet}`, `instr/`. | Porting order and module map below are rewritten. |
| Noise IK over secp256k1 | **Noise IK (FMP link layer) *and* Noise XK (FSP end-to-end sessions)**, both `*_secp256k1_ChaChaPoly_SHA256`; plus a shared rekey state machine (`proto/rekey`, 6.6k non-test lines) with legacy models of the deployed 0.5.x releases. | Crypto-critical surface roughly doubled; rekey/wire compatibility with deployed nodes is a hard constraint. |
| 6 transport backends, ~18k lines | 7 backends (UDP, TCP, Tor, SOCKS5, Nym, Ethernet, BLE), **46k non-test lines**; BLE alone is 16.8k with BlueZ, CoreBluetooth and an Android radio-bridge. Platforms: Linux, macOS, FreeBSD, Windows, Android, plus bare-metal builds of the core (thumbv7em, riscv32). | "Transport is 10% of effort" is no longer true; BLE/Ethernet are a port in their own right. |
| metadesk is ~26k lines / 80 files | **74,914 lines / 169 files** (C, C++, ObjC). | Irrelevant to the port but the earlier comparison was wrong. |
| nostrc "covers ~90% of async" | True for spawn/channels/select/context/timers. **False for datagram I/O**: libfiber exposes only `gof_read/gof_write/gof_connect/gof_accept`; there is no fiber-parking `recvfrom/sendto/recvmsg/sendmsg`. FIPS's UDP path needs `sendmsg`/`recvmsg` (per-peer connected sockets, `IP_PKTINFO`/`IP_RECVDSTADDR`, ECN/CE counting, `SO_BINDTODEVICE`/`IP_BOUND_IF`). | A small but mandatory libfiber extension. |

## 2. FIPS today: shape and size

### 2.1 Architecture that matters for a port

`fips` is now a two-crate workspace (`Cargo.toml` `[workspace] members = [".", "core"]`), 457 packages in `Cargo.lock`.

**`fips-core` (`core/`)** — `#![no_std]` + `alloc`. From `core/src/lib.rs`:

> Code in this crate builds for any target with an allocator. The rule that keeps it so: no `std`, no async runtime, no OS calls, and no target `cfg`. Anything that needs one of those belongs in the root `fips` crate, which supplies it to the core rather than the core reaching for it.

The host supplies randomness through `EntropySource`, time through `time::Clock`/`time::Instant`, and the core "takes instants as arguments and never reads a clock". `testing/build-core-baremetal.sh` builds it for `thumbv7em-none-eabihf` and `riscv32imac-unknown-none-elf` in CI. The AEAD has two backends (`ring` behind a feature for hosted builds; RustCrypto `chacha20poly1305` for bare metal).

**Root `fips` crate (`src/`)** — Tokio host: sockets, TUN, clocks, logging, Nostr client, config, control socket, the four binaries (`fips`, `fipsctl`, `fips-gateway`, `fipstop`). `src/node/` "drives all of it … calls into [`proto`] with what arrived and the current time, and carries out the actions it returns" (`core/src/proto/README.md`).

This split is the single most important fact for the port: **the part of FIPS that is hard to get right (crypto, state machines, codecs) is already isolated from the part nostrc replaces (runtime, I/O, Nostr).**

### 2.2 Size, measured (non-test lines unless stated)

**`fips-core` — 128,298 total / ≈41,400 non-test**

| Module | Non-test LoC | Notes |
|---|---|---|
| `proto/path` | 6,783 | multipath path set + `Bundle` machine (24,973 with its simulators) |
| `proto/rekey` | 6,575 | shared key-rotation machine; `fmp/rekey` (IK) and `fsp/rekey` (XK) profiles; legacy 0.5.x models in tests |
| `proto/settle` | 5,493 | peer settlement (crossed dials, restarts, rekeys); "called by nothing yet" |
| `proto/mmp` | 4,160 | link/session metrics, reports, path MTU, spin bit, ETX |
| `proto/fmp` | 2,186 | link layer: lifecycle + framing (wire sizes checked against `noise/`) |
| `proto/lookup` | 1,752 | coordinate lookups, retry ladder |
| `proto/stp` | 1,574 | spanning tree (declarations, coordinates, loop handling) |
| `proto/fsp` | 1,461 | end-to-end session layer |
| `proto/probe` | 1,220 | diagnostic path probe |
| `proto/bloom` | 1,162 | reachability bloom filters + announcements |
| `proto/routing` | 969 | next-hop selection, routing errors |
| `proto/*.rs` | 1,105 | `codec.rs` (bounds-checked `Reader`/`Writer`), `coord.rs`, `link.rs`, `framing.rs`, `math.rs` (`powi` for bit-exact cross-platform arithmetic), `rate_limit.rs` |
| **`proto` subtotal** | **≈34,400** | |
| `noise/` | 2,322 | IK + XK handshakes, `Fork` (read-on-copy), `NoiseSession`, `ReplayWindow`, `CipherState`, AEAD trait + 2 backends. All secret-holding structs are `Zeroize`/`ZeroizeOnDrop`. |
| `identity/`, `cache/`, `node/`, `utils/`, `time/`, `transport/` types, `entropy.rs` | ≈4,600 | keys, npub/nsec, `NodeAddr`→fd00::/8, auth challenge; `CoordCache`; rate limiters + stats history (generic over `Clock`); `SessionIndex`/`IndexAllocator`; on-link prefixes; SipHash-keyed `hashbrown` map |

**Root `fips` — 306,157 total / ≈176,800 non-test**

| Module | Non-test LoC | Notes |
|---|---|---|
| `node/` | 50,186 | `mod.rs` 5.1k; `handlers/` 13.7k; `settle/` 4.2k; `lifecycle/` 6.0k; `dataplane/` 3.5k; `encrypt_worker.rs` 3.8k + `decrypt_worker.rs` 1.0k; `peering/` 3.0k; `acl.rs` 1.9k; `netmon/` 1.0k; `session/` 1.2k; metrics/stats/diag/reject/peer_budget/bloom/tree glue. (`node/tests/` is another 95.6k.) |
| `transport/` | 45,991 | `ble/` 16,837 · `udp/` 6,281 · `ethernet/` 5,623 · `tcp/` 4,918 · `tor/` 4,149 · `nym/` 1,652 · `socks5/` 1,343 · shared (`framing`, `medium`, `presence`, `sockbuf`, `stream`, `watcher`, `loopback`) ≈5,200 |
| `bin/` | 13,616 | `fipsctl` 2.5k, `fipstop` TUI (ratatui) ≈5k, `fips-gateway` 0.9k, `fips` 0.8k + shared |
| `ipv6tun/` | 12,014 | TUN, IPv6 shim, ICMPv6, fragmentation, MSS clamp, `.fips` DNS, firewall state, BSD/Windows net glue |
| `gateway/` | 11,129 | DNS proxy, virtual IPv6 pool, nftables/proxy-NDP NAT, conntrack (Linux only) |
| `config/` | 10,386 | YAML (`serde_yaml`), floors/ceilings, reload log |
| `native/` | 8,708 | native datagram API: Unix `SOCK_SEQPACKET` control channel, fd passing, flow registry, line protocol, reference client (1.5k + 0.4k codec) |
| `nostr/` | 6,554 | rendezvous runtime (2.6k), traversal offers/answers + machine, overlay advert, STUN, signal gate, failure state |
| `control/` | 6,119 | line-delimited JSON over Unix socket (TCP 127.0.0.1:21210 on Windows); `show_*` queries, snapshots |
| `peer/` | 5,996 | `ActivePeer`, link state, retry/cooldown |
| `instr/` | 1,629 | tick-body profiler (feature-gated) |
| `utils/`, `mdns/`, `time/`, `dnsmsg.rs`, `hosts.rs`, `cache.rs`, `noise.rs` (re-exports) | ≈3,900 | |

**Platform surface** (root crate `cfg` sites): `unix` 306 · `linux` 174 · `macos` 93 · `windows` 65 · `freebsd` 29 · `android` (BLE). `unsafe`: 441 in root (FFI to BSD routing sockets, CoreBluetooth via `objc2`, Windows IP Helper/wintun), 6 in core.

**Shared state** (root): 56 `Arc<Mutex|RwLock<…>>`, 58 `ArcSwap` (hot-path snapshot swaps — relevant: libgo has `refptr` but no RCU-style swap primitive).

### 2.3 Protocol facts a C implementation must reproduce exactly

- **Noise DH over secp256k1** (`core/src/noise/handshake.rs:404-419`): `ecdh = SHA-256(x-coordinate of the shared point)`, y/parity discarded. libsecp256k1's `secp256k1_ecdh` with a custom hash callback that copies `x` — the pattern `nips/nip04/src/nip04.c` already uses (`ecdh_hash_xcopy`) — followed by SHA-256 gives the same bytes.
- **Patterns:** `Noise_IK_secp256k1_ChaChaPoly_SHA256` (FMP link setup, msg1 = 33-byte ephemeral + 49-byte encrypted static + 24-byte encrypted startup epoch) and `Noise_XK_secp256k1_ChaChaPoly_SHA256` (FSP sessions). Sizes are constants in `core/src/noise/mod.rs` and are cross-checked by the FMP wire codec.
- **Replay window, counters, rekey epochs (K-bit), session indices** — `proto/rekey/README.md` and `docs/reference/wire-formats.md` (951 lines) are the spec. Deployed 0.5.x behaviour is pinned by `proto/rekey/tests/legacy`.
- **Nostr surface** (`docs/reference/nostr-events.md`): kind **37195** overlay advert (parameterized replaceable, `d=fips-overlay-v1`, NIP-40 expiration, JSON `OverlayAdvert` with endpoints/signalRelays/stunServers); kind **21059** traversal signalling (`TraversalOffer`/`TraversalAnswer`, NIP-44 inside a NIP-59 gift wrap, ephemeral); kind **10050** NIP-17 inbox relay list. One secp256k1 key is both the FIPS identity and the Nostr key. The Rust side uses `nostr`/`nostr-sdk` 0.44 with `RelayPoolNotification`, `AdmitPolicy`/`AdmitStatus`, `EventBuilder`, `Filter`.
- **Addressing** (`fips_addr.h` in metadesk mirrors it): `fd || SHA-256(xonly_pubkey)[0..15]`; `.fips` names resolve via the daemon's DNS. IPv6 overhead 77 bytes.
- **Spec corpus:** `docs/design/*.md` + `docs/reference/*.md` = **15,864 lines**, including `fips-transport-layer.md` (1,469), `spanning-tree-dynamics.md` (1,016), `wire-formats.md` (951), `native-api.md` (810), `fips-session-layer.md` (626), `fips-nostr-discovery.md` (651). This is unusually good spec coverage for a port.

## 3. What nostrc provides today (re-verified at `120ca9672`)

| Component | Size | What is there | Relevance / gaps |
|---|---|---|---|
| **libgo** (`libgo/`) | 11,169 LoC | `go()` (pthread), `go_fiber()`/`go_fiber_compat()` (fiber, falls back to pthread if runtime not linked), `GoChannel` (bounded MPMC; `send/receive/try_*/…_with_context/close/ref/unref`), `go_select()`/`go_select_timeout()` (ref-counted waiters, timeout → `selected_case=-1`), `GoContext` (`background/with_cancel/with_deadline/done/err/is_canceled`), `GoWaitGroup`, `refptr`, `hash_map`, `Ticker` (`create_ticker/stop_ticker`), `blocking_executor` (`go_blocking_submit`). Tests: channel stress/MPMC/close, select cancel/closed/multi, context cancel/timeout/stress, ticker, refptr. | Direct replacements for `tokio::spawn`, `mpsc`, `oneshot` (cap-1 channel + close), `select!`, `CancellationToken`, `interval`. **No unbounded channel** (fips uses 34); **no `watch`/`ArcSwap` equivalent** (fips uses 20 + 58) — needs an `_Atomic` pointer + `refptr` snapshot helper. |
| **libfiber** (`libgo/fiber/`) | 3,541 LoC | Cooperative M:N scheduler, per-worker run queues, work stealing, affinity, sleep heap, `gof_init/run/start_background/spawn/yield/sleep_ms/join_background/request_stop`; netpoller **epoll + kqueue full, IOCP stub**; asm context switch x86-64/ARM64 + `ucontext` fallback; `gof_read/gof_write/gof_connect/gof_accept` park on the netpoller; `gof_chan_*`; GoChannel fiber hooks; stack dumps, trace hooks, scheduler stats. | Replaces the Tokio reactor for TCP/Unix sockets. **Gap: no `gof_recvfrom/sendto/recvmsg/sendmsg`** — must be added (~150–300 LoC mirroring `gof_read`) before any UDP transport port. **Cooperative only**: a CPU-bound tick (fips has a profiled "rx loop hot tick") must yield or run on `go_blocking_submit`. **Windows is a stub** — a C port would lose fips's Windows support unless IOCP is finished. |
| **libnostr** (`libnostr/`) | 27,796 LoC | Events, tags, filters, keys (secp256k1 Schnorr, bech32), relay (`nostr_relay_publish_and_wait` with `OK` tracking, `nostr_relay_auth` for NIP-42, `EOSE`/`CLOSED`/`AUTH` frame handling), `NostrSimplePool` (`subscribe`/`subscribe_async`/`query_single`, event + batch middleware, `auto_unsub_on_eose`, recovery window, brown-list), storage, rate limiter, security limits, GLib/GObject layer. | Covers everything `src/nostr/runtime.rs` does with `nostr-sdk` (`RelayPool`, notifications, admit policy ≈ middleware). metadesk already uses it. |
| **nips/** | 83,286 LoC, 60+ dirs | **nip17** (`create_rumor/seal/gift_wrap`, `wrap_dm/decrypt_dm`, `unwrap_gift_wrap/unwrap_seal`, 10050 relay list parse/create/get), **nip59** (`wrap/unwrap[_with_key]`, `create_ephemeral_key`, `randomize_timestamp`, `get_recipient`), **nip44** (+ `kdf.h`: `nostr_nip44_hkdf_extract/expand`, `hmac_sha256`), nip19, nip40, nip42, nip11, nip65, nip04 (secp256k1 ECDH x-copy pattern), nip49 (AEAD), … | Everything fips's three event kinds need. The NIP-59 wrap must carry the kind-21059 traversal payload — a thin FIPS-specific layer over `nostr_nip59_wrap`. |
| **Crypto primitives** | — | libsecp256k1 (`secp256k1_ecdh`, `keypair`, `xonly_pubkey`, Schnorr), libsodium `crypto_aead_chacha20poly1305_ietf_{encrypt,decrypt}` (used in nip44/nip49/libmarmot), SHA-256 and HMAC via OpenSSL EVP, HKDF-SHA256 ×3 (`nips/nip44/…/kdf.h`, `third_party/nostrdb/src/hkdf_sha256.h` (ccan), `libmarmot/src/mls/mls_key_schedule.c`), `secure_buf.h`/`secure_bzero`. | Every primitive Noise `*_secp256k1_ChaChaPoly_SHA256` needs. **Not present:** the Noise framework itself (symmetric state, `MixKey/MixHash/Split`, IK/XK message flows, fork-on-read). ~2.3k Rust → ~3.5–4.5k C of security-critical code. |
| **libjson** / cJSON | 2,143 | JSON; metadesk links cJSON. | Control socket + native API line protocols, advert content. |
| **libmarmot** | 88,899 | MLS over Nostr. | Not needed for FIPS; a source of HKDF/AEAD usage patterns only. |

**Not in nostrc and needed by a full port:** Noise framework; YAML (libyaml); TUN (ioctl per OS; metadesk has none either); rtnetlink/`PF_ROUTE` monitors; nftables (libnftnl) and conntrack for the gateway; BlueZ D-Bus / CoreBluetooth / Android BLE; SOCKS5 client (Tor/Nym); mDNS (`mdns-sd` → avahi or hand-rolled); DNS message codec (`dnsmsg.rs` 1.1k, `simple-dns`); ncurses for `fipstop`; `windows-service`/`wintun`.

## 4. metadesk's position

- metadesk is **74,914 lines / 169 files**; it consumes nostrc through a symlinked Meson subproject (`subprojects/nostrc → ~/Documents/Projects/nostrc`, HEAD `120ca9672`) and already uses `GoChannel`/`go_select_timeout` (`src/host/main.c`, `src/client/main.c`), libnostr events, NIP-46/51/5f/17.
- **metadesk does not embed FIPS.** `docs/metadesk-spec.md` §2.2.1 "External FIPS Runtime Contract": FIPS is a separately-run daemon that owns TUN, `.fips` DNS, discovery, NAT traversal, ACLs and routes; metadesk talks to its control socket (`src/core/fips_control.c`, one JSON line per connection, `show_status`/`show_peers`/`show_sessions`) and derives `fd00::/8` addresses locally (`src/core/fips_addr.c`). The legacy `src/fips-nat/` STUN/TURN path is gone.
- **Drift found while checking:** the spec says "Upstream FIPS v0.3.x / v0.4.x"; fips is at **0.6.0-dev** (tags through `v0.5.3`). Also fips's control-socket resolution order is now `/run/fips/control.sock` → **`/var/run/fips/control.sock` on macOS/FreeBSD** → `$XDG_RUNTIME_DIR/fips/control.sock` → `/tmp/fips-control.sock` (`docs/reference/control-socket.md`), while `fips_control.h` defines only `/run/fips`, the XDG suffix and `/tmp`. A packaged macOS daemon would not be found. See [follow-ups](#6-follow-ups).

So the question "port FIPS to C" is, for metadesk, really "should metadesk stop depending on an external daemon?" — and nothing in the current spec asks for that.

## 5. Revised assessment

### 5.1 Scope tiers

| Tier | Rust non-test LoC | Estimated C LoC (×1.5–2) | Content |
|---|---|---|---|
| **A. Core** | ≈41k | 60–80k | `fips-core`: Noise IK/XK + sessions + replay + rekey machine, identity, codecs, STP, bloom, routing, lookup, MMP, FMP/FSP, path, settle, probe, coord cache, rate limiters. Pure C, no I/O; `Clock`/`EntropySource` generics → vtable structs. |
| **B-min. Host for Linux + macOS** | ≈103k | 150–200k | `node` 50k, `peer` 6k, `config` 10k, `control` 6k, `nostr` 6.5k, `transport/{udp,tcp,shared}` ≈16k, `ipv6tun` 12k (or `native` 8.7k instead), `fips` binary 0.8k, `fipsctl` 2.5k. Enough for metadesk's §2.2.1 contract. |
| **B-full. Parity** | ≈+58k | +90–120k | BLE (3 platforms) 16.8k, Ethernet 5.6k, Tor/SOCKS5/Nym 7.1k, gateway 11.1k, `fipstop` ≈5k, Windows + FreeBSD paths, `instr`, mDNS. |
| **Harness** | — | 10–20k | Fixture extraction from the Rust tests, differential test runner (Rust core vs C core on identical byte vectors/timelines), sanitizer CI (ASan/UBSan/TSan), fuzzing of every codec (`Reader`/`Writer` boundaries, Noise messages, control JSON). |

### 5.2 Effort

| Scope | Calendar | People | Notes |
|---|---|---|---|
| A. Core only | 3–5 months | 1 senior C engineer with crypto review | Noise + rekey are the risk; everything else is mechanical but large. Differential testing against `fips-core` is mandatory — the Rust tests are the oracle, not something to port. |
| A + B-min (metadesk MVP) | **9–13 months** | 1–2 | Includes the libfiber datagram extension and a `watch`/`ArcSwap` helper for libgo (2–4 weeks, upstream to nostrc). |
| A + B-full | **18–24 months** | 2 | And then ongoing: fips landed 553 commits in six weeks; a C port is a permanent second implementation with wire-compat obligations to deployed 0.5.x/0.6.x nodes. |

Why this is higher than the earlier "4–6 months": the codebase measured then was the 2026-06 snapshot; non-test code alone has grown from ≈90k to ≈218k lines, and whole subsystems (multipath, rekey machine, settlement, native API, Nym, Ethernet, CoreBluetooth, Windows, FreeBSD) did not exist. nostrc's coverage of runtime/Nostr/crypto is as good as claimed — it just covers a smaller share of a much larger whole.

### 5.3 Translation patterns (checked against current headers)

| Rust (fips root) | C (nostrc) | Caveat |
|---|---|---|
| `tokio::spawn(async move { … })` | `go_fiber(fn, ctx, 0)` / `go_fiber_compat(fn, ctx)` after `gof_start_background()` | fiber bodies must not block on OS calls other than `gof_*`; use `go_blocking_submit` for `getaddrinfo`, D-Bus, etc. |
| `mpsc::channel(n)` | `go_channel_create(n)` | `unbounded_channel` → pick a bound; fips already bounds most queues (`pending_per_flow`, `backlog`, `max_flows`). |
| `oneshot::channel()` | `go_channel_create(1)` + `go_channel_close` after one send | |
| `watch::channel`, `ArcSwap` | `_Atomic(T*)` + `refptr` retain/release on read | not in libgo today; small helper. |
| `tokio::select! { … , _ = sleep(d) => … }` | `go_select_timeout(cases, n, ms)`; check `r.selected_case == -1` for timeout | |
| `CancellationToken` / shutdown | `GoContext` + `go_context_done(ctx)` as a receive case | |
| `tokio::time::interval` | `create_ticker(ms)` channel as a select case | |
| `UdpSocket::recv_from`/`send_to`, `recvmsg` with pktinfo/ECN | **`gof_recvmsg`/`gof_sendmsg` — to be added** | the only hard gap in libfiber. |
| `TcpStream`, `UnixStream`, `SOCK_SEQPACKET` | `gof_read/gof_write/gof_connect/gof_accept` | fd passing (`SCM_RIGHTS`) for the native API needs `sendmsg` too. |
| `Arc<Mutex<T>>` | `refptr` + `nsync_mu` (libgo already links nsync) or `pthread_mutex_t` | fiber-aware blocking: hold locks briefly, never across `gof_*`. |
| `enum` with data, `Result<T, E>` | tagged unions; `int` return + out-params, `Error **` as libnostr does | |
| `trait Clock/EntropySource/Instant` generics | vtable structs passed into every core call | matches `fips-core`'s "host supplies time and entropy" rule. |
| `Zeroize`/`ZeroizeOnDrop` | `sodium_memzero`/`secure_bzero` on every error path and free | fips is meticulous here (handshake forks, moved-out secrets); C needs the discipline by convention + ASan tests. |

### 5.4 Alternatives, ranked

1. **Keep FIPS as the external daemon (status quo, 0 porting cost).** This is metadesk's documented contract. Two upgrades are available without porting anything: (a) adopt the **native datagram API** (`node.native_api`, Unix-only, experimental: `SOCK_SEQPACKET` control channel, fd per flow, key+port addressing, no TUN) by writing a small C client for its line protocol — the Rust reference client is ~1.9k lines, so ~1–2k C; (b) refresh the §2.2.1 contract to 0.6.x (socket paths, `show_*` snapshots, rendezvous config keys). *Effort: days to a few weeks.*
2. **C host over a Rust `libfips_core` C ABI.** `fips-core` is `no_std`, sans-IO and generic over host-supplied clock/entropy — it is already shaped like a C library. Add a `cbindgen`/`#[no_mangle] extern "C"` shim crate (handshake/session/replay, codecs, STP/bloom/routing/lookup/MMP state machines; opaque handles; caller-provided buffers), then write the host on libgo/libfiber. This keeps the crypto and protocol logic single-sourced with upstream, so wire compatibility tracks fips releases automatically. Costs: a Rust toolchain in nostrc/metadesk's build, and `cbindgen` has no native Rust↔C path in nostrc yet (`bindings/rust` goes the other way, via `bindgen`). *Effort: shim 2–3 months; host B-min 5–8 months; total ≈8–11 months, but far lower long-term maintenance than option 3.*
3. **Full C port (Tiers A + B).** Only if the goal is independence from Rust itself (e.g. an OpenWrt/BSD base-system policy, or a C-only embedded target the `fips-core` bare-metal build does not already satisfy). *9–13 months to a metadesk-usable daemon; 18–24 to parity; permanent dual-implementation cost.*

A fourth option — linking the Rust `fips` library crate directly into metadesk as an "embedder" (`docs/how-to/migrate-an-embedder-to-v0.6.md`) — is option 2 without the C host, and inherits Tokio in-process; it is not attractive for a C/Meson application.

### 5.5 If the full port is chosen: order and gates

1. **libfiber datagram I/O + libgo snapshot helper** (upstream to nostrc first; unblock everything else).
2. **Core, bottom-up with differential tests at each step:** `proto/codec` → `identity` → `noise` (IK, XK, sessions, replay; test vectors captured from Rust) → `proto/{bloom,coord,stp,routing,lookup}` → `proto/{fmp,fsp}` → `proto/rekey` (+ legacy 0.5.x vectors) → `proto/{mmp,probe,path}` → `cache`, `node/{rate_limit,stats_history}` → `settle` last (nothing calls it yet).
3. **Host skeleton:** `config` (libyaml) → `control` (JSON lines; reuse `fips_control.c` conventions) → `peer` → `node` lifecycle/handlers/dataplane with libgo channels → encrypt/decrypt workers.
4. **Transports:** UDP, TCP; then Tor/SOCKS5; Ethernet; BLE only on demand.
5. **`nostr/` rendezvous** on `NostrSimplePool` + nip17/nip59/nip44.
6. **`ipv6tun`** (Linux, then macOS/FreeBSD) and/or **`native`** API.
7. **Gateway, `fipstop`, Windows** as separate, optional projects.

Gates: every core module passes its differential suite under ASan/UBSan before the next starts; a two-node interop test (C daemon ↔ Rust daemon over UDP, FMP IK + FSP XK + one rekey cycle) before any transport beyond UDP.

## 6. Follow-ups

Filed or to be filed as beads issues (not done in this session — this document is the deliverable):

- **metadesk:** update spec §2.2.1 from "v0.3.x / v0.4.x" to 0.6.x; add `/var/run/fips/control.sock` to `fips_control.h`'s resolution order for macOS/FreeBSD; evaluate the native datagram API as an alternative to TUN for the frame channel.
- **nostrc / libfiber:** add fiber-parking `gof_recvfrom/gof_sendto/gof_recvmsg/gof_sendmsg`; document the UDP story in `libgo/fiber/README.md`.
- **nostrc / libgo:** consider an atomic-swap snapshot helper (`watch`/`ArcSwap` analogue) on top of `refptr`.
- **Decision:** choose between §5.4 options 1, 2 and 3 before any code is written; if 2 or 3, open an epic with the §5.5 phases as children.

## Appendix A — how the numbers were measured

Run from each repo root on the commits named at the top. All counts are `wc -l` over `*.rs` excluding `target/`; "non-test" excludes paths matching `*/tests/*`, `tests.rs`, `*_tests.rs`, `testutil.rs`, `faults.rs`.

```sh
# fips: files / LoC
find . -name '*.rs' -not -path '*/target/*' | wc -l
find . -name '*.rs' -not -path '*/target/*' -print0 | xargs -0 cat | wc -l
# per-module, non-test (root crate; swap src→core/src for the core)
for d in src/*/; do printf "%-20s %s\n" "$d" "$(find "$d" -name '*.rs' -not \( -path '*/tests/*' -o -name 'tests.rs' -o -name '*_tests.rs' -o -name 'testutil.rs' \) -print0 | xargs -0 cat | wc -l)"; done | sort -k2 -nr
# concurrency
grep -rE 'async fn|\.await' --include='*.rs' src | wc -l
grep -rE 'tokio::spawn|tokio::task::spawn|spawn_blocking' --include='*.rs' src | wc -l
grep -rE 'select!' --include='*.rs' src | wc -l
grep -rhoE '(mpsc|broadcast|watch|oneshot)::(channel|unbounded_channel)' --include='*.rs' src | sort | uniq -c
grep -rE 'Arc<(Mutex|RwLock|tokio::sync::(Mutex|RwLock))' --include='*.rs' src | wc -l; grep -rc ArcSwap --include='*.rs' src | awk -F: '{s+=$2} END{print s}'
grep -rE '#\[(tokio::)?test' --include='*.rs' . | grep -v target | wc -l
# Noise patterns and DH
grep -rhoE 'Noise_[A-Za-z0-9_]+' --include='*.rs' core/src | sort -u
sed -n '404,419p' core/src/noise/handshake.rs
# history
git log --since='2026-10-01' --oneline | wc -l
git log --diff-filter=A --format='%ad %h' --date=short -- core/Cargo.toml | tail -1
for c in $(git log --format=%h --before='2026-06-30' -1); do git ls-tree -r --name-only $c | grep -c '\.rs$'; done

# nostrc
for d in libgo libgo/fiber libnostr libmarmot libjson nips; do printf "%-14s %s\n" "$d" "$(find "$d" \( -name '*.c' -o -name '*.h' \) -not -path '*/third_party/*' -not -path '*/_build*' -not -path '*/build/*' -print0 | xargs -0 cat | wc -l)"; done
grep -oE 'gof_[a-z_0-9]+\s*\(' libgo/fiber/include/libgo/fiber.h | sort -u          # no recvfrom/sendmsg
grep -oE 'nostr_nip(17|59)_[a-z_0-9]+' -rh nips/nip17/include nips/nip59/include | sort -u
grep -rhoE 'crypto_aead_chacha20poly1305_ietf_[a-z]+|secp256k1_ecdh|hkdf_(extract|expand)' --include='*.c' --include='*.h' libnostr nips libmarmot | sort | uniq -c

# metadesk
find . \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.m' \) -not -path './build*/*' -not -path './subprojects/*' -print0 | xargs -0 cat | wc -l
git -C subprojects/nostrc log -1 --format='%h %ad' --date=short
grep -n 'FIPS v0' docs/metadesk-spec.md; grep -n 'SOCKET' src/core/fips_control.h
```

## Appendix B — sources read

- `fips`: `Cargo.toml`, `core/Cargo.toml`, `core/README.md`, `core/src/lib.rs`, `core/src/README.md`, `core/src/proto/README.md`, `core/src/noise/README.md`, `core/src/noise/handshake.rs` (DH), `docs/design/fips-native-api.md`, `docs/reference/native-api.md`, `docs/reference/control-socket.md`, `docs/reference/nostr-events.md`, `docs/how-to/migrate-an-embedder-to-v0.6.md`, `CHANGELOG.md` (Unreleased), `testing/build-core-baremetal.sh`.
- `nostrc`: `libgo/include/{go,select,channel,context}.h`, `libgo/LIBGO.md`, `libgo/fiber/include/libgo/fiber.h`, `libgo/fiber/README.md`, `libgo/fiber/io/`, `libnostr/include/nostr-simple-pool.h`, `libnostr/include/nostr-relay.h`, `nips/nip17`, `nips/nip59`, `nips/nip44/include/nostr/nip44/kdf.h`, `nips/nip04/src/nip04.c`, `third_party/nostrdb/src/hkdf_sha256.h`.
- `metadesk`: `meson.build`, `docs/metadesk-spec.md` §2.2, `src/core/fips_control.h`, `src/core/fips_addr.h`, `src/host/main.c`, `docs/reviews/slop-audit-fips.md`.
