# Shared encrypted vaults — composing porthome, libhanami and libmarmot

**Status:** Implemented and merged — `nostrc-er4fz` (package boundaries) and `nostrc-hl30i` (shared vaults), both closed. This is the as-built design reference; §11 remains a future escalation path and §10.3 describes an out-of-tree consumer contract, not a delivered Onyx implementation.
**Beads:** `nostrc-er4fz`, `nostrc-hl30i`. Shared vaults use the delivered portable-home primitives; the remaining `nostrc-h10m` Phase-5 work is related, not a delivery prerequisite.
**Reads:** `docs/designs/home-from-relay.md` (§2 data model, §4 keys, §6.3 multi-writer punt, §8 threat model, HFR-D18), `docs/designs/porthome-crypto-spec.md` (PCS §2–§7, D4 approval), `docs/designs/nostrfs-porthome-overlay.md` (§11 D15 "MLS / group-shared homes"), `docs/libhanami-design.md` (LHD §3.3 kinds, §4.4 index), `docs/investigations/scion-nostr-blossom-doc-vs-libhanami-2026-10-13.md` (§A2 "a causal event chain is not compare-and-swap"), `docs/MLS_INTEGRATION.md`.

**Implementation map:**

| Design sections | Landed implementation |
| --- | --- |
| §3–§4, §5.1–§5.2 | [libporthome](../../libporthome/) — PCS v2, cryptree, sealed manifest and head codecs |
| §3.3, §5.3–§7 client side | [libvault](../../libvault/README.md) — rings, rekey/revoke, checkpoint fold, grants, discovery and NIP-59 carrier |
| §6.3 Marmot carrier | [libvault-marmot](../../libvault-marmot/README.md) — exporter seed and kind-445 delivery |
| §9.2, §9.8 R5 CLI | [tools/vaultctl](../../tools/vaultctl/) — share / grant / revoke / fold / fsck |
| §9.2, §9.8 R6, §10.3 ABI | [bindings/rust](../../bindings/rust/) — bindgen-generated `porthome-sys` and `vault-sys` |
| §3.5, §9.6, §10.2 | [libhanami](../../libhanami/) — sealed ODB and private ref-state mode |
| §7 server side | [apps/relayd](../../apps/relayd/README.md) and [lab-blossom](../../packaging/lab-blossom/) — opt-in VSP-01 |
| §10.1 | [nostr-homed syncd](../../gnome/nostr-homed/src/porthome-syncd/) — vault capture, head publication, fold, prefetch and materialisation |
| §9 | `nips/bud02`, `libblossom`, `libporthome`, `nostr-gobject` wrappers and the layer-ordered build graph |

> Decision numbering in this document (**D1…D18**) is local. Parent-design decisions are written **HFR-Dn**, **PCS §n**, **LHD §n**. External specs: NIP-34, NIP-42, NIP-44, NIP-51, NIP-59, NIP-78, BUD-01/02/03/04, GRASP-01/08, Marmot MIP-00–04, CORD-01/06.

***

## 0. Executive summary

nostrc composes three independently developed pieces into an end-to-end-encrypted, versioned, group-shareable file vault:

* **libporthome** (`libporthome/`) — convergent-AEAD sealing, chunking, name hashing, cryptree keys, sealed manifests and kind-30078 pointer/head codecs, independent of GNOME.

* **libhanami** (`libhanami/`) — libgit2 ODB and refdb over the shared packages, with plaintext GRASP mode or sealed vault mode.

* **libmarmot** (`libmarmot/`) — MLS over Nostr, including the additive exporter API used by `libvault-marmot` to supply the seed through the seam anticipated by overlay D15.

This document specifies the fourth piece — **a grant and membership layer that lets a porthome subtree be read and written by a set of Nostr pubkeys, with scoped rekey on removal** — and the composition rules under which porthome, hanami and a Tauri/Rust consumer such as Onyx all become thin assemblies of the same packages.

The design makes four commitments:

1. **Capabilities, not identities.** Membership is possession of a subtree key ring (Tahoe/Peergos model, CORD-01's "if you can decrypt the room you're in it"). Relays and Blossom servers never see a member list.
2. **PCS v1 convergent sealing is kept**, extended with per-subtree keys and the already-reserved `chunk_key_id` as a per-chunk epoch, so removal rotates one subtree and re-seals lazily (D5, D6).
3. **Multi-writer is owner-sequenced in v1**, with per-writer heads retained so a dead sequencer degrades to a read-only fold, never to data loss (D8). The Scion critique's CAS objection is accepted, not argued with.
4. **The repo is re-layered first** (§9). R0–R3 removed porthome's dependency on hanami's Blossom client, exposed the NIP-44 KDF through a public header, consolidated Blossom transport and promoted libporthome before shared-vault implementation. §9 records the resulting package boundaries.

The honest caveat up front: **D8 (owner-sequencer) means a shared vault has one party who can always advance the head.** That is the right trade for a small team sharing artifacts; it is not a general multi-master filesystem, and §11 says what it would take to become one.

***

## 1. Ground truth — package ownership

| Component | Location | As-built responsibility |
| --- | --- | --- |
| porthome crypto | `libporthome/src/crypto.c`, public `include/porthome/` | PCS v1 compatibility plus v2 cryptree keys, epochs, padding and content-defined chunking |
| porthome manifest / pointer | `libporthome/src/manifest.c`, `dir-node.c`, `head.c`, `wrapkey.c` | Legacy codecs plus strict v2 sealed nodes and per-writer heads; promoted sources **and headers** from nostr-homed |
| Vault policy | `libvault/` | Grants, rings, revocation, owner-checkpoint fold, conflicts, discovery and carrier seam |
| libhanami | `libhanami/` | Plain or sealed git ODB/refdb; OID↔Blossom index (LHD §4.4) implements libporthome's storage-agnostic index interface using SQLite |
| libmarmot | `libmarmot/`, adapter `libvault-marmot/` | MLS group lifecycle and kind 443/444/445; additive `marmot_export_secret` API supplies the vault exporter seed (§6.3) |
| Blossom transport | `libblossom/`, `nips/bud02/` | One transport/probe/shim implementation and kind-24242 auth; GNOME consumers use `GnBlossomClient`, nostr-homed uses the pool API directly |
| NIP-44 primitives | `nips/nip44/include/nostr/nip44/kdf.h` | Public namespaced HKDF/HMAC API, replacing cross-package bare `extern` declarations |
| Build graph | root `CMakeLists.txt`, `gnome/nostr-homed/CMakeLists.txt` | Layer-ordered packages; porthome is independent of libhanami; pam/authd retain their dependency fence (§9.1) |
| Group protocols outside this composition | `nips/nip-concord`, `nips/nip-communikeys`, `nips/nip29` | Not dependencies; see §2.2 |

HFR §6.3 and overlay D15's deferred sharing model is implemented by this composition; their single-home descriptions are not the shared-vault wire contract.

***

## 2. Goals and non-goals

### 2.1 Goals

* **G1** A vault owner can share any directory subtree (including the root) of a porthome with a set of Nostr pubkeys, read-only or read-write, by delivering a key ring and nothing else.

* **G2** Relays and Blossom servers learn no filenames, sizes beyond bucket, member list, or content; they learn that *some* vault with a random id exists and which pubkeys publish heads for it.

* **G3** Removing a member stops them reading anything written after removal, without re-uploading unchanged content and without a flag day for the rest of the home.

* **G4** Two or more writers can commit to the same subtree and every reader converges on the same tree, with conflicts surfaced per path rather than resolved silently.

* **G5** The same sealed objects are consumable by three assemblies: the GNOME `syncd`/FUSE pair, a libgit2 repository via libhanami, and a non-GNOME, non-C application (Onyx, Tauri/Rust) through a stable C ABI.

* **G6** A self-hostable "private vault service" — one Blossom plus one relay, both allowlisted — is specifiable in one page and implementable by configuration of existing servers.

### 2.2 Non-goals

* **Replacing Concord, Marmot or NIP-29 for chat.** A vault may *carry* a CORD-01 stream or a Marmot group for discussion, but this document defines storage, not messaging. `nips/nip-concord` and `nips/nip-communikeys` are not dependencies.

* **Live collaborative editing (CRDT).** G4 is version-level convergence. Character-level merging of markdown is an application concern (Onyx could layer Yjs updates as chunks; nothing here prevents it).

* **Forward secrecy of content at rest.** Content keys are long-lived by design (dedup). FS applies to key *delivery* (D11, Marmot) only.

* **Deniability or hiding vault existence** from a relay operator who can see kind-30078 traffic. Hiding the id/epoch label is possible (CORD-style rotating labels) and is parked in §14.

* **Multi-master without a sequencer** in v1 (D8, §11).

* **Erasure coding / multi-server durability policy.** BUD-04 mirroring to N servers is a `syncd` policy already (HFR §6.5); unchanged.

***

## 3. Data model

### 3.1 Vault, subtree, node

A **vault** is a porthome subtree chosen for sharing. The share boundary is a **directory node** in the manifest tree; its id is `vault_id = random[32]`, assigned at share time and never derived from content. Everything below that node is in the vault; nothing above it is.

A single porthome may contain many vaults (one per shared subtree), and one vault may be the whole home (HFR's "group-shared home", HFR-D18).

### 3.2 Key hierarchy (extends PCS §3)

PCS v1 has one `home_key` per home. v2 introduces a **node key** per directory node and makes `home_key` the node key of the root. Chunk, manifest and name keys derive from the *owning node's* key, not the home's.

```
seed[32]                         <- PCS: from wrapped_home_key via NIP-46, or (new) from a grant (§6)
  |
  v
root_node_key = HKDF(salt="porthome/v2/home", ikm=seed, info="", L=32)

node_key(child) = HKDF(salt="porthome/v2/node",
                       ikm = node_key(parent),
                       info = node_id(child),           # random[32], stored in parent's manifest entry
                       L = 32)
                  UNLESS the parent's manifest entry for child carries key_override:
                       node_key(child) = key_override    # random[32], sealed inside parent's manifest node

per node, per epoch e (key ring, §3.3):
  chunk_nonce = HKDF(salt="porthome/v2/chunk",    ikm=node_key[e], info=SHA256(pt), L=12)
  chunk_key   = HKDF(salt="porthome/v2/chunk",    ikm=node_key[e], info=chunk_nonce,  L=32)
  mani_nonce  = HKDF(salt="porthome/v2/manifest", ikm=node_key[e], info=SHA256(pt), L=12)
  mani_key    = HKDF(salt="porthome/v2/manifest", ikm=node_key[e], info=mani_nonce,   L=32)
  name_key    = HKDF(salt="porthome/v2/name",     ikm=node_key[e], info="",           L=32)
  head_key    = HKDF(salt="porthome/v2/head",     ikm=node_key[e], info="",           L=32)   # NEW: seals pointer/head bodies
```

Properties this buys, each priced:

* **Sharing a subtree is one secret.** Giving someone `node_key(S)` lets them derive every descendant's key (cryptree). Nothing above `S` is derivable: HKDF is one-way and the parent key is never in `S`'s material.

* **Convergence is per subtree, not per home.** Identical plaintext under two different node keys yields two different Blossom addresses. Dedup works within a vault (members dedup against each other — the common case for a team) and is deliberately broken across vaults, so two vaults on one server cannot be shown to hold the same file. This is PCS §6(c) narrowed from "per home" to "per vault".

* **`key_override`** **is the rekey primitive.** Setting it on a node breaks the derivation chain below that point and nowhere else.

The salt version bumps `v1 → v2` because the derivation tree changed shape; per PCS §2 the on-wire version byte bumps in lockstep (§4.1). v1 blobs remain decodable by a v1 code path for migration (§14 item 6); they are key-disjoint from v2.

### 3.3 Epochs and key rings

Every node has a **key ring**: an ordered list `[(epoch, node_key)]`, epoch starting at 1. A rekey (D5) appends `(e+1, random[32])` and writes that key as `key_override` in the parent's manifest entry. The ring is what a grant conveys (§6.2) — a member added at epoch 3 receives epochs ≥ 3 only, unless the owner chooses to grant history.

PCS's reserved `chunk_key_id` becomes **the epoch of the node key the chunk was sealed under**. The manifest records it per chunk. Decoders MUST hold the matching ring entry or refuse (PCS §4 rule, now meaningful).

### 3.4 Manifest node (`porthome_dir_node` v2)

A sealed directory node, CBOR, under `mani_key`. New fields over the Phase-3 manifest are marked ⊕.

```
dir_node {
  node_id        : bytes32                 # random, stable for the node's lifetime
  ⊕ epoch        : uint                    # current epoch of this node's key ring
  ⊕ vault        : optional {              # present only on a share-boundary node
       vault_id   : bytes32
       owner      : pubkey                 # the sequencer (D8)
       writers    : [pubkey]               # may publish heads; validated by readers
       readers    : [pubkey]               # informational for UI; NOT an ACL — key possession is the ACL
       grants_rev : uint                   # bumps whenever writers/readers change
     }
  entries        : [{
       name_enc   : hex48                  # PCS §7 keyed-hash index, under this node's name_key
       ⊕ name_plain : utf8 basename        # required, inside this AEAD-sealed body (PCS §7)
       kind       : dir | file | symlink
       mode, mtime, size_plain
       dir:   { node_id, ⊕ key_override?: bytes32, ⊕ child_epoch: uint, manifest_sha256 }
       file:  { chunks: [{ sha256, size_sealed, ⊕ size_plain, ⊕ chunk_key_id }] }
     }]
}
```

`name_enc` is a keyed-hash index, not reversible encryption (PCS §7). A fresh
member cannot recover basenames from it, so `name_plain` is required **inside
the sealed body**, with no owner-private name inventory needed. Each chunk's
`size_plain` is authenticated there too: padded bucket sizes do not reveal true
chunk lengths (§4.2), and the per-file total cannot recover each CDC boundary.
The chunk lengths sum exactly to the entry's file `size_plain`.
Basenames are safe single UTF-8
components (1–255 bytes, no NUL, slash, backslash, `.` or `..`). These are manifest
body data, not an owner-private inventory. The unreleased v2 codec includes them
in place and strictly rejects missing, duplicate, unknown or malformed fields.

`readers` is explicitly *not* enforced anywhere — it exists so a UI can show "shared with" without a side channel. The enforcement is the key ring. Writing a pubkey into `readers` without sending it a grant shares nothing; sending a grant without listing it shares everything. The owner's client keeps the two in step; the protocol does not depend on it.

### 3.5 Ref-state node (private 30618) — hanami mode only

When a vault is also a git repository (D13), hanami's refdb does **not** publish kind 30618. Commit OIDs are SHA-1/SHA-256 of *plaintext* git objects; publishing them is confirmation-of-known-plaintext to anyone holding the plaintext, which defeats G2. Instead the ref state is a sealed node:

```
ref_state_node {
  refs      : [{ refname, target_oid }]
  index     : [{ git_oid, sealed_sha256, chunk_key_id }]   # LHD §4.4's OID↔Blossom index, now shared via the vault
  prev      : optional sha256                                # previous ref_state_node
}
```

sealed under the vault root's `mani_key`, addressed by `sha256(sealed)`, and referenced from the head (§5.2). Hanami's ODB stores each git object as a **porthome chunk**: `sealed = seal(node_key, git_object_bytes)`; the index maps `git_oid → sha256(sealed)`. Nothing else in hanami changes; the Blossom server holds opaque chunks indistinguishable from any other vault's.

A public 30618 MAY still be published for a vault whose owner has flipped it public (GRASP-08's "privacy can be lifted"); that is a projection, not the source of truth.

***

## 4. Sealed objects (extends PCS §4–§5)

### 4.1 Byte layout

```
offset  size  field
   0     1    version    0x02
   1    12    nonce
  13     N    ciphertext
 13+N   16    poly1305 tag
```

Identical to PCS v1 except the version byte. Overhead stays 29 bytes. Address is `sha256(sealed)`. PCS §5 strictness applies verbatim; decoders additionally MUST refuse `version == 0x02` blobs whose `chunk_key_id` is not in their ring, and MUST NOT fall back to trying other ring entries (an oracle).

### 4.2 Padding

PCS §9 left bucket padding to "Phase 2". For shared vaults it is **required**: a team's PDFs have fingerprintable sizes and a shared Blossom lists them. Plaintext chunks are padded to the NIP-44 `calc_padded_len` schedule before sealing (reusing the implementation; the schedule is already audited). `size_plain` in the manifest records the true length; the decoder truncates after AEAD. Chunking is content-defined (D3) so bucketing applies per chunk, not per file.

### 4.3 PNG shim

Unchanged from `hanami-blossom-shim.h`; it is a transport concern of the Blossom client (§9.3) and invisible above it. A shimmed blob's address is `sha256(shim‖sealed)` per the existing contract.

***

## 5. Pointers, heads and the fold

### 5.1 Reuse of kind 30078

porthome's pointer is a kind-30078 (NIP-78 application data) addressable event, `d` scoped to porthome, content NIP-44-self-encrypted. Shared vaults keep the kind and change two things:

* `d = "porthome/v2/" ‖ hex(vault_id)` — one addressable slot per (author, vault).

* content is NIP-44 v2 encrypted under a **conversation key derived from `head_key`**, not the author's identity key. `porthome_head_key_v2` reduces the §3.2 HKDF output modulo the secp256k1 group order and rejects zero, yielding a valid BIP340 scalar. `head.c` derives its x-only Schnorr public key with `nostr_key_get_public`, then calls `nostr_nip44_convkey(head_key, pub(head_key), out_convkey)` for self-ECDH (the CORD-01 construction). Any holder of the matching ring epoch decrypts; the relay sees NIP-44 noise.

Because 30078 is addressable per author, **every writer has their own head slot** for free. There is no new kind for heads.

### 5.2 Head body

```
head {
  vault_id       : bytes32
  epoch          : uint                  # root node epoch this head was written under
  root_manifest  : sha256                # sealed dir_node of the vault root
  ref_state      : optional sha256       # §3.5, hanami mode
  seq            : uint                  # per-writer monotonic
  parents        : [{ writer: pubkey, seq: uint, root_manifest: sha256 }]   # heads this one was folded from
  grants_rev     : uint
  written_at     : unix
}
```

`parents` records per-writer incorporation coverage (§5.3). It is **conflict detection, not compare-and-swap** — the Scion critique §A2 is accepted in full. Two writers can both publish `seq=8` with the same parent; both are valid events.

### 5.3 Fold rule (what every reader does)

1. Fetch the owner's current signed kind-30078 head for `d = porthome/v2/<vault_id>`, then heads from **all pubkeys in `writers`** of its verified root manifest (bootstrap: the authenticated grant names the owner). Use `vault_fold_checkpoint(grant, records, count, owner_index, &result)`: the selected verified record's author, manifest owner, vault ID and root node ID must match the grant. Selecting an older owner head when a higher owner sequence is supplied is refused. The current owner manifest anchors the writer policy, not an unavailable ancestor chain.
2. Drop heads whose author is not in `writers`, or whose `grants_rev` is older than the newest manifest's (a removed writer's stale head).
3. Compute the **frontier** by per-writer sequence dominance among accepted heads: a parent reference `(W, s, root_manifest)` in any accepted head dominates supplied heads by writer `W` with `seq ≤ s`. Kind-30078 heads are addressable; relays need retain only each writer's latest head, so older ancestor head events need not be fetchable. Missing ancestors are not an error. Ignored nonwriter, stale or policy-mismatching heads cannot dominate anything. Sort the undominated heads by the stable record ordering, not arrival time.
4. If the frontier has one element, that is the tree. An empty frontier fails closed. With multiple frontier heads, step 5 selects the latest unambiguous accepted owner head, even if it is not itself on the frontier; ambiguous owner state fails rather than falling back to a clock/LWW rule.
5. If it has more than one: the **owner's** head wins for the materialised view (D8); other frontier heads are surfaced as *conflicts* per path where their `entries` differ from the owner's. Nothing is deleted; a conflicting writer's content stays addressable by its own head until the owner folds it.

The "newest manifest" in step 2 is the owner's checkpoint manifest, not an arbitrary writer's revision. Policy checks additionally reject heads with a different owner, vault, writer set or `grants_rev` from the checkpoint policy; conflicting owner policy at the same revision fails closed. Duplicate records are ignored. Per-path conflicts include differing digests and paths present on only one side (deletion-vs-edit presence flags). The result retains all verified input root hashes and their parent root hashes, including ignored heads; the fold deletes no objects.

Before publishing a successor, `vault_head_cover` (`libvault/src/parents.c`) merges every accepted record actually incorporated **and its existing parents**, retaining the highest sequence per writer. Equal-writer/equal-sequence references with different roots fail, even if a higher sequence is also present; exceeding the parent cap fails rather than truncating coverage. `vault_head_include` and `vault_tree_head_new` preserve earlier coverage for administrative successors. This carries incorporation forward when the relay replaces earlier heads. The separate strict `vault_fold` API still requires a complete signed ancestor closure; it is not the addressable-relay fold.

Step 5 is the whole of v1's multi-writer authority rule. It is deterministic and needs no clock agreement. Its cost is that a non-owner writer's commit is *provisional* until the owner's `syncd` (or any client acting with the owner's key) folds it — typically seconds when the owner is online, indefinitely when not. §11 describes the upgrade path.

### 5.4 Discovery — private vault service list

A NIP-51-style list, **kind 10319** (the implemented local allocation; external registry coordination is not established here), follows GRASP-08's kind-10318 private-discovery pattern. Its plaintext JSON object contains `vault-relay` and `vault-blossom` URL arrays, carried NIP-44-self-encrypted in `content`, with public `tags` empty. A client decrypts its own list, then asks each listed relay for its 30078 heads. The list's existence and author are public; the servers are not. This keeps vaults off public relays without hard-coding server URLs into grants (grants carry hints only, §6.2).

***

## 6. Grants and membership

### 6.1 What a grant is

A grant is the delivery of `(vault_id, node_id, key ring[epochs], cap)` to a pubkey, plus bootstrap hints. `cap ∈ {read, write}`. A write grant is **the same key material** as a read grant plus the owner having added the pubkey to `writers` in the manifest; write authority is asserted by the owner's signed manifest, not by the grant. (Readers verify heads against `writers`; a "write" grant to someone not in `writers` is a read grant.)

### 6.2 Grant body (rumor)

```
grant {                                    # rumor kind 30804; external Onyx 308xx draft unverified here
  vault_id     : bytes32
  node_id      : bytes32                   # share-boundary node; = root node_id for a whole-vault grant
  ring         : [{ epoch, node_key }]
  cap          : "read" | "write"
  owner        : pubkey
  root_manifest: sha256                    # bootstrap; the fold (§5.3) supersedes it
  hints        : { relays: [url], blossom: [url] }   # advisory; 10319 is authoritative for the recipient's own set
  issued_at    : unix
}
```

Kind 30804 is a **rumor-only** kind: it is never published bare and never signed as a standalone event; it appears only as the inner event of the two carriers below. Rumor kinds are type tags, so using an addressable-range number for a never-published rumor is harmless, and does not imply a published addressable slot. This design uses 30804 locally; Onyx's external 30800–30803 draft is unverified here, so no registry compatibility is claimed.

### 6.3 Carriers

**Primary — Marmot group (D11).** The owner creates (or reuses) a Marmot group whose members are the vault's members. The vault's root ring is **derived from the group's exporter secret**: `seed = MLS-Exporter(label="porthome/v2/vault", context=vault_id, L=32)`, exactly the seam overlay-D15 anticipated. `libvault-marmot/src/seed.c` calls the additive `marmot_export_secret` API (RFC 9420 §8.5), with the label `porthome/v2/vault` and the 32 raw `vault_id` bytes as context, not hex. Grant bodies travel as kind-445 application messages inside the group. Add/remove is an MLS commit; the new epoch's exporter yields the new root key; PCS/forward secrecy of delivery comes from MLS, not from this document.

**Fallback — NIP-59 gift wrap.** For a recipient without an MLS-capable client (today: Onyx, any non-nostrc app), the grant rumor is sealed and wrapped per NIP-59 to the recipient's pubkey. No forward secrecy on delivery; the recipient's identity key decrypts it forever. The owner's client MUST prefer the Marmot carrier when the recipient has a kind-443 key package on the vault relays and MAY fall back otherwise; the recipient learns which carrier was used from the wrapper, nothing in the grant body differs.

### 6.4 Revocation (D5, D6)

To remove member `M` from subtree `S` (which may be the root):

1. Owner appends `(e+1, random)` to `S`'s ring and writes it as `key_override` in `S`'s parent entry (for the root, in the head itself — the root has no parent; its new key is delivered only by grant/exporter).
2. Owner removes `M` from `writers`/`readers`, bumps `grants_rev`, re-seals `S`'s manifest node under epoch `e+1`.
3. **Lazy chunk re-seal (D6):** chunks under `S` keep `chunk_key_id = e` and remain readable by anyone holding epoch `e` — which `M` does. New and modified chunks are sealed under `e+1`. A background task re-seals old chunks under `e+1` opportunistically (bandwidth-budgeted, same budget class as HFR §6.5 retention); when every chunk under `S` is at `e+1`, epoch `e` is dropped from the ring in future grants.
4. Marmot: MLS Remove commit; the exporter changes; done. Gift-wrap: new grant to each remaining member.
5. Service (D14): owner removes `M` from the relay/Blossom allowlist. This is the second factor — it stops `M` fetching even the epoch-`e` chunks they could still decrypt.

What `M` keeps: everything they already fetched, and the ability to decrypt any epoch-`e` chunk they can still *obtain*. Step 5 is what makes "still obtain" hard. This is the same post-removal statement GRASP-08 and CORD-06 make; no shared-key design does better without re-encrypting everything, and D6 lets the owner choose to do exactly that (eager re-seal) when the content warrants it.

***

## 7. Server requirements — "VSP-01" (one page, D14)

A **private vault service** is any pair of (Blossom server, Nostr relay) that:

1. **Blossom:** implements BUD-01 GET/HEAD, BUD-02 PUT with kind-24242 auth, BUD-04 mirror; rejects uploads from pubkeys outside an operator allowlist with `401` and a `WWW-Authenticate: Nostr` header; **returns** **`401`, never** **`403`/`404`, for GET/HEAD of any blob by a non-allowlisted pubkey** (existence hiding, GRASP-08 rule). Advertises itself in BUD-01's server-info document with `"private": true`. Accepts `application/octet-stream` bodies (no sniffing) *or* is probed and shimmed by the client (`hanami-server-capability`).
2. **Relay:** requires NIP-42 auth before serving or accepting kind 30078 / 445 / 1059 for the vault namespace; `auth-required:` when unauthenticated, `restricted:` when authenticated but not allowlisted; NIP-11 `limitation.auth_required = true` plus a `"vault": true` local VSP-01 marker so clients can classify a URL before connecting.
3. **Allowlist source:** operator-configured pubkeys, optionally unioned with the `writers`/`readers` of vaults the operator hosts if the operator is given a read grant (an operator who can read the vault can read the member list; an operator who cannot, manages the list by hand). There is no protocol event for the allowlist; it is service configuration, as in GRASP-08.

`relayd` (`apps/relayd/`) implements this as an opt-in `vault_allowlist` and NIP-11 marker. It protects kinds 30078/445/1059 service-wide: encrypted wraps cannot be classified safely by a plaintext vault tag. `packaging/lab-blossom` supplies the reference private service through its explicit VSP-01 Compose override; its default remains permissive. The GRASP→Gitea adapter is outside this implementation's server scope.

A publish is acknowledged only after committed visibility. If an admitted write's outcome is still unknown at relayd's 10-second confirmation deadline, relayd sends an event-bearing `NOTICE` (`error: storage outcome unknown for event <id>`) and closes the publisher WebSocket, **without an OK**. A later confirmed commit is reconciled independently of that connection and fans out once to matching authorized live subscribers. This is neither a denied write nor proof it was lost; see `apps/relayd/README.md` (Storage confirmation and late commits).

***

## 8. Threat model delta (vs PCS §6, HFR §8)

| Adversary                       | PCS v1 (single home)                           | This design                                                                                                                                                                                                                                                                                                                 |
| ------------------------------- | ---------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Relay operator                  | sees 30078 traffic, timestamps, sizes          | additionally sees *which pubkeys publish heads for the same* *`vault_id`* — i.e. the writer set, not the reader set. Rotating `d` labels (§14 item 4) would hide this.                                                                                                                                                      |
| Blossom operator                | sees sealed blobs, addresses, uploader pubkeys | additionally sees that N uploaders share address space (dedup across members confirms same-vault membership of uploaders). Priced in; it is the same signal PCS §6(c) accepts for one user's two machines.                                                                                                                  |
| Removed member                  | n/a                                            | keeps epoch-`e` material (§6.4). Mitigated by service allowlist and eager re-seal option.                                                                                                                                                                                                                                   |
| Malicious writer                | n/a                                            | can publish a head pointing at garbage or at a manifest that omits others' files. Readers fold against `writers` and surface conflicts; the owner's head is authoritative for the view; nothing is destroyed because every head's objects remain addressable. A malicious *owner* is out of scope — they hold the root key. |
| Confirmation-of-known-plaintext | requires `home_key`                            | requires the *subtree's* node key; strictly narrower.                                                                                                                                                                                                                                                                       |
| Nonce reuse                     | SHA-256 collision only                         | unchanged; per-epoch keys add no new reuse path because epoch is in the key, not the nonce.                                                                                                                                                                                                                                 |
| Head replay / rollback by relay | author-signed, `created_at` ordered            | additionally `seq` per writer and `parents` DAG; a relay serving an old head is detected by any reader who has seen a newer `seq`. Not prevented — accepted, as in NIP-01 generally.                                                                                                                                        |

***

## 9. Repository restructuring — normative

### 9.1 Why the package boundaries are prerequisites

The restructuring removes three composition obstacles, rather than merely renaming packages:

1. Porthome previously borrowed libhanami's Blossom client and was GNOME-gated, preventing a standalone non-GNOME consumer from taking porthome without git. The pam/authd fence **already prevented** libhanami/libgit2 from reaching the base nostr-login stack; this was a load-bearing boundary, not an existing pam dependency leak. The experimental-option fence in `gnome/nostr-homed/CMakeLists.txt` (originally around lines 53–68) remains protected after extraction.
2. Crypto and wrapkey previously declared NIP-44 HKDF/HMAC internals as bare `extern` symbols, relying on link order. R0 replaced them with the public `<nostr/nip44/kdf.h>` API so declarations and export policy belong to the providing package.
3. Five Blossom clients used divergent guards and two auth styles (NIP-98 kind 27235 versus BUD-02 kind 24242). Their shared transport, probe, shim and guards now belong to libblossom, with BUD-02 auth in `nips/bud02`.

Layer-ordered `add_subdirectory` calls replace the old `if(TARGET hanami)` ordering workaround. Required siblings are explicit dependencies, not silently unavailable features.

### 9.2 Package layout

Libraries are ordered by layer; an arrow may only point downward. ⊕ marks additions made by this composition, not pending implementation. **No package may include another package's** **`src/`, declare another package's symbols** **`extern`, or test for another package's target with** **`if(TARGET)`** **to degrade silently.**

```
Layer 0  libgo/  libjson/  third_party/
Layer 1  libnostr/
Layer 2  nips/nipNN/            each exports include/nostr/nipNN/*.h ONLY; src/ is private
         nips/nip44/            ⊕ exports <nostr/nip44/kdf.h>: nostr_nip44_hkdf_extract/expand, nostr_nip44_hmac_sha256
         nips/bud02/            ⊕ NEW: kind-24242 Blossom auth (moved from libhanami/src/hanami-bud02-auth.c)
Layer 3  libblossom/            ⊕ NEW: the one Blossom client (§9.3)
         libmarmot/             MLS; additive exporter API (§6.3)
Layer 4  libporthome/           ⊕ NEW: promoted from gnome/nostr-homed/src/{porthome,porthome-common} (§9.4)
Layer 5  libvault/              ⊕ NEW: grants, rings, epochs, fold, VSP discovery (§9.5)
         libhanami/             git-specific only; consumes libblossom + libporthome (§9.6)
Layer 6  nostr-gobject/         ⊕ GnBlossomClient, GnVault GObject wrappers (GTask) for GNOME consumers
         marmot-gobject/
         nostr-gtk/             reusable widgets/portable rendering only; no GNostr app services
         bindings/rust/         ⊕ porthome-sys, vault-sys (bindgen from installed C headers) for Tauri/Onyx
Layer 7  gnome/nostr-homed/     syncd, FUSE, provisioner, pam/nss/auth glue, systemd — no crypto, no Blossom HTTP
         gnome/groundhog/, gnome/nostr-share/, apps/gnostr/   consume nostr-gobject's GnBlossomClient
         apps/gnostr/src/widgets/   app-coupled note row, embed, thread view and profile pane
         tools/vaultctl/        ⊕ CLI over libvault (share / grant / revoke / fold / fsck)
```

There is no `nips/nip78` package: libporthome owns the application-specific kind-30078 pointer/head codec. Layer 6's nostr-gtk does not own GNostr-coupled widgets; those live in Layer 7's `gnostr-widgets` archive. See `docs/TIMELINE_LIBRARY_APP_BOUNDARY.md` for the reusable/app widget boundary.

The three assemblies this document is about:

| Assembly                                          | = packages                                                                                                                                                                       |
| ------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **porthome** (GNOME roaming home)                 | libporthome + libvault + libblossom + nips/{44,59,bud02} + libnostr, assembled by `gnome/nostr-homed`                                                                         |
| **libhanami** (encrypted or plain git on Blossom) | libgit2 + libporthome (sealed ODB mode, D13) + libblossom + nips/{34,bud02} + libnostr                                                                                           |
| **onyx-vault** (Tauri app)                        | bindings/rust/{porthome-sys,vault-sys} → libvault + libporthome + libblossom + nips/{44,59,bud02} + libnostr; **no GLib, no libgit2, no libmarmot unless the Rust side opts in** |

### 9.3 libblossom — one client

Seeded from `libhanami/src/hanami-blossom-client.c` (the largest, the one the porthome reviews exercised, and the one with batch auth + capability probing) plus `hanami-blossom-shim.c` and `hanami-server-capability.c`. Public surface, `include/blossom/`:

* `blossom_client_{new,free}`, `_get`, `_head`, `_put` (BUD-02, kind 24242 via `nips/bud02`), `_put_batch` (the N-x-tag shared auth event, gated by probe), `_mirror` (BUD-04), `_list` (BUD-02 list), `_delete`.

* `blossom_probe_capabilities` (batch_ok, content-type sniffing, size limits, `private` flag per §7) with the 24 h cache discipline from the shim module.

* `blossom_shim_{wrap,unwrap,detect}` — PNG shim as a pure function; the *decision* to shim lives in the client, driven by probe, not by an env var (closes the "automatic capability-probe-driven decision" follow-up bead named in `hanami-blossom-shim.h`).

* Guards currently split across `nh_porthome_blossom.c` (SSRF, body-size) and `common/blossom_client.c` move here once, with the stricter of each pair.

* Transport: libcurl. **No GLib.** The GLib/GTask adapter is `GnBlossomClient` in nostr-gobject; `gh-blossom-client.c` and `ns-blossom.c` become thin consumers of it.

Deleted after migration: `gnome/nostr-homed/src/common/blossom_client.c`, `nh_porthome_blossom.c` (its guards and shim decision are absorbed), `gh-blossom-client.c`'s HTTP layer (its media-specific logic stays), `ns-blossom.c`. Kind-27235 (NIP-98) upload auth is **retired for Blossom** — HFR-D7 already said so; libblossom speaks BUD-02 only and NIP-98 stays where it belongs (`nips/nip98`, HTTP auth generally, GRASP smart-HTTP in hanami).

### 9.4 libporthome — promote, rename, de-GNOME

The promotion moved the reusable sources from `gnome/nostr-homed/src/{porthome,porthome-common}` to `libporthome/src`, and their public headers from `gnome/nostr-homed/include/` to `libporthome/include/porthome/`. The private CBOR header moved with the sources. `nh_porthome_*` became `porthome_*`, with the one-release `porthome/compat-nh.h` macro compatibility header for syncd/FUSE.

Contents: PCS v1+v2 sealing (§3–§4), manifest codec, name hashing, wrapkey, pointer/head codec (§5.1–§5.2), chunker (D3), the OID↔sealed index type and storage-agnostic interface (hanami is one user; its SQLite implementation stays in `libhanami/src/hanami-index-sqlite.c`). **Not** contents: anything that opens a websocket, signs, touches systemd, FUSE, `auth.sock`, D-Bus, or `$HOME`. Those stay in nostr-homed.

Dependencies: libnostr, `nips/nip44` **through `<nostr/nip44/kdf.h>`**, libblossom, OpenSSL. The bare `extern` declarations were removed in R0, before this promotion. No `nips/nip78`, libhanami or libmarmot dependency.

### 9.5 libvault — the new code

Owns §3.3, §5.3, §6 and §7 client side: key rings, `key_override`, epoch handling, grant codec, the fold, conflict reporting, kind-10319 list, VSP classification. Carriers are **pluggable**: libvault defines `vault_grant_carrier` (send/receive callbacks); `libvault-marmot/` (a tiny adapter, Layer 5, depends on libmarmot) implements the exporter-secret seed and kind-445 transport; the NIP-59 carrier is in libvault proper (it needs only nip59). This keeps libmarmot out of Onyx's link line until Onyx wants it.

### 9.6 libhanami — shrink to git

Keeps: ODB backend (now over `porthome_seal`/`porthome_open` when a vault key is present, raw when not — D13), refdb (30618 public mode; `ref_state_node` private mode), GRASP client, `blossom://`/`nostr://` transport, NIP-34 event helpers. Loses: Blossom HTTP (→ libblossom), BUD-02 auth (→ nips/bud02), shim and capability probe (→ libblossom), index type/interface (→ libporthome; SQLite implementation stays in hanami). `hanami-nostr.c` uses libnostr's `NostrSimplePool` rather than duplicating relay plumbing.

### 9.7 Build-system rules (enforced, not advised)

1. Every library: `target_include_directories(<lib> PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include> PRIVATE src)`. Public headers under `include/<pkg>/`; nothing in `src/` is installed or visible to other targets.
2. Namespaced alias targets `nostrc::blossom`, `nostrc::porthome`, `nostrc::vault`, `nostrc::hanami`; consumers link aliases only. Each ships a `.pc` and a CMake config so out-of-tree consumers (Onyx's Rust build) `find_package(nostrc COMPONENTS vault)`.
3. Root `CMakeLists.txt` adds subdirectories **in layer order** (§9.2) and `gnome/` last. `if(TARGET <sibling>)` is banned for required dependencies; a missing dependency is a configure-time `FATAL_ERROR` naming the option to flip, as libhanami already does for nip34.
4. `-fvisibility=hidden` default on every library, with `NOSTRC_API` export macros on public functions. This turns today's `extern` trick into a link error, which is the point.
5. `scripts/check-package-boundaries.sh` runs in CI: fails on `#include` paths containing `../` or `/src/` outside `tests/`; fails on `extern <type> <ident>(` declarations whose identifier is defined in another package (symbol table diff against `nm` of sibling objects); fails on any Layer-N target linking a Layer-(>N) target. Baseline the current violations in an allowlist file that may only shrink.
6. Tests may include a sibling's `src/` (`#include "../src/core/nip46_session.c"` style) **only** inside that package's own `tests/`; cross-package tests use public headers or test fixtures installed to `tests/common/`.

### 9.8 Migration order (implemented)

| Step   | Change                                                                                                                                                                          | Risk                                                                                           |
| ------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------- |
| **R0** | Land `<nostr/nip44/kdf.h>`; delete the `extern` declarations in porthome; add the boundary script with the current violations baselined                                         | nil — pure additive header + deletions                                                         |
| **R1** | Create `nips/bud02` from `hanami-bud02-auth.c`; hanami links it                                                                                                                 | nil                                                                                            |
| **R2** | Create `libblossom` from hanami's client/shim/probe; hanami links it; nostr-homed's `nh_porthome_blossom.c` becomes a 50-line adapter over it; delete `common/blossom_client.c` | medium — re-run the five `porthome-blossom-*` live reviews against libblossom                  |
| **R3** | `git mv` porthome → `libporthome`, compat header, de-GNOME; nostr-homed links `nostrc::porthome`; fix `add_subdirectory` order; remove the `if(TARGET hanami)` block            | medium — pam/authd dependency-closure check (the `CMakeLists.txt` experimental-option fence, originally lines 53–68) must stay green |
| **R4** | `GnBlossomClient` in nostr-gobject; groundhog and nostr-share migrate                                                                                                           | low, but touches two apps — separate PRs                                                       |
| **R5** | `libvault` skeleton (codecs, ring, fold, no carriers) + `tools/vaultctl`                                                                                                        | new code                                                                                       |
| **R6** | `bindings/rust/*-sys` via bindgen from installed C headers; CI builds a Rust consumer                                                                                                          | low                                                                                            |
| **R7** | hanami sealed-ODB mode + `ref_state_node` (D13)                                                                                                                                 | new code, depends R2/R3/R5                                                                     |
| **R8** | `libvault-marmot` carrier (D11)                                                                                                                                                 | new code, depends libmarmot W2x stability                                                      |

R0–R3 landed before shared-vault implementation. R5 onward implements this design proper. The temporary R2 nostr-homed Blossom adapter was subsequently deleted; callers use libblossom directly (§9.3).

***

## 10. Assembly contracts

### 10.1 porthome (`gnome/nostr-homed`)

`syncd` has a `vaults[]` config: for each shared subtree, the `vault_id` and whether this machine's account is owner. The reconciler treats a vault subtree as a second generation source: local capture → seal under the subtree's current epoch → publish *this writer's* head → fold (§5.3) → materialise the owner-authoritative view, writing conflicts to `Portable/.conflicts/<path>@<writer>` (overlay-D3's "truthful folder" principle: never silently drop a colleague's version). Deletion-only conflicts have no alternate file to copy, so durable conflict metadata (exposed through the status seam) records the path, writer/head and presence flags. Over-long conflict names use `Portable/.conflicts/.overflow/<sha256>` with metadata preserving their identity. Syncd prefetches the selected private-service subtrees and conflict objects into its cache; v1 FUSE consumes the folded snapshot and cached sealed objects, **not lazy authenticated private-service GETs**.

### 10.2 libhanami

`hanami_repo_open` takes an optional `const porthome_ring_t *vault`. Present → sealed ODB + `ref_state_node` refdb + heads via libvault; absent → today's behaviour. GRASP push against a sealed repo is refused (a GRASP server cannot verify sealed objects) unless the repo has been made public, in which case a 30618 projection is published and plaintext objects are mirrored — the "lift privacy" path, one-way.

### 10.3 onyx-vault (Tauri, out-of-tree contract)

The intended Rust side links `vault-sys`/`porthome-sys` and exposes commands `vault_share(path, pubkeys, cap)`, `vault_accept(grant)`, `vault_sync()`, `vault_revoke(pubkey)`. For an Onyx integration, the intended contract replaces rather than wraps the external draft's 30800/30801 self-sync: a one-member vault uses the same sealed chunks on Blossom as a shared vault. The draft's 30802 one-shot "send a copy" gesture is conceptually separate. These Onyx kind assignments and existing behavior are **external draft, unverified here**; only the C ABI and Rust bindings, not the Onyx migration, landed in this repository. The intended Onyx grant carrier is NIP-59 until a Rust Marmot (MDK) is wired; the format is identical so the owner can be on either.

***

## 11. Beyond owner-sequencer

D8 is a v1 decision. The documented escalation path, so nobody re-derives it:

1. **Rotating sequencer by owner delegation** — the owner's manifest names a `sequencer` pubkey that may differ from `owner`; same fold rule, different authority. Cheap, and covers "owner on holiday".
2. **Per-path LWW fold without a sequencer** (Concord/Autobase "fold-anytime") — every reader materialises the same view from the frontier by `(written_at, writer pubkey)` tie-break. Deterministic, sequencer-free, but *lossy at the path level* unless conflicts are retained as siblings. Acceptable for a notes vault, not for a git repo.
3. **Threshold or external sequencing** — the Scion critique's requirements (durable atomic allocation, equivocation policy, fork recovery) apply in full; this is a project, not a decision.

Hanami mode already has a better answer than any of these: git merge. Concurrent writers are branches; the owner (or anyone with a write grant) merges. §10.2 should expose that rather than inventing a parallel fold for git-backed vaults.

***

## 12. Decisions

D1–D18 were taken at the recommendations below; §14 records the dispatch decisions and §14a the implementation refinements.

| #       | Decision                        | Options                                                                                                                        | Decision taken                                                                                                                                                                                         |
| ------- | ------------------------------- | ------------------------------------------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **D1**  | Membership model                | (a) ACL events checked by relays/servers (NIP-29, GRASP-08); (b) key possession (Tahoe, CORD-01)                               | **(b).** Only (b) keeps the member list off the server and makes a relay a dumb pipe. (a) is kept as an *additional* factor at the service (D14), never as the source of truth.                        |
| **D2**  | Key scoping                     | (a) one `home_key` per home, share = share everything (PCS v1); (b) per-directory node keys, cryptree derivation with override | **(b).** Sharing a subtree must not leak the home; revocation must not re-key the home.                                                                                                                |
| **D3**  | Chunking                        | (a) fixed-size; (b) content-defined (FastCDC or buzhash, 256 KiB target, 64 KiB–1 MiB bounds)                                  | **(b).** Dedup across edits of large artifacts (PDF, image) is the whole point of convergent sealing; fixed chunks lose it on any insertion. The chunker is a pure function in libporthome.            |
| **D4**  | Convergent vs random chunk keys | (a) keep PCS convergent; (b) random per-file keys                                                                              | **(a).** Team dedup is a feature; PCS §6's threat delta still holds, narrowed to the subtree.                                                                                                          |
| **D5**  | Revocation unit                 | (a) whole home re-key (PCS §9 "Phase 5"); (b) subtree epoch bump via `key_override`                                            | **(b).** (a) is a flag day for every machine and every unrelated subtree.                                                                                                                              |
| **D6**  | Chunk re-seal on epoch bump     | (a) eager (all chunks under S); (b) lazy, `chunk_key_id` per chunk, background re-seal; (c) never                              | **(b), with (a) as an owner-selectable option.** (c) leaves removed members decrypting new content's *neighbours* forever; (a) is correct but can be gigabytes. Uses PCS's reserved field as reserved. |
| **D7**  | Head event kind                 | (a) new kind (external Onyx 30803 draft, unverified here); (b) reuse porthome's kind 30078 with vault-scoped `d`                                           | **(b).** Addressable-per-author gives per-writer heads for free; no registry fight; porthome's existing pointer code is the codec.                                                                     |
| **D8**  | Multi-writer                    | (a) owner-sequencer with per-writer heads retained; (b) deterministic LWW fold; (c) CAS via sequencer service                  | **(a).** Lossless, needs no new theory, matches the team-artifacts use case. §11 records the escalation.                                                                                               |
| **D9**  | Padding                         | (a) none (PCS v1 29-byte overhead); (b) NIP-44 bucket schedule per chunk                                                       | **(b).** Shared Blossom + fingerprintable team documents make size leakage material.                                                                                                                   |
| **D10** | Path privacy                    | (a) PCS §7 keyed hash under subtree `name_key`; (b) real deterministic encryption                                              | **(a).** Unchanged argument from PCS §7; scoped per node it is strictly better than v1.                                                                                                                |
| **D11** | Grant carrier                   | (a) NIP-59 gift wrap only; (b) Marmot only; (c) Marmot primary, gift-wrap fallback, identical body                             | **(c).** (b) excludes every non-MLS client today, including Onyx. (a) has no forward secrecy on delivery and no clean group add/remove.                                                                |
| **D12** | Head body encryption            | (a) NIP-44 to each member (N copies); (b) NIP-44 under `head_key`-derived conversation key (one copy)                          | **(b).** CORD-01's construction; N copies leak N and re-raises NIP-17's fan-out cost.                                                                                                                  |
| **D13** | Git on vaults                   | (a) hanami stays plaintext, vaults are separate; (b) hanami ODB over `porthome_seal`, private `ref_state_node`                 | **(b).** Encrypted history for free; the index already abstracts the address; the only new object is the ref-state node. Public 30618 becomes a projection.                                            |
| **D14** | Service enforcement             | (a) none — public relays/Blossom; (b) VSP-01 allowlist + NIP-42 + 401-not-404                                                  | **(b), optional per vault.** Second factor against removed members and metadata scraping; costs one config knob on relayd and lab-blossom.                                                             |
| **D15** | Discovery                       | (a) hard-code servers in grants; (b) kind-10319 encrypted self-list, grants carry hints                                        | **(b).** GRASP-08's 10318 pattern; lets members move servers without re-granting.                                                                                                                      |
| **D16** | Blossom client consolidation    | (a) leave five; (b) libblossom from hanami's client, GLib adapter in nostr-gobject                                             | **(b).** Non-negotiable precondition; see §9.1.                                                                                                                                                        |
| **D17** | porthome placement              | (a) stays in `gnome/nostr-homed`; (b) `libporthome` at Layer 4, GNOME-free                                                     | **(b).** Onyx and hanami cannot otherwise consume it; pam's dependency closure gets smaller, not larger.                                                                                               |
| **D18** | Boundary enforcement            | (a) convention + review; (b) visibility=hidden, export macros, CI boundary script with shrinking allowlist                     | **(b).** The `extern` hack passed review once; the compiler should refuse it next time.                                                                                                                |

***

## 13. Test contracts

These are the design's verification contracts, not a claim that every route ran on two physical machines or in hosted CI. The epic children's close reasons record actual executions and limits: notably §13(i) ran on one Docker host with two Linux UIDs and headless FUSE callbacks, not a kernel mount or two-machine deployment; R6's Rust consumer ran locally, without hosted CI. The original test labels below describe the areas; component test inventories own the executable names.

| #     | Area                          | Test                                                          | Pass criterion                                                                                                                                               |
| ----- | ----------------------------- | ------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **a** | PCS v2 vectors                | `test_porthome_seal_v2`                                       | Fixed seed, fixed tree → byte-exact sealed blobs and addresses; v1 vectors still pass through the v1 path; cross-version decode refused                      |
| **b** | Cryptree                      | `test_vault_ring_derivation`                                  | `node_key(child)` derivable from parent, not vice versa; `key_override` breaks derivation below and only below                                               |
| **c** | Epoch                         | `test_vault_revoke_lazy`                                      | After revoke: new chunk at `e+1`; old chunk still decrypts with ring `[e]`; ring `[e+1]` alone refuses old chunk with the specified error, no oracle retries |
| **d** | Fold                          | `test_vault_fold_frontier`                                    | Two writers, divergent heads → owner view + per-path conflict list; stale writer (old `grants_rev`) ignored; nothing unreachable                             |
| **e** | Grant round-trip              | `test_vault_grant_nip59`, `test_vault_grant_marmot`           | Same grant body via both carriers yields identical ring; Marmot epoch change changes seed                                                                    |
| **f** | Padding                       | `test_porthome_pad_buckets`                                   | Sealed sizes fall on the NIP-44 schedule; `size_plain` restores exactly                                                                                      |
| **g** | hanami sealed ODB             | `test_hanami_sealed_roundtrip`                                | `git clone` of a sealed repo via libgit2 with ring; Blossom holds no object decodable without the ring; no 30618 published                                   |
| **h** | VSP conformance               | `run_vsp01_lab.sh` against `packaging/lab-blossom` + `relayd` | Non-allowlisted GET → 401 (not 404); unauth relay REQ → `auth-required:`; allowlisted path fully functional                                                  |
| **i** | Two-machine, two-account live | extend `porthome-jmx0-two-machine` to two npubs               | Owner shares `Portable/Team`; member sees it; member edits; owner folds; member removed; member's next fetch fails at service and at ring                    |
| **j** | Boundary script               | CI                                                            | Zero new violations; allowlist count monotonically non-increasing                                                                                            |
| **k** | Rust consumer                 | `bindings/rust/examples/vault-cli`                            | `cargo build` against installed `nostrc::vault`; seal/unseal vectors match C                                                                                 |

***

## 14. Implementation decisions

The shared-vault epic (`nostrc-hl30i`) records these decisions at the design's recommendations, rather than leaving them as dispatch questions:

1. **§9 as a precondition:** R0–R3 first; shared-vault code targets the extracted packages.
2. **D8 — owner-sequencer:** a colleague's edit is provisional until the owner folds it. No LWW or multi-master escalation in v1 (§11).
3. **D13 — git mode included:** R7 delivered hanami's sealed ODB and private ref-state mode; it was not deferred.
4. **Relay-visible `vault_id`:** `d = porthome/v2/<hex vault_id>` is retained. Writer correlation is accepted; rotating labels are parked, not required for v1.
5. **Kind numbers:** grant rumor 30804 and encrypted service list 10319 are implemented as proposed. The external Onyx 308xx draft remains unverified here; these local allocations are not a claim of external registry ratification.
6. **PCS v1 → v2 migration:** no re-seal or chunk re-upload. The old home becomes a v2 vault with root ring `[(1, home_key)]` and `key_override` at each existing directory, preserving access to old content.
7. **`readers[]`:** shipped as informational UI metadata, not an ACL; key possession still enforces reading (§3.4).

***

## 14a. As-built deviations

* **Owner-checkpoint fold (§5.3):** addressable heads do not supply ancestor closure. `vault_fold_checkpoint` anchors policy on the verified current owner head; `vault_head_cover` carries per-writer sequence coverage forward (`libvault/src/{fold,parents}.c`, `libvault/README.md`).
* **Sealed-body metadata (§3.4):** required `name_plain` and per-chunk `size_plain` complete the unreleased v2 schema in place; no owner-private name/length sidecar is needed (`libporthome/include/porthome/dir-node.h`; `nostrc-hl30i.10`).
* **Private-service reads (§10.1):** syncd prefetches the private-service subtrees into cache; v1 FUSE does not lazily fetch them with authenticated GETs (`nostrc-hl30i.10`).
* **Conflict representation (§10.1):** deletion-only conflicts remain in durable metadata, while over-long names use `.conflicts/.overflow/<sha256>` plus metadata instead of losing the conflict (`nostrc-hl30i.10`).
* **Marmot seam (§1, §6.3):** libmarmot needed the additive `marmot_export_secret` API; it was not merely consumed unchanged (`nostrc-hl30i.7`, `libvault-marmot/src/seed.c`).
* **Rust generation (§9.2/R6):** bindgen generates Rust from installed C headers; cbindgen would be the opposite direction (`nostrc-er4fz.7`, `bindings/rust/*-sys/build.rs`).
* **Late storage outcome (§7):** relayd sends NOTICE and closes at the confirmation deadline without an OK for an unknown outcome; a later confirmed commit can still fan out (`apps/relayd/README.md`, `apps/relayd/src/protocol_nip01.c`).
* **Widget ownership (§9.2):** GNostr-coupled note row/embed, thread view and profile pane moved out of Layer 6 nostr-gtk into Layer 7 `apps/gnostr/src/widgets` (`gnostr-widgets`); reusable widgets remain in nostr-gtk (`apps/gnostr/CMakeLists.txt`, `docs/TIMELINE_LIBRARY_APP_BOUNDARY.md`).

***

## 15. References

* `docs/designs/home-from-relay.md` — §2.2 pointer, §4.1 `key_epoch`, §4.3 `wrapped_home_key`, §6.3 multi-writer, §8 threat model, HFR-D18.

* `docs/designs/porthome-crypto-spec.md` — §2 salts, §3 hierarchy, §4 layout, §5 strictness, §6 threat delta, §7 names, §9 deferred (rotation, padding), D4 approval 2026-09-23.

* `docs/designs/nostrfs-porthome-overlay.md` — §11 D15 non-goals ("MLS / group-shared homes … would arrive by the same seam"), §12 D3/D4/D5.

* `docs/libhanami-design.md` — §4.4 OID↔Blossom index, §3.3 NIP-34 kinds, §3.4 auth.

* `docs/investigations/scion-nostr-blossom-doc-vs-libhanami-2026-10-13.md` — §A2 CAS objection, §A3 retention.

* `docs/MLS_INTEGRATION.md`, `docs/MARMOT_API.md` — exporter secret, kind 443/444/445.

* `docs/reviews/porthome-blossom-{batch-auth,content-type,shim,body-size,band-bud02-conformance}-2026-09-2x.md` — the evidence base that must be re-run against libblossom (R2).

* NIP-34, NIP-42, NIP-44 (extended length prefix, 2026), NIP-51, NIP-59, NIP-78, NIP-11.

* Blossom BUD-01/02/03/04; GRASP-01, GRASP-08 (ngit.dev/protocol/grasp/specification/08).

* Marmot MIP-00–04; Concord CORD-01, CORD-06; RFC 9420 §8 (exporters).

* Tahoe-LAFS capability model; Peergos cryptree; Willow/Meadowcap (path-scoped read/write capabilities) — prior art for D1/D2.
