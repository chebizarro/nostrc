# fipsd 0.1.0 release notes

**Tag:** `fipsd-v0.1.0` · **Date:** 2026-11-20 · **Upstream pin:**
[`jmcorgan/fips@6aa80504`](https://github.com/jmcorgan/fips/commit/6aa80504baa0fa312b392009eea0e3e59fb6fe4c)
(`0.6.0-dev`, see [`libfips-core/UPSTREAM.md`](../../libfips-core/UPSTREAM.md))

First release of the C implementation of [FIPS](https://github.com/jmcorgan/fips),
the Nostr-native mesh daemon, built on nostrc's `libnostr`, `libgo` and fiber
runtime. It is wire-compatible with the Rust daemon at the pinned commit and
interoperates with it over every transport that can be tested here.

## What ships

All components are versioned 0.1.0 in [`VERSION_MANIFEST.md`](../../VERSION_MANIFEST.md)
and ship together in the fipsd packages.

| Component | What it is |
|---|---|
| `fipsd` | the daemon |
| `libfips-core` | sans-IO protocol core: codecs, identity, Noise IK/XK, spanning tree, bloom filters, routing, lookup, FMP/FSP, rekey, MMP, probes, multipath |
| `libfipsnative` | C client library for the native datagram API |
| `fipsctl` | control client (CLI and output match upstream) |
| `fipstop` | ncurses dashboard |
| `fips-gateway` | Linux LAN gateway (nftables) |
| `fips-ble-agent` | macOS CoreBluetooth agent |

**Features:** UDP, TCP, Tor/SOCKS5, Nym, Ethernet and BLE (BlueZ, CoreBluetooth)
transports; Nostr rendezvous (adverts, STUN, NAT traversal) and mDNS discovery;
TUN with the IPv6 shim; the `.fips` DNS responder; the native datagram API;
ACLs, metrics and stats; the tick profiler.

**Platforms:** Linux (x86_64, aarch64), macOS (arm64), Windows 11 ARM64
(service, TUN via Wintun, TCP control socket, ZIP). Packages: Debian/Ubuntu
`.deb`, RPM, Arch, macOS `.pkg` with launchd; Windows ZIP.

## Compatibility

- Byte-exact against vectors exported from the pinned Rust code, with every
  vector family replayed in CI: entropy, identity, codec, noise (83/83), proto
  (28/28), rekey, and legacy rekey traces for 0.5.0–0.5.3.
- C↔Rust interop is tested in CI for UDP (IPv4, IPv6), TCP (including rekey
  under saturation), multi-node topologies, the native API and `fipsctl`.
  Tor, mDNS, Ethernet, BLE (virtual controllers) and NAT traversal were proven
  against the Rust daemon during development.
- Known upstream behaviours are recorded in `libfips-core/UPSTREAM.md`, not
  "fixed" in C. One example: FIPS v1 passes empty AAD to the handshake AEAD.

## Performance

Measured on 2026-11-20 with `fipsd/tests/perf_loopback.py`: two daemons on
loopback UDP, 1200-byte native-API datagrams, 2 workers each. The baseline is
`5b3c987c8`, before the optimization pass (`nostrc-u19cz`). Runs alternated in
one session on a shared host, so treat ranges as indicative.

| Apple M4 Max (macOS) | Before the pass | 0.1.0 | Rust↔Rust, same runs |
|---|---|---|---|
| C↔C unpaced delivered | 594–680 Mbit/s | **819–1,324 Mbit/s** | 103–705 Mbit/s (55–91% loss) |
| C↔C sustainable (<1% loss) | 244–650 Mbit/s | **506–1,200 Mbit/s** | 24–262 Mbit/s |
| CPU µs/datagram at 100 Mbit/s, tx / rx | 61–63 / 59–61 | **42–43 / 39–42** | 27–40 / 42–48 |
| One-way latency p50 at 100 Mbit/s | 94–97 µs | 64–91 µs | 73–146 µs |

| Linux aarch64 VM (10 vCPU) | Before the pass | 0.1.0 | Rust↔Rust, same runs |
|---|---|---|---|
| C↔C unpaced delivered | 494–540 Mbit/s | **1,009–1,163 Mbit/s** | 82–92 Mbit/s (97% loss) |
| C↔C sustainable (<1% loss) | 394 Mbit/s | **800 Mbit/s** | 197–356 Mbit/s |
| CPU µs/datagram at 100 Mbit/s, tx / rx | 85 / 69–71 | **60 / 52–53** | 54–68 / 70–99 |
| One-way latency p50 at 100 Mbit/s | 326–380 µs | 294–298 µs | 256–347 µs |

What changed in the pass:
- **AEAD:** ChaCha20-Poly1305 moved to OpenSSL EVP (NEON/AVX) for payloads of
  256 bytes or more, about 2.1× faster at 1400 bytes.
- **Crypto threads:** packet crypto runs on dedicated OS threads, inline when
  that is cheaper.
- **Worker-side send:** the encrypting worker sends data-lane UDP frames itself,
  and per-peer wire order is preserved.
- **Fused open:** one job opens both the link and the session layer of an
  inbound frame.
- **Linux batching:** `recvmmsg`, plus `sendmmsg`/UDP GSO.
- **Native delivery:** batched, with direct client writes.
- **Hot path:** cached timer deadlines; exact-extent wipes; no per-datagram
  `fcntl`.
- **Moderate load:** inline crypto while only a short backlog is queued, and
  libgo skips redundant idle-worker wakes on Linux.

Details and the full measurements are in [`perf.md`](perf.md).

**Mixed pairs under overload.** The pinned Rust daemon's receive path collapses
when it is offered more than it can absorb. Rust↔Rust delivers about 55 Mbit/s
at 98% loss in the unpaced CI benchmark. The 0.1.0 C sender is fast enough to
push it there too: on hosted CI, unpaced C→Rust went from 421 Mbit/s (old C
sender, just under the collapse point) to 58 Mbit/s, equal to Rust→Rust.

This only affects senders that exceed the Rust receiver's capacity without
backing off, such as the synthetic unpaced benchmark. TCP over the TUN adapts
to loss. At paced rates the Rust receiver can sustain, C→Rust delivers normally.

## Security

- **Reviews:** a Noise audit with backend addenda
  ([`docs/reviews/libfips-core-noise-audit-2026-11-15.md`](../../docs/reviews/libfips-core-noise-audit-2026-11-15.md)),
  a security review
  ([`docs/reviews/fips-port-security-review-2026-11-15.md`](../../docs/reviews/fips-port-security-review-2026-11-15.md))
  with its high and medium findings fixed, and a Nostr protocol review
  ([`docs/reviews/fips-nostr-protocol-review-2026-11-16.md`](../../docs/reviews/fips-nostr-protocol-review-2026-11-16.md)).
- **CI:** every protocol codec has a libFuzzer harness, smoke-run in CI. CI also
  runs ASan+UBSan and TSan on Linux and macOS, a GCC `-O3 -Werror` leg, and a
  key-residue test for the OpenSSL backend.
- **Constant time:** checked with a constant-time probe for the AEAD, run by
  hand (`fips_core_ct_probe`).

## Known limitations

- A real Mac↔Linux Bluetooth link hasn't been tested (no Linux machine with an
  adapter here); BLE is proven with BlueZ virtual controllers and the macOS
  radio (`nostrc-gz960`).
- Nym has not run against a real mixnet (`nostrc-dal5p`).
- On Windows, UDP data frames are sent from the dispatcher, not the crypto
  workers (`nostrc-akn6b`).
- FreeBSD/pfSense packaging isn't done (`nostrc-iq1n9.7`). Peer settlement
  (`settle`) waits for upstream to wire it (`nostrc-f12v2.7`).
- CI builds only the default OpenSSL AEAD backend, not the libsodium fallback
  (`nostrc-x1u8j`).
- The TCP rekey-under-saturation tests are load-sensitive (`nostrc-4im43`).
- The remaining performance follow-ups are filed under `nostrc-u19cz`.