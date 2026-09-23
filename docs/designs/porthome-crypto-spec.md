# `porthome` crypto — Phase 1 primitive spec

**Status:** EXPERIMENTAL. Unreviewed. Gated behind `NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL` (default `OFF`). Do not deploy.
**Scope:** the on-wire byte layout and key derivation used by `libnostr_porthome` (the Phase-1 library primitives for `docs/designs/home-from-relay.md` §1, §2, §4). One document, one review target.
**Where used:** encrypting file chunks and manifest nodes for upload to Blossom; hashing filenames for the encrypted-path form stored inside manifest entries.
**Bead:** `nostrc-h10m` (E-portable-home).

This spec is a **Phase-1 realisation of D4** with a deliberate scope shrink: OpenSSL EVP `ChaCha20-Poly1305` instead of hand-rolled NIP-44 v2 sealing, because the properties the design asks of the blob layer are (deterministic key + deterministic nonce + AEAD tag), not "NIP-44 on the outer envelope". NIP-44 remains the correct choice for the *relay pointer body* (§2.2 of the design) once Phase 2 wires the identity keypair through; that envelope is not implemented here.

---

## 1. Notation

- `HKDF-SHA256(salt, ikm, info, L)` — RFC 5869 with SHA-256, extract-then-expand, output length `L` bytes. Implemented via `nip44_hkdf_extract` / `nip44_hkdf_expand` (in-tree, `nips/nip44/src/core/nip44_hkdf_hmac.c`).
- `HMAC-SHA256(key, msg)` — RFC 2104, 32-byte output. Implemented via `nip44_hmac_sha256`.
- `SHA256(x)` — 32-byte digest.
- `ChaCha20-Poly1305(key, nonce, aad, pt)` — RFC 8439 AEAD, 32-byte key, 12-byte nonce, 16-byte tag. OpenSSL `EVP_chacha20_poly1305()`.
- All ASCII string constants below are UTF-8, no NUL terminator on the wire, no trailing whitespace.

---

## 2. Domain-separation constants

Every HKDF call fixes both `salt` (the "purpose") and `info` (the "input binding"). The four purposes used in Phase 1 are:

| Purpose | `salt` (ASCII) | Notes |
| --- | --- | --- |
| home-key derivation | `porthome/v1/home` | `ikm = seed[32]` (see §3) |
| chunk sealing | `porthome/v1/chunk` | `ikm = home_key`, `info = SHA256(plaintext_chunk)` |
| manifest-node sealing | `porthome/v1/manifest` | `ikm = home_key`, `info = SHA256(plaintext_cbor)` |
| name-key derivation | `porthome/v1/name` | `ikm = home_key`, `info = ""` (empty) |

The `v1` in each string is the **layout version** covered by this document. If any byte layout below changes, the salt bumps to `v2` and the on-wire version byte (§5) bumps in lockstep — old and new blobs are then key-disjoint.

`ikm` for chunk / manifest / name is always the **32-byte `home_key`**, never the raw seed. `home_key` sits above the seed by exactly one HKDF hop (§3), so a leaked `home_key` reveals only that home's blobs, and a leaked seed can regenerate `home_key` but nothing above the seed.

---

## 3. Key hierarchy

```
seed[32]                       <- for Phase 1: caller-provided fixture bytes.
                                  Production (Phase 2, out of scope here): the
                                  broker requests the wrap-key from the signer
                                  via NIP-46 nip44_decrypt of the `wrapped_home_key`
                                  field of the kind-30078 pointer (design §4.3);
                                  that decrypted 32-byte payload is `seed`.
     |
     | home_key = HKDF-SHA256(
     |               salt = "porthome/v1/home",
     |               ikm  = seed,
     |               info = "",
     |               L    = 32)
     v
home_key[32]                   <- kept in an mlock'd page in Phase 2. In Phase 1 it
                                  lives on the caller's stack; the library never
                                  writes it to disk or logs.
     |
     +-- chunk_nonce = HKDF(salt="porthome/v1/chunk",    ikm=home_key, info=SHA256(pt),    L=12)
     |   chunk_key   = HKDF(salt="porthome/v1/chunk",    ikm=home_key, info=chunk_nonce,   L=32)
     |
     +-- mani_nonce  = HKDF(salt="porthome/v1/manifest", ikm=home_key, info=SHA256(pt),    L=12)
     |   mani_key    = HKDF(salt="porthome/v1/manifest", ikm=home_key, info=mani_nonce,    L=32)
     |
     +-- name_key   = HKDF(salt="porthome/v1/name",     ikm=home_key, info="",         L=32)
           -> per-component tag  = HMAC-SHA256(name_key, component_utf8)
           -> encrypted component = lowercase-hex(tag[0..24])   # 48 hex chars
```

The derivation is two-stage on purpose. The nonce is bound to `SHA256(plaintext)`; the key is then derived from that nonce. Convergence enters at the nonce (same plaintext under the same `home_key` → same nonce → same key → same ciphertext and tag), and the receiver — which sees the nonce on the wire (§4) but never `SHA256(plaintext)` — reproduces the key from the nonce and validates the AEAD. A distinct plaintext always yields a distinct nonce (barring a SHA-256 collision) and hence a distinct key.

`chunk_key` and `mani_key` are cryptographically disjoint from one another because their salts differ (`porthome/v1/chunk` vs `porthome/v1/manifest`). A ciphertext produced under one purpose cannot be redirected to the other's decrypt path even if an attacker can force the same `SHA256(plaintext)` prefix.

`name_key` is derived once per home. Names are keyed but not encrypted per-message: the encrypted-name form is `HMAC-SHA256(name_key, component)[0..24]` rendered as 48 lowercase hex chars, per component, joined with `/`. This gives **stable per-home lookup** (the same filename always hashes the same, so a manifest re-encode is idempotent) and **cross-home disjointness** (two homes with the same filename produce different tags because their `name_key` bytes differ). We truncate to 24 bytes (192 bits) — comfortably above birthday-collision territory for any home size we cap in the design (500k entries).

**We never store or transmit the raw filename.** The path field inside a manifest entry is the slash-joined 48-hex-per-component string.

---

## 4. Byte layout of a sealed blob

Every sealed blob (chunk or manifest-node) is one byte layout:

```
offset  size  field
   0     1    version                  (0x01)
   1    12    nonce                    (HKDF-derived, §3)
  13     N    ciphertext               (same length as plaintext)
 13+N   16    poly1305 tag             (AEAD authentication tag)
```

Total sealed size = `plaintext_len + 29` bytes. There is no length prefix inside the blob — the recipient learns the length from Blossom (`Content-Length` / stored size). There is no separate AAD; the version byte is bound implicitly by the receiver rejecting `version != 0x01` before touching the AEAD.

The Blossom address of a chunk is `sha256(sealed_blob)`, i.e. `SHA256(version || nonce || ct || tag)`. Because both `nonce` and `ct` are deterministic functions of `plaintext` under a fixed `home_key`, the address is deterministic per (plaintext, home_key). A different `home_key` produces a different `chunk_key` **and** a different `chunk_nonce`, so a different address — no Blossom server can confirm "this home stores the same file as that home". This is the per-home convergence property from D4.

`chunk_key_id` in the manifest schema (`nh_porthome_chunk.chunk_key_id`) is reserved for **future** key-rotation (`key_epoch` in the pointer, design §4.1); Phase 1 always writes `0`. Decoders MUST refuse any non-zero value they do not have keying material for.

---

## 5. Decode-time strictness

Decoders MUST refuse a sealed blob if:

1. total length `< 29` bytes,
2. `version` byte is not `0x01`,
3. AEAD verification fails (Poly1305 tag mismatch).

There is no fallback path, no permissive mode, no length-mismatch tolerance. AEAD failure is fatal: the caller retries against another Blossom server or fails closed, per design §5.4.

The chunk address is verified **before** decryption — the fetcher computes `SHA256(bytes)` on receipt and compares to the requested hash; a mismatch is a hard error (`NH_PORTHOME_ERR_HASH_MISMATCH`) and the AEAD is never invoked on adversary-controlled bytes. This is the "verify the hash before decryption" invariant from design §8.2.

---

## 6. Threat-model delta vs. per-file random keys

Choosing convergent (key, nonce) derived from `SHA256(plaintext)` — instead of a fresh key per file — changes exactly three things versus fully-random AEAD:

**(a) Deduplication becomes possible.** A re-uploaded unchanged chunk hits the same Blossom address and turns into a cheap HEAD. This is the whole reason for the construction — see design §2.5 and §5.2.

**(b) A local plaintext-guess check leaks to whoever has both the ciphertext address and a candidate plaintext.** If an adversary already possesses a plaintext file and has read access to a Blossom server that hosts this home, they can compute `sha256(plaintext)` → derive `chunk_key` and `chunk_nonce` **only if they also hold `home_key`**. Without `home_key` they can compute nothing. The plaintext-guess attack that plain convergent encryption suffers (identity-of-file confirmation across users) does not apply here because `home_key` is per-account and never leaves the account's device. So the threat delta relative to random AEAD is: **an attacker who steals `home_key` gets confirmation-of-known-plaintext on all this home's blobs.** That attacker already has decryption for every blob, so the "confirmation of known plaintext" bit is subsumed and adds nothing.

**(c) The Blossom server sees the same address twice for the same plaintext, both within a home and across a home's lifetime.** That is a metadata signal: "the user has the same-content file X on machine A and machine B" is observable by any server that sees both uploads. This is the intended dedup behaviour and is priced in.

**Nonce-reuse hazard.** ChaCha20-Poly1305 catastrophically fails if the same (key, nonce) pair is reused across two *different* plaintexts. In this construction the (key, nonce) pair is a deterministic function of `SHA256(plaintext)`, so identical plaintexts share a (key, nonce) pair by design (the dedup property) and different plaintexts have different (key, nonce) pairs by construction, save for a SHA-256 collision. A SHA-256 collision on any two plaintexts a single home has ever stored is well outside the assumed adversary budget for this system. If SHA-256 is broken, this construction is broken along with the vast majority of the Nostr stack; that is an acceptable coupling.

**What this construction does not do.** It does not authenticate the *sequence* of chunks against tampering by an attacker who can swap two legitimate ciphertexts of this home. The manifest above binds the ordered list of chunks per file with their `sha256` fields, and the manifest is itself sealed and its address referenced from the (Phase-2) kind-30078 pointer, so tampering must climb up to the pointer level, which is signed by the account key. Design §8.2's "manifest substitution" analysis applies unchanged.

---

## 7. Name encryption (§3, `name_key` branch)

**Property required:** deterministic per (home_key, component), disjoint across `home_key`s, collision-free at the scale of the largest allowed home.

**Construction:** `enc(c) = lowercase-hex( HMAC-SHA256(name_key, c)[0..24] )`. 24 bytes → 48 hex chars → 192-bit collision domain. At 500,000 entries per home (design §5.4 cap), collision probability is `~500k^2 / 2^193 ≈ 10^-47`. Ignorable.

**What it is not.** It is not an encryption in the standard sense: an attacker who holds `name_key` can enumerate the whole English filesystem dictionary and check for matches. That is acceptable because `name_key` is derived from `home_key`, so possession of `name_key` implies possession of every ciphertext and every chunk key. We chose a keyed hash rather than a proper deterministic encryption because we never need to invert it — the decoder always fetches the encrypted name from the manifest and stores it back as-is; the plaintext filename lives inside the manifest node body, which is AEAD-sealed (§4).

**Path assembly.** A path `/a/b/c` becomes `<enc(a)>/<enc(b)>/<enc(c)>`. Absolute-path prefix `/` is never emitted; a leading `/` in the source is stripped before component splitting. `.` and `..` are refused at encode time and refused at decode time — decoders MUST reject any manifest entry whose reconstructed path contains any of `..`, `.`, `/./`, `/../`, or a leading `/`. The path field in a manifest entry is validated by `nh_porthome_manifest_decode`; §5's D5 label-callback consumer (Phase 2) re-validates.

---

## 8. Reference API

The following are the Phase-1 public entry points (see `include/nh_porthome_crypto.h`):

```c
int nh_porthome_key_derive(const uint8_t seed[32], uint8_t out_home_key[32]);

int nh_porthome_encrypt_chunk(const uint8_t home_key[32],
                              const uint8_t *pt, size_t pt_len,
                              uint8_t **out_ct, size_t *out_ct_len,
                              uint8_t out_sha256[32]);
int nh_porthome_decrypt_chunk(const uint8_t home_key[32],
                              const uint8_t *ct, size_t ct_len,
                              uint8_t **out_pt, size_t *out_pt_len);

int nh_porthome_encrypt_manifest(const uint8_t home_key[32],
                                 const uint8_t *pt, size_t pt_len,
                                 uint8_t **out_ct, size_t *out_ct_len);
int nh_porthome_decrypt_manifest(const uint8_t home_key[32],
                                 const uint8_t *ct, size_t ct_len,
                                 uint8_t **out_pt, size_t *out_pt_len);

int nh_porthome_encrypt_name(const uint8_t home_key[32],
                             const char *component,
                             char out_hex49[49]);
int nh_porthome_encrypt_path(const uint8_t home_key[32],
                             const char *path,
                             char **out_joined);
```

All success paths return `0`. On failure the return is `< 0`, output pointers are set to `NULL`/`0`, and any partially-written buffers are `OPENSSL_cleanse`d.

---

## 9. What this spec deliberately does not cover

- The **relay pointer body** (kind 30078, self-encrypted NIP-44 v2 wrap of the pointer JSON — design §2.2). Phase 1 exposes convergent AEAD only; the NIP-44 wrap on the pointer lands with the Phase-2 provisioner, which is where the identity keypair actually enters the picture.
- The **wrap-key → home-key indirection at the pointer level** (design §4.1). Phase 1's `nh_porthome_key_derive` takes an already-materialised 32-byte seed; Phase 2 will source that seed from `wrapped_home_key` via NIP-46 `nip44_decrypt`, which is *not* implemented here.
- **Key rotation.** `chunk_key_id` is reserved and always zero in Phase 1. Rotating to a new `key_epoch` requires re-encrypting every chunk under a new `home_key`; that is a Phase-5 workflow.
- **Padding and metadata-hiding beyond AEAD** (design §8.3 buckets). The sealed-blob layout above is 29 bytes of overhead over the plaintext; padding to power-of-two buckets is a caller responsibility left to Phase 2.
- **fscrypt / plaintext-at-rest** (D16). Out of scope for this spec.

---

## 10. Review checklist for D4

Reviewers of D4 should confirm each of the following against §3–§7:

- [ ] The four HKDF `salt` strings are unambiguously distinct ASCII, contain a version tag, and match `docs/designs/porthome-crypto-spec.md` §2 verbatim.
- [ ] The two-stage derivation (nonce from `SHA256(plaintext)`, key from that nonce, both under the same `salt`) is what §3 and the reference implementation use — a single 44-byte HKDF whose `info` was `SHA256(plaintext)` would give the sender both pieces at once but leave the receiver unable to reproduce the key from wire bytes.
- [ ] The sealed-blob layout (`version || nonce || ct || tag`, 29-byte overhead) is fixed for `v1` and any change bumps both `salt` version and the on-wire `version` byte together (§4, §2).
- [ ] The Blossom address of a chunk is `SHA256` of the *sealed* bytes, not the plaintext; the fetcher verifies address-before-decrypt (§5).
- [ ] The nonce-reuse hazard analysis in §6 correctly locates the only failure to a SHA-256 collision, and no operational path can bypass the HKDF `info` binding.
- [ ] Name encryption (§7) is a keyed hash, not a promise of confidentiality against an attacker with `name_key`; that trade is documented and accepted here.
- [ ] The Phase-1 API surface (§8) does not expose the seed after derivation and cleanses failure-path buffers.
