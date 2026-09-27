# nostr-seal

Encrypt files **for Nostr public keys** (npubs) or a **passphrase**, without
the sealing or opening tool ever holding your nsec. Bead `nostrc-da9c`.

```sh
nostr-seal encrypt --to npub1alice… --to npub1bob… report.pdf     # → report.pdf.nsealed
nostr-seal encrypt --to-self --to npub1bob… report.pdf             # include your signer identity
nostr-seal encrypt --passphrase notes.txt                          # NIP-49 passphrase instead
tar c photos/ | nostr-seal encrypt --to npub1… -o photos.tar.nsealed -
nostr-seal inspect report.pdf.nsealed                              # who can open it
nostr-seal decrypt report.pdf.nsealed                              # via org.nostr.Signer → report.pdf
nostr-seal decrypt --identity npub1bob… -o - report.pdf.nsealed | less
```

Double-clicking a `.nsealed` file in Files runs `nostr-seal-gtk` (built when
libadwaita ≥ 1.5 is available): it lists the recipients, preselects the
signer's active identity (`GetPublicKey`), and opens the file next to the
sealed one after the signer approves the request.

### Defaults (`~/.config/nostr/seal.conf`)

Optional; `$NOSTR_SEAL_CONFIG` overrides the path. Edited by the Files page
of Nostr Settings (`org.nostr.Settings`) or by hand — see
`data/seal.conf.example` (installed to `/usr/share/doc/nostr-seal/`).

```ini
[seal]
default_recipients=npub1alice…;npub1bob…   # used when encrypt names no --to/--to-self/--passphrase
include_self=true                          # …plus your signer identity
work_factor=16                             # scrypt log2(N) for --passphrase without --work-factor (16..20)
```

Defaults never mix with explicit recipients: any `--to`, `--to-self` or
`--passphrase` on the command line ignores `default_recipients` /
`include_self`. When the defaults are used the CLI says so on stderr. A
malformed file or recipient makes `encrypt` fail rather than seal for a
different set of people.

## Why not "just NIP-44"?

NIP-44 v2 is a *message* cipher: plaintext is capped at 65 535 bytes, it
pads to hide length, and it has no streaming or random access. It is the
right tool for wrapping a 32-byte key and the wrong one for a 4 GB video.
So nostr-seal uses each primitive for what it is reviewed for:

| Job | Primitive (reused, not reimplemented) |
|---|---|
| Key agreement with an npub | NIP-44 v2 conversation key: `HKDF-extract(SHA-256, IKM = ECDH(secp256k1) shared x, salt = "nip44-v2")` — `nostr_nip44_convkey` |
| Wrapping the file key for a recipient | a standard NIP-44 v2 payload of the 32-byte file key — `nostr_nip44_encrypt_v2_with_convkey` |
| Wrapping the file key for a passphrase | NIP-49 (scrypt → XChaCha20-Poly1305): the stanza *is* an `ncryptsec1…` string |
| Bulk encryption | the porthome D4 sealed-blob container (`docs/designs/porthome-crypto-spec.md`, `nh_porthome_crypto.c`) rooted at the file key |

No new KDF, MAC or AEAD construction is introduced.

## Trust model

* **Confidentiality**: only holders of a listed recipient's secret key, or of
  the passphrase, can recover the file key.
* **Integrity**: every byte is authenticated. The header (including every key
  stanza), chunk order, chunk count and total length are bound by the sealed
  index; a flipped, swapped, dropped, truncated or appended byte is rejected.
* **Sender is anonymous.** The key agreement uses a fresh ephemeral key per
  stanza, so anyone can seal to an npub and nothing says who did. Sign the
  `.nsealed` (or the kind-1063 event announcing it) if authorship matters.
* **Recipients can forge for each other.** Anyone who can open a
  multi-recipient file knows its file key and could produce a different file
  the other recipients would also accept (same as age). Recipients are not
  authenticated to each other.
* **Recipient list is visible.** Each NIP-44 stanza names its recipient's
  x-only pubkey in clear so the right identity can be picked without trial
  decryption through the signer (which would mean one approval prompt per
  stanza). Chunk count / total size are visible from the file length.
* **Passphrase files have exactly one stanza**; mixing a passphrase with npub
  recipients is refused on both ends (a passphrase file must not be openable
  by anyone else, and vice versa).

## Signer seam — the nsec stays in the daemon

Sealing needs no signer at all (ephemeral sender key). Opening does:

1. `GetPublicKey` (or `--identity npub1…`) picks the identity; the tool finds
   the stanza naming it.
2. `org.nostr.Signer.NIP44DeriveConversationKey(ephemeral_pubkey_hex, identity,
   "org.nostr.Seal")` (nip55l ≥ 0.3.0) returns the conversation key with the
   stanza's **ephemeral** key. It goes through the signer's approval flow (ACL
   section `[NIP44DeriveConversationKey]`, else an `ApprovalRequested` prompt
   of kind `nip44_conversation_key`). Because the peer is a throwaway key, the
   returned key opens this one stanza and nothing else.
3. nostr-seal opens the NIP-44 payload locally and gets the file key.

Signers that predate the method answer `UnknownMethod`; nostr-seal then asks
them to open the stanza payload directly with `NIP44DecryptB64` (it is an
ordinary NIP-44 v2 payload, so any NIP-44-capable signer — including a NIP-46
bunker via `nip44_decrypt` — can open it).

## Format (`.nsealed`, version 1)

MIME type `application/vnd.nostr.sealed`, glob `*.nsealed`, magic
`NSEALED\x01` at offset 0. All integers are big-endian.

```
file   = header ‖ chunk_0 ‖ … ‖ chunk_{n-1} ‖ sealed_index ‖ footer
```

### Header

| Offset | Size | Field |
|---|---|---|
| 0 | 7 | magic `"NSEALED"` |
| 7 | 1 | format version = `0x01` |
| 8 | 1 | flags = `0x00` (any other value: unsupported) |
| 9 | 1 | `chunk_log2`, 12…24 (chunk size = 2^chunk_log2 plaintext bytes; default 20 = 1 MiB) |
| 10 | 2 | stanza count, 1…1024 |
| 12 | 32 | key commitment (see Keys) |
| 44 | … | stanzas |

Each stanza is `type[1] ‖ len[2] ‖ body[len]`:

* **`0x01` NIP-44 recipient** — `len` = 196 exactly:
  `recipient_pk[32] ‖ ephemeral_pk[32] ‖ payload[132]` where `payload` is the
  canonical base64 NIP-44 v2 payload of the 32-byte file key under
  `nostr_nip44_convkey(ephemeral_sk, recipient_pk)` (= the recipient's
  `convkey(recipient_sk, ephemeral_pk)`). Both keys must be valid x-only
  secp256k1 points; recipients must be unique.
* **`0x02` NIP-49 passphrase** — `len` ≤ 256: the ASCII `ncryptsec1…` string
  (162 bytes for NIP-49 v2) encrypting the file key, security byte `0x01`,
  scrypt `log_n` default 16 (the `nip49-keyencrypt` plugin's default;
  `--work-factor` 16…20). Decrypt refuses `log_n` > 20 (1 GiB of scrypt
  memory) before running scrypt.
  Must be the only stanza. Passphrases are NFKC-normalised per NIP-49.

Unknown stanza types, bad lengths, non-base64 payloads, off-curve keys,
duplicate recipients, and passphrase-plus-recipient mixes are rejected.
Stanza lengths are fixed or small, so a header is at most ~204 KB.

### Keys

```
file_key  = 32 random bytes
prk       = HKDF-Extract(SHA-256, salt = "nostr-seal/v1", IKM = file_key)
root_key  = HKDF-Expand(prk, info = "root",   L = 32)   # porthome chunk/manifest root
commit    = HKDF-Expand(prk, info = "commit", L = 32)   # stored in the header
```

(The in-tree HKDF from `nips/nip44`, the same one porthome uses.) The root is
domain-separated from porthome's home keys by the `nostr-seal/v1` salt; the
chunk and manifest seals below keep porthome's own purpose salts because they
*are* the porthome construction, unchanged.

ChaCha20-Poly1305 is not key-committing, so without `commit` a malicious
sender could give two recipients different file keys under which the same
bytes authenticate to different plaintexts. Every opener checks
`commit == HKDF-Expand(prk, "commit")` right after unwrapping, so all
recipients provably hold the same `file_key` and see the same plaintext.

### Chunks

The plaintext is split into `n = ceil(total / 2^chunk_log2)` chunks (zero
chunks for an empty file); every chunk is full except possibly the last.

```
chunk_pt_i = u64(i) ‖ data_i
chunk_i    = nh_porthome_encrypt_chunk(root_key, chunk_pt_i)   # porthome/v1/chunk
           = 0x01 ‖ nonce[12] ‖ ct ‖ tag[16]                     # 8 + |data_i| + 29 bytes
addr_i     = SHA-256(chunk_i)
```

The porthome construction is convergent (nonce and key derive from
`SHA-256(pt)` under `root_key`). `root_key` is unique per file, so nothing is
shared across files; the `u64(i)` prefix additionally makes identical data at
different offsets (e.g. runs of zeros) seal differently and binds each chunk
to its position.

### Index and footer

```
index        = "NSEALIDX" ‖ u8(1) index version ‖ u8(chunk_log2) ‖ u16(0) reserved ‖
               SHA-256(header) ‖ u64(total) ‖ u64(n) ‖ addr_0 ‖ … ‖ addr_{n-1}
sealed_index = nh_porthome_encrypt_manifest(root_key, index)    # porthome/v1/manifest
footer       = u64(|sealed_index|) ‖ container_address[32] ‖ "NSEALEND"
container_address = SHA-256(sealed_index)
```

The **container address** is the root of the file's Merkle-style address
tree (it commits to every chunk address). It lives in the fixed-size footer,
not the header, because a streaming writer only knows it after the last
chunk; the header is bound to it through `SHA-256(header)` inside the index.

### Opening (order matters)

1. Parse the header strictly; pick the stanza (identity match or passphrase)
   and recover `file_key`; derive `root_key` and check the key commitment.
2. Read the footer; bound-check `|sealed_index|`; verify
   `SHA-256(sealed_index) == container_address` **before** decrypting it;
   open it with the porthome manifest AEAD.
3. Check the index: magic, version, zero reserved bytes, `SHA-256(header)`
   matches the bytes parsed in 1, `chunk_log2` matches,
   `n == total / chunk + (total mod chunk ≠ 0)`, index length is exact,
   and `|header| + total + n·37 + |sealed_index| + 48 == file size`.
4. For each chunk in order: read it, verify `SHA-256(chunk_i) == addr_i`
   **before** decrypting, open it, check the `u64(i)` prefix, write the data.

Plaintext is only ever written for chunks that have passed 2–4, in order.
A later failure (e.g. a tampered chunk 7) aborts; the CLI and GUI write to a
temporary file beside the target and only rename it into place on success.
Decrypt therefore needs a seekable input; encrypt streams (pipes are fine).

Memory is bounded by two chunks plus 32 bytes of index per chunk (a 3 GiB
sparse file seals and opens in ~21 MiB RSS). Limits: 2^22 chunks per file
(4 TiB at the default chunk size).

## Building and tests

`-DENABLE_NOSTR_SEAL=ON` (top-level CMake). `nostr-seal-gtk` and the
`.desktop` file are built when libadwaita ≥ 1.5 is found
(`-DNOSTR_SEAL_ENABLE_GTK=OFF` to skip).

| ctest | Covers |
|---|---|
| `nostr_seal_config` | `seal.conf` parsing (missing / valid / bad recipient / bad values) and the CLI sealing for the configured default recipient |
| `nostr_seal_unit` | 0 B / 1 B / C−1 / exactly one chunk / C+1 / multi-chunk round trips and exact size geometry; exactly one default chunk; 3-recipient; wrong recipient / wrong key; tamper matrix (header fields, key commitment, own and foreign stanzas, transplanted stanza from another file, chunk bytes and tags, chunk swap, dropped chunk, index, footer, truncation, append); identical chunks seal differently; passphrase round-trip with NFKC, wrong passphrase, work-factor cap; pipe input; decrypt refuses a pipe |
| `nostr_seal_big` | multi-GB streaming by fd on a sparse file (`NSEAL_TEST_BIG_BYTES`, default 3 GiB + 12 345), bounded RSS |
| `nostr_seal_signer` | CLI ↔ real `nostr-signer-daemon` on a private bus: `--to-self`, `inspect`, decrypt through `NIP44DeriveConversationKey`, not-a-recipient, signer policy deny, passphrase via `--passphrase-file` |

The signer method itself is covered by `nip55l_dbus_contract` (ACL allow /
deny, interactive `ApprovalRequested` → `ApproveRequest` approve and deny,
malformed and off-curve peers).

## Publishing (`--publish`, nostrc-hby8)

```sh
nostr-seal encrypt --to npub1alice… --to npub1bob… --publish report.pdf
nostr-seal publish [--dry-run] [--identity npub1…] report.pdf.nsealed
```

1. The sealed bytes are uploaded to your Blossom servers: your kind-10063
   (BUD-03) list, else `blossom_servers` in
   `~/.config/nostr-share/nostr-share.conf`. This reuses nostr-share's
   upload path (hanami client, kind-24242 auth signed through
   `org.nostr.Signer`); nostr-seal has no second Blossom client.
2. A NIP-94 kind-1063 event is signed through `org.nostr.Signer` (as
   `org.nostr.Seal`). It carries `url`, `m` = `application/vnd.nostr.sealed`,
   `x`/`ox` (SHA-256 of the sealed bytes), `size`, an `alt` text and one `p`
   tag per recipient **read from the file's own header**, so the tags always
   match who can open it. No file name or plaintext metadata is published.
   A passphrase-sealed file gets no `p` tags.
3. It is published with libnostr-publish (`NostrPublisher` and the
   transport factory) to the targets that
   `nostr_publish_policy_select_targets()` picks from your NIP-65 write
   relays (kind 10002 found via the session relay or `home_relays`, else
   `home_relays`) under `upstream_mode` in `seal.conf`. The default is
   `direct_only`: the session relay does not forward to other relays yet,
   so the session modes would keep the event on this machine.

`--dry-run` prints the unsigned event with the predicted blob URL and the
chosen targets, and uploads, signs and publishes nothing. It works without
a signer. Needs a build with `-DENABLE_NOSTR_SHARE=ON` (the packages have
it). `nostr-seal-gtk` does not offer publishing yet.

## Not yet

* A "Publish" button in `nostr-seal-gtk`.
* `gnostr-signer-daemon` exports the same nip55l GLib service, so it serves
  `NIP44DeriveConversationKey` as-is; the gnostr-signer approval dialog shows
  it with the generic kind/preview text, and its remembered-decision store is
  keyed by (app, account) without the request kind — follow-up bead.
