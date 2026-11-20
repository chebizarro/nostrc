# Pinned Nostr v0.44 captures

`v044/*.json` are byte-for-byte copies of the four files at
`jmcorgan/fips@6aa80504baa0fa312b392009eea0e3e59fb6fe4c`, paths
`src/nostr/fixtures/v044/{advert,inbox-list,offer-wrap,answer-wrap}.json:1-4`.
Upstream provenance says captured together on 2026-11-12 from `fe43c61d`.
The contract is `src/nostr/tests/fixtures.rs:27-331`; fixture-body construction
is at 107-198 and the assertions at 235-331. MIT license: `../../LICENSE.upstream`.
All signing/recipient keys are public BIP-340/FRC-1601 TEST keys. Never deploy.

Do not edit signed capture contents or replace their relay URLs: signatures
and ids cover them. They are immutable differential inputs, not defaults,
configuration, examples or recommended relays. No relay is contacted.

The root `fips-mesh` git dependency compiles at the pin. Its private test
builders/loaders are inaccessible to dependencies, so these captures are
transcribed verbatim, not re-generated. `src/nostr.rs` uses that dependency's
real public payload types and nostr 0.44 crypto to parse, open and verify them,
then the ordinary deterministic `export` emits `nostr.json` and `.bin`
sidecars. Each descriptor contains pin/version/length/hash/hex provenance.
The family also pins RelayUrl display normalization and root-slash equality.
