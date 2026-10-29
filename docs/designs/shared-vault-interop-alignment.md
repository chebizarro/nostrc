# Shared vaults — interoperability alignment note

**Status:** Proposal (follow-up to `docs/designs/shared-vault-composition.md`). **Pre-release.** Every change here is intended to land before the first tagged release of `libvault`/`libporthome`, while the wire format can still move without a migration.
**Beads:** follow-ups to `nostrc-er4fz`, `nostrc-hl30i`; one new bead per §2 item is proposed in §4.
**Reads:** `docs/designs/shared-vault-composition.md` (SVC §3.4 manifest, §5.1 head `d`, §6.2 grant, §6.3 carriers, §8 threat model, §14 open items), `libvault/include/vault/vault.h`, `libporthome/include/porthome/{dir-node,head,manifest}.h`.
**Date:** 2026-10-28

> Decision numbering here is **A1…A9**; SVC decisions are cited as **SVC-Dn**.

---

## 0. Summary

A prior-art sweep on 2026-10-27 found no implementation that solves the shared-vault problem, but it found three Nostr drafts that arrived independently at the same key tree during September–October 2026, one shipping product with a weaker model, and one adjacent failure (Radicle) that is the strongest available argument for SVC-D1. None of this invalidates the design or the implementation. It does mean that **the first release will enter a field with competing kind numbers and tag vocabularies**, and that a handful of small, mostly additive changes now will save a compatibility layer later.

The changes, in priority order:

| # | Change | Kind | Wire impact | Effort |
| --- | --- | --- | --- | --- |
| A1 | Register/coordinate kinds 30804 and 10319 and the `porthome/v2/` `d` prefix before release; approach #2412's authors | process | none if done now; a rename later is a migration | days of conversation, hours of code |
| A2 | Add a NIP-94 / kind-15 *projection* for single-file export from a vault | additive API | none to vault format | small |
| A3 | Accept a NIP-4E device key (kind 10044) as a gift-wrap recipient; document that vault keys already satisfy NIP-4E's goal | additive carrier option + doc | none | small |
| A4 | Related-work section in SVC naming #2412, NIP-FS, #2437, NIP-4E, Proton Drive, Radicle | doc | none | small |
| A5 | Cite Radicle (2026-09-23) in SVC §8 and tighten SVC-D14 wording: VSP is never the only control | doc | none | trivial |
| A6 | Add the "public-GET Blossom makes the address a ciphertext capability" line to §8 | doc | none | trivial |
| A7 | Fix the stale `chunk_key_id` "MUST be 0" contract in `manifest.h` to match `dir-node.h` v2 semantics | code/doc | none (comment + decoder guard) | trivial |
| A8 | Record Proton's asymmetric-node-key design as considered-and-rejected for SVC-D2 | doc | none | trivial |
| A9 | `vaultctl import` reader for NIP-FS single-blob files (Formstr Drive) — optional | additive tool | none | medium; defer |

---

## 1. What the sweep found (condensed; links in §5)

- **NIPs PR #2412 "metadata and drive"** (geralt-debugs, theLockesmith; open, WIP). theLockesmith's half derives file keys by HKDF from a folder key, chunk nonces from a base nonce, addresses as sha256(ciphertext), and revokes by rotating the folder key — SVC §3.2 without epochs, padding, or multi-writer, and with plaintext `folder`/size/hash tags on kind 30078 that the authors themselves call a leak. geralt's half uses per-file keys and encrypted child references. They note that re-keying means re-encrypt and re-upload and that it's costly — exactly the gap SVC-D5/D6 close. **Kind 30078 collision risk is real:** both we and they hang state off NIP-78.
- **NIP-4E** (PR #1647): separate "encryption key" from identity key; kinds 10044 (announce), 4454/4455 (device exchange). Shipped by Jumble and Coop; opposed by vitorpamplona (reliability, rotation tracking) and sondreb (fragmentation); fiatjaf prefers the simpler identity-key-encrypted sharing. Unmerged.
- **Formstr Drive / "NIP-FS"** (shipping at drive.formstr.app): 64 KiB segments, AES-256-GCM directly under the NIP-44 conversation key, nonce = segment index, no HKDF, no per-file key, one blob per file. Folder sharing removed from the UI because NIP-FS marks it TBD. Not compatible; not a model to adopt.
- **NIPs PR #2437 "NIP-94 file encryption"** (Alex Gleason): kind-15's `decryption-key`/`nonce`/`x` tag vocabulary extended to `imeta`. staab's history in-thread: tried encryption, moved to auth-protected Blossom, dropped encryption because "the hash is effectively the password."
- **cloistr-stash/coldforge-drive, cmdruid/nostr-storage, holzi**: plaintext-metadata drive, single-secret store, and a Cryptomator-style key chain on S3 respectively. Prior art only.
- **nostr-crdt** (Yjs over relays): the only live multi-writer answer in Nostr, text only. Compatible with SVC-D8 as an *application* layer on top of a vault file.
- **Radicle, 2026-09-23**: private repositories were access-controlled, not encrypted; two vulnerabilities (cleartext private-repo traffic, peer impersonation) affect every released version; users told to stop using private repos; fix is a breaking protocol upgrade.
- **Proton Drive** (public model): random share passphrase + asymmetric share key; per-node asymmetric keys locked by the parent; filenames under the parent node key; ≤4 MB signed block chain; new member = re-wrap one session key. Revocation unspecified.
- **Autonomi `self_encryption`**: pure convergent chunking (keys from neighbour chunk hashes, no vault key) — global confirmation-of-content, which PCS deliberately avoids.

**What nobody has:** per-chunk epoch with lazy re-seal (SVC-D6), a DAG head with retained per-writer heads (SVC-D8), MLS exporter as the seed for a persistent tree (SVC-D11). These are the implementation's defensible novelties; the note below does not touch them.

---

## 2. Proposed changes

### A1 — Kind registry and `d` prefix, before release

**Problem.** SVC §14 item 5 treated 30804 (grant rumor) and 10319 (private service list) as a numbering question. It is now a coordination question: #2412 is unfinalised and also uses 30078; Onyx's draft uses 30800–30802; GRASP-08 owns 10318 and we are proposing its neighbour.

**Change.**
1. Open a NIPs PR (or a comment on #2412) that **declares** the `d` prefix `porthome/v2/` on kind 30078 and the two kinds, with one-paragraph semantics each, before the first `libvault` tag. Addressable-kind `d` prefixes are the established way to share NIP-78 without colliding; declaring ours makes a later collision their bug, not ours.
2. Ask the #2412 authors whether they will adopt the `porthome/v2/*` HKDF constants (SVC §3.2) as the common derivation. The trees are isomorphic; the only differences are our epochs, `key_override`, padding, and encrypted structure. If they agree, their drive and our vault become the same object at the key layer and only the manifest encoding differs. If they don't, record the divergence in A4 and move on — do **not** add a second derivation path to `libporthome`.
3. If 30804 proves contested, the fallback is to move the grant to a **regular** kind in the 1xxx–9xxx range; nothing in `vault_grant_encode` depends on the number. Decide before release; after release it is a migration.

**Code.** None beyond possibly one constant. **Owner.** Maintainer (outreach), one bead.

### A2 — NIP-94 / kind-15 projection for single-file export

**Problem.** A vault member often wants to send *one* file to someone who will never hold the vault — a White Noise, 0xchat or Amethyst user. Today that requires the recipient to understand sealed chunks, rings and manifests. Kind 15 (NIP-17 encrypted file) and #2437's `imeta` extension already define how an encrypted Blossom blob is described: `x` (hash), `decryption-key`, `decryption-nonce` (as a hex string, so 'nonce'), `encryption-algorithm`, `file-type`, `size`.

**Change.** Add to `libvault`:

```c
/* Produce NIP-94 / kind-15 style tags for one file so it can be sent to a
 * recipient who does not hold the vault. Re-seals the file as ONE blob under a
 * fresh random key (never exports a ring key or a derived chunk key), uploads
 * it, and returns the descriptor tags. Caller wraps them in a NIP-17 kind 15 or
 * an imeta on any event. */
VAULT_API int vault_file_export_nip94(const vault_ctx *, const porthome_dir_entry *file,
                                      blossom_client_t *, vault_nip94_tags **out);
```

Export re-seals rather than exposing a chunk key because (a) a derived chunk key would leak nothing by itself but establishes a habit of exporting ring-derived material, and (b) kind-15 clients expect one blob, not a chunk list. Algorithm string: `"aes-gcm"` (what kind-15 clients implement today), with the sealed-blob format noted as a future value once #2437 settles.

**Wire impact on vaults.** None. **Effort.** Small; it is `porthome_seal` with a random key plus a tag builder. **Owner.** one bead.

### A3 — NIP-4E device keys as gift-wrap recipients

**Problem.** The NIP-59 carrier (SVC §6.3) wraps to the recipient's *identity* key, which is exactly the thing NIP-4E users (Jumble, Coop) have moved off their devices. For them the grant is undeliverable or lands on a signer that must then forward it.

**Change.**
1. `vault_grant_send_nip59()` gains an optional recipient-key resolver: if the recipient publishes kind 10044, wrap to that key instead of the identity key; record which was used in the wrapper's own tags (not the grant body). Receiving is symmetric: try the device key first, fall back to identity.
2. Add one paragraph to SVC §6.3 stating that **vault keys already satisfy NIP-4E's stated goal** — content is never encrypted under an identity key — so the design is neutral to how that PR resolves, and the only touchpoint is the grant envelope.

**Wire impact.** None to the grant body. **Effort.** Small; nip44 conversation key with a different pubkey. **Owner.** one bead; implement behind a runtime option until 10044 semantics are merged.

### A4 — Related-work section in SVC

Insert as a new §1.1 ("Independent designs, Sept–Oct 2026") between ground truth and goals: one row each for #2412, NIP-4E, NIP-FS, #2437, cloistr/coldforge, nostr-storage, holzi, nostr-crdt, Proton Drive, Autonomi self_encryption, Radicle — what it does, what it shares with SVC, what it lacks. The table in §1 above is the draft. Purpose is reviewer orientation and a citable answer to "why not just use X."

### A5 — Radicle citation and D14 wording

In SVC §8, add a row: *"Access-controlled 'private' storage without E2EE — Radicle private repositories, every released version, disclosed 2026-09-23: cleartext inter-node traffic, peer impersonation; remediation requires a breaking protocol upgrade."* In SVC-D14, change "optional per vault" to "**an additional factor; never the only control. A vault whose confidentiality depends on VSP allowlisting is misconfigured.**" The implementation already behaves this way (`vault_vsp_blossom_classify` only tightens); this is a wording fix so nobody reads D14 as permission to skip sealing.

### A6 — Address-as-capability line in §8

staab's observation (a Blossom hash is effectively the password) is true for *plaintext* blobs on a public-GET server. For sealed blobs it reduces to: *on a public-GET Blossom, knowing an address lets anyone fetch the ciphertext; it never yields plaintext without the ring.* Add that sentence to §8 so the two cases are not confused, and note it as the second reason (after metadata) for VSP's `401`-on-GET rule.

### A7 — `chunk_key_id` contract drift

`libporthome/include/porthome/manifest.h:64` and `:209` still carry the PCS v1 text ("Reserved; MUST be 0 in Phase 1 … refuses non-zero `chunk_key_id`"), while `dir-node.h:60` encodes v2 semantics (per-chunk epoch) and `libvault` depends on them. Before release: (a) the v1 manifest codec keeps the refusal (it is correct for v1 blobs), (b) the comment states that v2 dir-node payloads carry the epoch and the refusal applies **only** to the v1 codec, (c) a test asserts that a v1-codec decode of a v2 node fails closed with `PORTHOME_ERR_EPOCH`, not with a silent zero. Doc + guard; no wire change.

### A8 — Proton's asymmetric node keys, considered

Record under SVC-D2: Proton Drive gives every node its own asymmetric keypair so a new member is added by re-wrapping one session key with no derivation chain. Rejected for SVC because (i) a keypair per directory node is a per-node cost we avoid with HKDF, (ii) HKDF derivation already gives add-without-rekey (SVC §3.2), and (iii) the symmetric ring is what the Marmot exporter seam produces. The one thing Proton's model does better — granting a *single file* without the folder — is served by A2.

### A9 — NIP-FS import (optional, defer)

Formstr Drive users have blobs in a documented-enough format (AES-256-GCM segments under a NIP-44 conversation key, nonce = index‖last-flag, 64 KiB + 16). A `vaultctl import --nipfs` that reads those with the user's own key and re-seals them into a vault would be a one-way on-ramp. Low priority; not a release blocker; file as a bead and leave it.

---

## 3. What this note does **not** change

- Derivation constants (`porthome/v2/{home,node,chunk,manifest,name,head}`), sealed-blob layout, padding, epochs, fold rule, grant body, carriers. All stand.
- The decision to reuse kind 30078 for heads (SVC-D7). A1 protects it rather than replacing it.
- The owner-sequencer (SVC-D8). nostr-crdt is an application-layer complement for text, not a replacement for the tree fold.

---

## 4. Proposed beads

| Bead | Title | Depends on |
| --- | --- | --- |
| SV-A1 | Declare `porthome/v2/` prefix + kinds 30804/10319 upstream; #2412 outreach | — (blocks release) |
| SV-A2 | `vault_file_export_nip94` | libblossom upload path |
| SV-A3 | NIP-4E device-key recipient option for the NIP-59 carrier | — |
| SV-A4–A6, A8 | SVC doc edits (one PR) | — |
| SV-A7 | `manifest.h` contract + fail-closed test | — (blocks release) |
| SV-A9 | `vaultctl import --nipfs` | deferred |

Release gate: **A1 and A7.** Everything else can follow in the first point release without a format change.

---

## 5. References

- NIPs PR #2412 — https://github.com/nostr-protocol/nips/pull/2412
- NIPs PR #1647 (NIP-4E) — https://github.com/nostr-protocol/nips/pull/1647
- NIPs PR #2437 (NIP-94 file encryption) — https://github.com/nostr-protocol/nips/pull/2437
- Formstr Drive PR #69 (NIP-FS single blob) — https://github.com/formstr-hq/formstr-drive/pull/69 ; PR #71 (sharing) — https://github.com/formstr-hq/formstr-drive/pull/71
- theLockesmith/cloistr-stash — https://github.com/theLockesmith/cloistr-stash
- cmdruid/nostr-storage — https://github.com/cmdruid/nostr-storage
- holzi PR #358 — https://github.com/haexmas/holzi/pull/358
- nostr-crdt — https://www.npmjs.org/package/nostr-crdt
- Radicle private-repository disclosure — https://runtimewire.com/article/radicle-network-protocol-vulnerabilities-private-repositories ; https://lwn.net/Articles/1096200/
- Proton Drive security model — https://proton.me/blog/protondrive-security
- Autonomi self_encryption — https://docs.rs/crate/self_encryption/latest
- Peergos (FOSDEM 2026) — https://fosdem.org/2026/schedule/event/peergos-e2ee-web
- GRASP-08 — https://ngit.dev/protocol/grasp/specification/08
