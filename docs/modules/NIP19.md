Examples
--------

- Multi-relay nrelay encode/decode
```c
const char *relays[] = { "wss://r.x.com", "wss://relay.example.com" };
char *bech = NULL;
nostr_nip19_encode_nrelay_multi(relays, 2, &bech);
// bech -> "nrelay1..."
char **out_relays = NULL; size_t cnt = 0;
nostr_nip19_decode_nrelay(bech, &out_relays, &cnt);
for (size_t i=0;i<cnt;++i) free(out_relays[i]);
free(out_relays);
free(bech);
```

- Unified pointer: build, encode, parse
```c
NostrNAddrConfig cfg = {
  .identifier = "my-id",
  .public_key = "<64-hex-pubkey>",
  .kind = 30023,
};
NostrPointer *p = NULL;
nostr_pointer_from_naddr_config(&cfg, &p);
char *bech2 = NULL; nostr_pointer_to_bech32(p, &bech2);
NostrPointer *p2 = NULL; nostr_pointer_parse(bech2, &p2);
nostr_pointer_free(p2);
free(bech2);
nostr_pointer_free(p);
```
NIP-19 Module (libnip19)
=========================

Status: stable • Tests: yes • Sanitizers: ASAN/UBSAN/TSAN

Overview
--------
`libnip19` implements the NIP-19 specification for bech32-encoded identifiers and TLV pointer types.
It exposes canonical C APIs (no wrappers) under `nips/nip19/include/`:
- `nip19.h`: top-level encode/decode helpers and TLV pointer APIs
- `nostr-pointer.h`: pointer data structures used by TLV encoders/decoders

Public API surface
------------------
Bare bech32
- `int nostr_nip19_encode_npub(const uint8_t pubkey[32], char **out)`
- `int nostr_nip19_decode_npub(const char *npub, uint8_t out_pubkey[32])`
- `int nostr_nip19_encode_nsec(const uint8_t seckey[32], char **out)`
- `int nostr_nip19_decode_nsec(const char *nsec, uint8_t out_seckey[32])`
- `int nostr_nip19_encode_note(const uint8_t event_id[32], char **out)`
- `int nostr_nip19_decode_note(const char *note, uint8_t out_event_id[32])`
- `int nostr_nip19_inspect(const char *bech, NostrBech32Type *out_type)`

TLV bech32 (shareable identifiers)
- Generic: `nostr_nip19_encode_tlv`, `nostr_nip19_decode_tlv`
- nprofile: `nostr_nip19_encode_nprofile`, `nostr_nip19_decode_nprofile`
- nevent:   `nostr_nip19_encode_nevent`,   `nostr_nip19_decode_nevent`
- naddr:    `nostr_nip19_encode_naddr`,    `nostr_nip19_decode_naddr`
- nrelay:   `nostr_nip19_encode_nrelay`,   `nostr_nip19_decode_nrelay`

TLV mapping per spec
--------------------
- nprofile
  - T=0: 32-byte pubkey
  - T=1: relay (ascii), repeatable
- nevent
  - T=0: 32-byte event id
  - T=1: relay (ascii), repeatable
  - T=2: 32-byte author pubkey (optional)
  - T=3: u32 kind (big-endian, optional)
- naddr
  - T=0: identifier string ("d" tag)
  - T=1: relay (ascii), repeatable
  - T=2: 32-byte author pubkey
  - T=3: u32 kind (big-endian)
- nrelay (deprecated):
  - T=1: relay (ascii), repeatable

Relationship to other libraries
-------------------------------
`libnip19` is glue between human-shareable strings and internal data models.

- libjson (`libnostr_json`)
  - Provides `Filter` and `Event` JSON (de)serialization.
  - Outputs/inputs hex keys and binary fields required by NIP-01.
  - Use TLV-decoded pointers to build filters:
    - nevent: set `ids = [id]`; optionally `authors = [author]`; optionally `kinds = [kind]`.
    - naddr: set `authors = [author]`, `kinds = [kind]`, and `#d = [identifier]`.
    - nprofile: usually implies `authors = [pubkey]`.

- libnostr (relay/subscription)
  - `relay_connect()`/`relay_close()` manage connections.
  - `subscription_create()`/`relay_subscribe()` publish REQ with `Filter`.
  - Use pointer `relays[]` to select preferred targets:
    - If present, connect/subscribe to those relays first.
    - Otherwise, fall back to app/relay list.

- Examples/tests
  - `relay_smoke` shows typical flow: decode a pointer, derive filters, connect, subscribe, print events, clean shutdown.
  - `test_nip19_tlv` verifies TLV round-trips and robustness.

Memory management
-----------------
All TLV pointer structs are heap-owned and must be freed:
- `NostrProfilePointer *nostr_profile_pointer_new(void)` / `nostr_profile_pointer_free()`
- `NostrEventPointer   *nostr_event_pointer_new(void)`   / `nostr_event_pointer_free()`
- `NostrEntityPointer  *nostr_entity_pointer_new(void)`  / `nostr_entity_pointer_free()`

Encode/Decode consume/produce copies; the caller owns the returned bech32 string or pointer struct.

Usage patterns
--------------
- Decode and query
```c
NostrEventPointer *ep = NULL;
if (nostr_nip19_decode_nevent(bech, &ep) == 0) {
  // Build a Filter from ep
  // ids[0] = ep->id; authors[0] = ep->author (if set); kinds[0] = ep->kind (if set)
  // Prefer ep->relays when choosing relays
  nostr_event_pointer_free(ep);
}
```

- Encode from internal state
```c
NostrEntityPointer *ap = nostr_entity_pointer_new();
ap->identifier = strdup(d_tag);
ap->public_key = strdup(pubkey_hex);
ap->kind = kind;
// optionally add relays
char *bech = NULL;
nostr_nip19_encode_naddr(ap, &bech);
// use bech, then free
free(bech);
ostr_entity_pointer_free(ap);
```

Validation & robustness
-----------------------
- Unknown TLVs are ignored per spec.
- Length/type checks enforced (e.g., T=0 for keys/ids must be 32 bytes).
- Strings are bounded to 255 bytes as required by TLV L=uint8.
- Hex parsing uses `nostr_hex2bin()`; bech32 uses `nostr_b32_*` helpers.

Troubleshooting
---------------
- If REQ yields no events, verify the filter derived from pointer matches the intent (ids/authors/kinds/d tag).
- When multiple relays are present, ensure connection policy respects `relays[]` order or app policy.
- Ensure to free all pointer fields and arrays to avoid leaks (tests cover this under sanitizers).
