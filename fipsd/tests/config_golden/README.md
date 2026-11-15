# Configuration reference snapshots

Oracle: the supplied upstream `docs/reference/configuration.md` and
`src/config/{node,transport,peer,gateway,floors}.rs` at the compatibility pin
`6aa80504baa0fa312b392009eea0e3e59fb6fe4c` (see libfips-core/UPSTREAM.md).
**No Rust binary dump was used.** `cli-fips.md` exposes neither a check-config
nor effective-config dump flag. Goldens were assembled independently from the
reference's tables and Ruby/Psych-parsed fixture overrides, not captured from
the C implementation. Table defaults/types were reviewed against the Rust
structs/default methods (including effective getters for optional fields).

Hand-checked groups:

* Limits: 256/128/256/1000 connections/peers/links/pending; max_sessions 1024
  from Rust (not yet tabulated in the reference).
* Rates: 100/10 burst/refill, 30 s timeout, 1000 ms initial resend, 2 backoff,
  5 resends, 64/16 setup bucket; established overrides stay absent (derived
  core values 128/6.4 at defaults, checked separately).
* Cache: 50000 entries/300 s/10000 identities; lookup 64/[1,2,4,8]/10/0/0/2.
* Rendezvous: Nostr disabled, advertise enabled, configured_only; offers
  16/4, signal 120 s, attempt 10 s, replay 300 s; LAN disabled.
* Rekey: 120 s / 65536 messages; MMP full/30 s/32 samples in both scopes.
* Buffers: 1024/1024/64/4096. Native API off, 16 pending/16 backlog/256 flows.
* UDP: 1357 MTU, 2 MiB buffers, accept=true/public=false/outbound=false.
  TCP: 1400 MTU, 5000 ms connect, nodelay=true, keepalive 30 s, 256/8 limits.
* Ethernet: 0x2121, auto MTU absent, 2 MiB buffers, listen=true and other
  discovery booleans false, beacon 30 s, optional=false.
* TUN false/fips0/1280; DNS true/::1/5354/300. Gateway DNS [::1]:5365,
  upstream [::1]:5354, TTL/grace 60; conntrack 432000/30/180/30.
* Tor/Nym/BLE defaults are taken from their configuration reference tables,
  including Tor's optional directory-service block (absent unless requested).
* Paths: false/2/true, switch 1.3/2 s, probes 200/1000 ms; role normal and
  UDP/TCP kind absent. Optional grants UID/GID absent, ports [], keys/learn false.

Exceptions are explicit: default socket resolution is host-dependent and uses
`@auto@` in comparisons; search-path order has separate tests, while the
per-host socket selection branches are source-inspected.
The upstream default relay prohibited by nostrc's AGENTS.md is omitted (the
remaining two relays keep their order). Binary64 values use `%.17g`, so e.g.
0.2 displays its full round-trip decimal value. Secrets are always redacted.

All 33 source files have comment-only lines removed and trailing blank lines normalized in `config_fixtures/`.
No active YAML field was changed. The two `node.template.yaml` files contain
unexpanded `{{PEERS}}` and reject parsing. Chaos scenario and static topology
files describe harnesses, not daemon schemas; their unknown root fields warn
and their partial-document effective dump is defaults, not a claim that a
harness scenario was materialized into running nodes. File loading separately
refuses files which configure nothing. Upstream SHA-256 hashes are in
`SOURCES.sha256`; the optional live-checkout tests replay the original bytes.

`FipsdConfigTest` independently checks all protocol floors/caps, invalid semantic
constraints (several full upstream error strings pinned bytewise), explicit
nulls, defaults, layering, reader lifetimes and core parameter mapping.
`FipsdProcessTest` exercises the production startup/SIGHUP/error-log path.
`config_corpus/` is deterministic malformed-input replay, not a fuzz target.
