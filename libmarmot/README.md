# libmarmot

Pure C implementation of the [Marmot protocol](https://github.com/marmot-org/mdk) (MLS + Nostr) for secure group messaging.

## Overview

libmarmot implements the Marmot Improvement Proposals (MIPs) for encrypted group messaging over Nostr using the Messaging Layer Security (MLS) protocol (RFC 9420):

| MIP | Description | Event Kind | Status |
|-----|-------------|------------|--------|
| MIP-00 | Credentials & KeyPackages | 30443 (addressable) | ✅ Complete |
| MIP-01 | Group Construction (Extension 0xF2EE) | — | ✅ Complete |
| MIP-02 | Welcome Events (NIP-59 gift-wrapped) | 444 | ✅ Complete |
| MIP-03 | Group Messages (NIP-44 encrypted) | 445 | ✅ Complete |
| MIP-04 | Encrypted Media (ChaCha20-Poly1305) | — | ✅ Complete |

**Ciphersuite**: `MLS_128_DHKEMX25519_AES128GCM_SHA256_Ed25519` (0x0001) — the only ciphersuite mandated by the Marmot protocol.

## Architecture

```
┌──────────────────────────────────────────────────────┐
│                 Public API (marmot.h)                 │
│  marmot_create_group / process_welcome / …            │
├──────────────────────────────────────────────────────┤
│                Protocol Layer (MIP-00─04)              │
│  credentials · groups · welcome · messages · media    │
├──────────────────────────────────────────────────────┤
│                  MLS Layer (RFC 9420)                  │
│  mls_crypto    — X25519, Ed25519, AES-128-GCM, HKDF  │
│  mls_tls       — TLS presentation language codec      │
│  mls_tree      — TreeKEM ratchet tree (left-balanced) │
│  mls_key_sched — Epoch secrets, sender ratchets       │
│  mls_framing   — PrivateMessage encrypt/decrypt       │
│  mls_key_pkg   — KeyPackage creation/validation       │
│  mls_group     — Group state machine                  │
│  mls_welcome   — Welcome message construction         │
├──────────────────────────────────────────────────────┤
│              Storage Interface (vtable)                │
│  memory · sqlite · nostrdb  backends                  │
├──────────────────────────────────────────────────────┤
│  libsodium (Ed25519/X25519)  OpenSSL (AES/HKDF/SHA)  │
└──────────────────────────────────────────────────────┘
```

## Dependencies

| Library | Purpose | Required |
|---------|---------|----------|
| **libsodium** | Ed25519 signing, X25519 DH, ChaCha20-Poly1305, CSPRNG | ✅ |
| **OpenSSL** | AES-128-GCM, HKDF-SHA256, SHA-256 | ✅ |
| **libsecp256k1** | Schnorr/x-only keys for Nostr events | ✅ |
| **libnostr** | Nostr event creation, signing | ✅ |
| **libnostrgo** | `string_array` used by libnostr's public tag API | ✅ |
| **NIP-44** (`nostr_nip44_core`) | Content encryption for group messages | ✅ |
| **NIP-59** | Gift wrapping for welcome events | Optional (for MIP-02) |
| **SQLite3** | Persistent storage backend | Optional |
| **LMDB** | nostrdb storage backend | Optional |

## Building

### CMake (in-tree, part of nostrc)

```bash
cd nostrc
cmake -B build -DBUILD_LIBMARMOT=ON -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build -R marmot --output-on-failure
```

### CMake (standalone)

```bash
cd nostrc/libmarmot
cmake -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

### Meson (standalone)

libnostr, libnostrgo and NIP-44 are required. By default they come from
installed copies via pkg-config (`nostr`, `libnostrgo`) with
`libnostr_nip44_core` next to libnostr; `nips/nip44` does not install that
library yet, so configure then stops with an explicit error. Alternatively,
take them from an already-built nostrc CMake tree:

```bash
cd nostrc
cmake -B /tmp/nostrc-build && cmake --build /tmp/nostrc-build --target nostr nostrgo nostr_nip44_core
meson setup /tmp/marmot-meson libmarmot -Dnostrc_build_dir=/tmp/nostrc-build
meson test -C /tmp/marmot-meson
```

libmarmot's own link closure needs no nsync, libwebsockets or GLib.

## API Overview

### Lifecycle

```c
#include <marmot/marmot.h>

// Create with default config and in-memory storage
MarmotStorage *storage = marmot_storage_memory_new();
Marmot *m = marmot_new(storage);

// ... use the API ...

marmot_free(m);  // also frees the storage
```

### MIP-00: Credentials & KeyPackages (kind 30443)

```c
MarmotKeyPackageResult result;
MarmotError rc = marmot_create_key_package_unsigned(m, my_pubkey,
                                                    relay_urls, relay_count, &result);
// result.event_json      → unsigned kind:30443 event to sign and publish
// result.key_package_ref → 32-byte KeyPackageRef (the event's `i` tag)
marmot_key_package_result_free(&result);

// Inviter side: pick one KeyPackage from everything the relays returned.
size_t idx;
rc = marmot_select_key_package_event(fetched_jsons, fetched_count,
                                     invitee_pubkey, &idx);
// fetched_jsons[idx] → pass to marmot_create_group() / marmot_add_members()
```

Every KeyPackage of an account reuses that account's `d` publication slot, so a
rotated KeyPackage replaces the previous one on relays.

### MIP-01: Group Construction

```c
MarmotGroupConfig config = { .name = "My Group", .description = "...", ... };
MarmotGroupResult result;
int rc = marmot_create_group(m, creator_pubkey, members_kp_events, member_count, &config, &result);
// result.group → the created group
// result.welcome_rumors → NIP-59 gift-wrap these for each member
// result.evolution_event → kind:445 commit event
marmot_group_result_clear(&result);
```

### MIP-02: Welcome Events (kind 444)

```c
MarmotWelcomePreview preview;
int rc = marmot_process_welcome(m, event_id, welcome_rumor, &preview);
// preview.group_name, preview.member_count, etc.
rc = marmot_accept_welcome(m, &preview);
```

### MIP-03: Group Messages (kind 445)

```c
// Send
MarmotOutgoingMessage out;
int rc = marmot_create_message(m, group_id, inner_event, &out);
// out.event_json → kind:445 event, signed by a fresh ephemeral key: publish as is

// Receive (application messages and Commits share kind:445). A relay's
// event must carry a valid id and signature (checked first, since 0.6.0);
// a rumor from a NIP-59 gift wrap goes to marmot_process_rumor_message().
MarmotMessageResult result;
rc = marmot_process_message(m, received_event, &result);
if (result.type == MARMOT_RESULT_APPLICATION_MESSAGE) {
    // result.app_msg.inner_event_json → decrypted inner event
} else if (result.type == MARMOT_RESULT_COMMIT) {
    // the group moved to a new epoch; result.commit.updated_group
}

// Group changes return a pending Commit (signed kind:445): publish it, then
// merge once a relay answered OK, or clear it if none did.
char *commit_json = NULL;
rc = marmot_update_group_metadata(m, group_id, &config, &commit_json);
if (publish_until_relay_ok(commit_json))          /* the application's relay code */
    rc = marmot_merge_pending_commit(m, group_id);  /* MARMOT_ERR_WRONG_EPOCH: lost a race */
else
    rc = marmot_clear_pending_commit(m, group_id);
```

### MIP-04: Encrypted Media

```c
MarmotEncryptedMedia enc;
int rc = marmot_encrypt_media(m, group_id, file_data, file_len, "image/png", "photo.png", &enc);
// enc.encrypted_data, enc.nonce, enc.file_hash → upload encrypted blob
// enc.imeta → metadata for the group message

uint8_t *decrypted;
size_t dec_len;
rc = marmot_decrypt_media(m, group_id, encrypted_data, enc_len, &imeta, &decrypted, &dec_len);
```

## Storage Backends

| Backend | Constructor | Persistence | Notes |
|---------|------------|-------------|-------|
| In-memory | `marmot_storage_memory_new()` | ❌ | Testing, ephemeral use |
| SQLite | `marmot_storage_sqlite_new(path, key)` | ✅ | Optional encryption via SQLCipher |
| nostrdb | `marmot_storage_nostrdb_new(ndb, lmdb_env)` | ✅ | LMDB-backed, shares nostrdb instance |

All backends implement the same `MarmotStorage` vtable (optional
`begin`/`commit`/`rollback` transaction hooks since 0.7.0):
- Group CRUD (save, find by MLS ID, find by Nostr ID, list all, update relays)
- Message operations (save, find, pagination, last message, processed tracking)
- Welcome operations (save, find, pending list, processed tracking)
- MLS key-value store (label+key → value, for MLS internal state)
- Exporter secrets (per-group per-epoch secret storage)
- Snapshots (for commit race resolution)

## MDK Interoperability

libmarmot targets wire interoperability with [MDK](https://github.com/marmot-protocol/mdk)
0.8 (the legacy 0xF2EE profile White Noise 0.8 builds on).
Until 0.11.0 this was a design goal, not a tested fact: the first live test
found five wire deviations (see the 0.11.0 changelog).

What is tested now:

- **Live, both directions.** `gnome/groundhog/tests/mls/test_mdk_interop.c`
  runs Groundhog's `GhMlsService` against MDK v0.8.0 (`575ae29d`) on local
  relays. It is opt-in (`-DBUILD_MDK_INTEROP=ON`, Docker), and runs on demand
  and nightly in CI. See `tests/interop/mdk/README.md`.
- **Vectors.** `tests/test_interop.c` checks libmarmot against MDK:
  - marmot_group_data exactly as MDK encodes it (decoded and re-encoded byte
    for byte);
  - an MDK kind:445 content with its epoch key;
  - OpenMLS message-protection and tree vectors in `tests/vectors/mdk/`.
- **Protocol constants**: kind:30443/444/445, extension type 0xF2EE.

What does not hold yet:

- **Default mode.** Without `MarmotConfig.allow_unproven_members`, MDK 0.8
  members are refused (0.10.0, below).
- **Standalone proposals.** They are not processed, so an MDK member's
  SelfRemove is not seen (nostrc-2um6).
- **The adopted profile.** MDK 0.9 and later use it; libmarmot does not speak
  it yet (nostrc-qp24.5.1).

## Changelog

### 0.12.0 (unreleased): MIP-04 encrypted media v2 and group image components (nostrc-u7cb)

**New format, old one retired** (MINOR for 0.x). Media now follows
`features/encrypted-media.md` (encrypted-media-v2) at marmot-protocol/marmot
`07da8ffb`, byte for byte against MDK v0.11.0 (`tests/vectors/media/`):

- `marmot-media.h`: `marmot_media_encrypt()` seals for the current epoch with
  `HKDF-Expand(MLS-Exporter("marmot", "encrypted-media", 32), "encrypted-media-v2"
  0x00 plaintext_sha256 0x00 media_type 0x00 filename 0x00 "key")` and an AAD
  binding the hash, canonical MIME type and filename; `marmot_media_decrypt()`
  takes the carrying message's epoch (current or retained), checks the
  ciphertext SHA-256, opens, then checks the plaintext SHA-256.
- `marmot_media_imeta_build()`/`_parse()`: the ordered v2 `imeta` tag
  (`v`, `locator`s, `ciphertext_sha256`, `plaintext_sha256`, `nonce`, `m`,
  `filename`, optional `dim`/`thumbhash`) with every validation rule of the
  spec, MDK's shared fixture verdicts included; `marmot_media_type_canonicalize()`.
- `MarmotMessageResult.app_msg.epoch`: the epoch a received message was sent
  in, the source epoch of its attachments (late messages read with the
  retained parent report the parent's epoch).
- Group image components: `0x8002` marmot.group.blossom.image.v1 codec and
  image AEAD (`marmot_group_image_encrypt/decrypt`, fresh key, nonce and
  Blossom upload key), `0x8007` marmot.group.avatar-url.v1 codec with a
  strict WHATWG-serializer subset (IDNA hosts are refused, not repaired), and
  `marmot_group_avatar_select()` (the URL avatar wins). They are not yet read
  from or written to live groups (that needs AppDataUpdate and adopted
  admission).

Review follow-ups (nostrc-u7cb):

- **SONAME `libmarmot.so.0.12`** (`.0.12.dylib`): while 0.x the SONAME is
  0.MINOR in CMake and meson, because a 0.x MINOR may break the ABI and this
  one does (`MarmotMessageResult` grew `app_msg.epoch`).
- **0x8007 decode is three-way.** Valid (inside libmarmot's subset, byte
  equal), invalid (provably not any WHATWG serializer's output) or
  unverified: accepted, kept byte for byte, `url_unverified` set, rendered as
  a placeholder and never contacted (`MARMOT_GROUP_AVATAR_URL_PLACEHOLDER`).
  WHATWG implementations already disagree (url 2.5.8 and ada-url on `^` in a
  path and on `..` over a `b:` segment), so libmarmot never refuses a Commit
  over a URL another parser calls canonical.  Producing stays strict.
- `marmot_media_check_epoch()`: reconciles an interrupted epoch transition
  before comparing, as `marmot_create_message()` does
  (`MARMOT_ERR_MEDIA_EPOCH_CHANGED`).
- The read-only legacy decryptor now requires `file_hash`.

**Incompatible:** the pre-0.12 media format (HMAC `marmot-media-key` of the
raw exporter secret, MIME-only AAD) matched neither v2 nor the frozen v1, so
no other client could read it. `marmot_encrypt_media()` now returns
`MARMOT_ERR_MEDIA_LEGACY_FORMAT`; `marmot_decrypt_media()` stays, deprecated
and read-only, for references already stored. Frozen encrypted-media-v1 is
not read (its validity rules need the frozen unsafe-host set): such a tag is
`MARMOT_ERR_MEDIA_UNSUPPORTED_VERSION`.
### 0.12.0 (unreleased): hardening after the W23 MDK review (nostrc-c7ho, nostrc-w285, nostrc-2lrz, nostrc-dkiq)

**API behaviour change** (MINOR for 0.x); no wire or state-format change.

#### What changed

- **libmarmot 0.10.0 GroupData only from our own state (nostrc-c7ho).**
  `marmot_group_data_extension_deserialize()` reads MIP-01 only. The
  0.10.0 layout is read only from GroupData we stored ourselves: a stored
  group's, or a Commit's that leaves it byte-identical. GroupData from a
  Welcome, or one a Commit writes, must be MIP-01. Before, the 0.10.0
  fallback ran on every input, network input included (W23 review L3).
- **MIP-01 version 1 is read (nostrc-c7ho).** Both encodings MDK used:
  without `image_upload_key` (MDK before December 2025) and with it empty
  (MDK 0.8). Written back as MDK 0.8 writes it. A v1 `image_key` is the
  image's encryption key itself, not a v2 seed, and `MarmotGroup` does not
  carry the version yet (nostrc-x215).
- **An invitation survives a storage error (nostrc-w285).**
  `marmot_accept_welcome()` refuses a Welcome for good only when its raw
  data is missing (`MARMOT_ERR_STORAGE_NOT_FOUND`, was
  `MARMOT_ERR_STORAGE`); any other load error is returned and the Welcome
  stays pending (W23 review L1). A refusal whose failed state cannot be
  saved records nothing, leaves the Welcome pending and returns the save's
  error (L2).
- **A group's kind:445 events carry strictly increasing created_at
  (nostrc-2lrz).** MDK 0.8 never retries a kind:445 it failed once, so a
  Commit of epoch n+1 it reads before the one of epoch n strands it, and
  created_at is the only order relays give. Every Commit and application
  message we publish to a group is dated after the previous one, and after
  the newest Commit of another member we applied (at most a minute ahead of
  our clock). The floor is kept per `nostr_group_id` in `mls_kv`, label
  `group_event_created_at`; group snapshots leave it alone, since a Commit
  rolled back was still published.
- **Tests for Commits sent as PrivateMessages (nostrc-dkiq, W23 review
  M1).** OpenMLS's message-protection `commit_priv` vector is opened as
  `private_commit_open()` does. Through `marmot_process_message()`, a
  PrivateMessage Commit built in-tree applies, and each forgery is refused:
  another member's sender data, a blank leaf, a tampered ciphertext,
  non-zero padding, a PublicMessage signature, another epoch, an Add whose
  account proof fails. At the MLS layer: another epoch, a refused Commit
  consuming no handshake key, a generation read before.

### 0.11.0 (unreleased): Marmot wire conformance, found by the first live MDK 0.8 test (nostrc-7gx7, nostrc-77pa)

**Wire change** (MINOR for 0.x). 0.11.0 and 0.10.0 or older cannot read each
other's kind:445 events: upgrade whole groups together.

Before this release libmarmot was never run against another Marmot
implementation. Its self-tests passed because both sides were libmarmot.
Against MDK v0.8.0 (`575ae29d`), each deviation below broke the exchange.
Each is cited against the legacy Marmot text (marmot-protocol/marmot
`cc73aa8`) or RFC 9420, and each has a test.

#### What changed

- **kind:445 content (MIP-03).**
  - Now: `base64(nonce || ChaCha20-Poly1305(key, nonce, MLSMessage, aad ""))`,
    where `key = MLS-Exporter("marmot", "group-event", 32)` of the epoch.
    `marmot_group_event_encrypt()` and `marmot_group_event_decrypt()`
    (internal) keep taking the epoch's exporter_secret; `_with_key`
    variants take the derived key.
  - Before: NIP-44 with the raw exporter_secret as a secp256k1 key. That is
    MIP-03's text before marmot #48. MDK 0.8 still read it as a legacy
    fallback, but only until 2026-05-15 (its
    `LEGACY_EXPORTER_SECRET_MIGRATION_DEADLINE`), so MDK read none of our
    messages or Commits.
  - The old format is neither written nor read any more.
  - Application messages are zero-padded inside the MLS PrivateMessageContent
    to NIP-44's length buckets (RFC 9420 section 6.3.1), since the new AEAD
    pads nothing.
- **marmot_group_data (0xF2EE, MIP-01 version 2).**
  - Now encoded as MIP-01 specifies: every vector QUIC-varint prefixed
    (admins included), and the image fields as `opaque<V>` (empty or exact
    size) instead of a `has_image` byte and fixed fields.
  - Neither side decoded the other's extension: MDK could not read the
    group data of our Welcomes, and we joined MDK's groups without name,
    relays or admins (vectors captured from MDK in `tests/test_interop.c`).
  - The 0.10.0 layout is still read (strictly, and only when the MIP-01
    layout does not parse), so existing groups load. It is never written.
  - A later version's appended fields (v3 `disappearing_message_secs`) are
    kept verbatim in the new `extra`/`extra_len` fields and written back
    (MIP-01 forward compatibility).
- **Joining needs exactly one marmot_group_data** (MIP-01, MIP-02 step 3).
  A Welcome whose group has none, several, or one that does not decode is
  refused (`MARMOT_ERR_EXTENSION_FORMAT`). Such groups used to be joined
  without a nostr_group_id.
- **required_capabilities (MIP-01 "Required MLS Extensions").**
  - New groups carry `required_capabilities {extension_types [0xF2EE],
    proposal_types [], credential_types []}`. That is byte for byte what
    MDK 0.8 computes for a group with a member without SelfRemove.
  - A metadata Commit adds it to a group that lacks it.
  - Without it OpenMLS (check valn1001) refuses every GroupContextExtensions
    proposal that carries 0xF2EE, so MDK could not follow a rename.
  - MIP-01 also asks for `self_remove`. libmarmot does not implement
    SelfRemove (nostrc-2um6), so it cannot require it.
- **Commits as PrivateMessages (RFC 9420 section 6.3).**
  - MDK sends Commits encrypted (OpenMLS `MIXED_CIPHERTEXT`), which RFC 9420
    allows and MIP-03 does not restrict. libmarmot routed them to the
    application decryptor, and every MDK Commit failed (`MARMOT_ERR_MLS`).
  - Handshake PrivateMessages are now routed by their clear `content_type`.
    The sender is resolved from the sender data against the state that
    judges the Commit (current epoch, retained parent, contested removal,
    deferred replay).
  - The Commit is decrypted with the sender's handshake ratchet, left
    untouched, and verified with `wire_format mls_private_message`. There is
    no membership tag.
  - New internal `mls_group_handshake_sender()` and
    `mls_handshake_content_decode()`.
  - Standalone proposals, either wire format, are still
    `MARMOT_ERR_UNSUPPORTED`.
- **An Add's Welcome names the group relays (MIP-02).**
  `marmot_add_members()` put no `relays` tag on the kind:444 rumor, and MDK
  refuses such a Welcome (`validate_welcome_event`). The tag now comes from
  the group's marmot_group_data.
- **A Welcome refused for good leaves the pending list.** The accept path's
  final failures (an unproven Welcome-tree leaf, a missing KeyPackage key, an
  MLS failure) now save the Welcome as `MARMOT_WELCOME_STATE_FAILED`, as
  `marmot_decline_welcome()` saves `DECLINED`. Before, it stayed pending: an
  invitation that fails on every attempt. Storage failures still roll back
  and can be retried.

#### Compatibility

- **Wire.**
  - kind:445 is incompatible both ways with 0.10.0 and older.
  - marmot_group_data: 0.11.0 reads 0.10.0's layout, but 0.10.0 cannot read
    0.11.0's.
  - A 0.11.0 member's first metadata Commit in an old group adds
    required_capabilities.
  - Upgrade every member together, then rejoin or commit.
- **API/ABI.**
  - `MarmotGroupDataExtension` gains `extra` and `extra_len` at its end.
    Zero them, or start from `marmot_group_data_extension_new()`, which
    does.
  - No function signature changes.
- **State.** Unchanged. Stored groups keep their extension bytes until their
  next metadata Commit.
- **MDK 0.8.**
  - With `allow_unproven_members`, every tested flow works both ways
    (`tests/interop/mdk/README.md`).
  - By default, MDK 0.8 members are still refused (0.10.0's proof rule).
    MDK 0.8 reads 0.10.0+ KeyPackages, with the proof in the leaf's
    app_data_dictionary (nostrc-77pa: tested, below).

### 0.10.0 (unreleased): member leaves are bound to their accounts; the retained parent retires (nostrc-7vyi, nostrc-yuj2, security)

**Security fix, wire change and new API** (MINOR for 0.x).

#### Security advisory

Before 0.10.0 a member's credential was trusted exactly as it was added.
Only the inviter checked that a KeyPackage belongs to the account its
credential names (the kind:30443 event is signed by that account). Every
other member got the bare leaf in the Commit's Add, and a joiner got the
bare ratchet tree in the Welcome: nothing bound those leaves to their
accounts. A malicious or compromised admin (in a group without admins, any
member) could therefore:

- add a leaf naming another account, a non-member or a second "device" of a
  member, with an MLS key it holds, and post as that account. 0.9.0's author
  check then reported the forged author faithfully;
- send a Welcome for a group whose other "members" it made up.

Outsiders could not do this. Treat the membership of groups joined before
the upgrade, and the authors of messages from members added by someone you
do not trust, as unauthenticated.

#### What changed

- **The binding travels in the leaf** (Marmot `foundation/identity.md`,
  `app-components/account-identity-proof-v2.md`).
  - Every LeafNode libmarmot produces carries
    `marmot.member.account-identity-proof.v2`: the account's BIP-340
    signature over the leaf's MLS signature key, ciphersuite and signature
    scheme.
  - It sits in a LeafNode `app_data_dictionary` (0x0006) with
    `app_components` [0x0001, 0x8009], `safe_aad` [] and the 104-byte proof:
    the same entries as the adopted profile and MDK's cgka-engine.
  - Leaf capabilities list 0x0006, so the MDK 0.8 kind:30443 `mls_extensions`
    tag is now `0x0006 0x000a 0xf2ee`. MDK 0.8 requires `0x000a` and `0xf2ee`
    there and accepts other ids (MDK v0.8.0 `key_packages.rs`).
- **Where the proof comes from.**
  - `marmot_create_key_package()` signs it with the account key.
  - `marmot_create_key_package_for_profile()` signs it through
    `account_sign` (now also for the MDK 0.8 profile).
  - `marmot_create_key_package_unsigned()` uses the enrolled instance key.
    Without an enrollment it fails with `MARMOT_ERR_KEY_PACKAGE_IDENTITY`.
  - A member's Commit keeps its leaf's proof: the UpdatePath leaf keeps the
    signature key and credential the proof binds.
  - The creator's leaf of `marmot_create_group()` carries it: creating a
    group needs the instance to be enrolled for the creator, else
    `MARMOT_ERR_KEY_PACKAGE_IDENTITY` (legacy mode creates an unproven
    creator leaf).
  - An existing leaf gains one through `marmot_self_update()` with the
    template of `marmot_group_account_proof_template()` signed by the
    account: see "Existing groups" below.
- **Enrollment (new API)** for callers that sign through a signer:
  `marmot_account_proof_template()`, `marmot_set_account_proof()`,
  `marmot_has_account_proof()`.
  - The template is a local-only kind:450 event for this instance's MLS
    signature key. The account signs it once, and unsigned KeyPackages and
    created groups use that key and proof.
  - The instance key is generated per `Marmot` and not stored, so enroll after
    every `marmot_new()`. `marmot_create_key_package()` with the account key
    also enrolls.
  - **One leaf key per instance run (review W20 N3).** Every signer-only
    KeyPackage, and every group the instance creates in that run, uses the
    enrolled signature key; the spec allows reusing the proof for the same
    binding. RFC 9420 section 7.3 wants signature keys unique within a
    group, so a second such KeyPackage cannot join a group that already
    holds that key. `marmot_add_members()` refuses it and changes nothing,
    and receivers would reject it too. A leak of that one key affects all of
    these leaves. KeyPackages made with the account key or `account_sign` get
    a fresh key each, with their own proof.
  - marmot-gobject 1.4.0 wraps these calls. Gnostr enrolls through its
    signer before its first KeyPackage and before it creates a group or
    invites anyone, and says so while it waits.
- **Every member checks it.** A proof must name the leaf's credential
  identity, sign that leaf's own signature key (a proof replayed from
  another leaf fails) under ciphersuite 0x0001 and Ed25519, and verify
  under the account key.
  - **Commits** (`marmot_commit_authorize()`; the same policy runs on our own
    Commits before they are published). Each leaf a Commit adds, or whose
    slot now holds another account, must carry a valid proof. A member's
    replaced leaf (UpdatePath, Update) may not drop one it had. Otherwise the
    Commit fails with `MARMOT_ERR_KEY_PACKAGE_IDENTITY` and nothing is
    stored.
  - **Welcome joins** (`protocol-core/joining.md`, step 5). Every leaf of the
    tree must carry a valid proof, with two exceptions:
    - our own leaf;
    - the leaf that signed the GroupInfo, when it is the Welcome's sender.
      That is the rumor's `pubkey`, which the NIP-59 seal authenticates; the
      caller must have checked it.

    Otherwise the join fails with `MARMOT_ERR_KEY_PACKAGE_IDENTITY`, nothing
    of the group is stored, and the Welcome is recorded as failed.
  - **The inviter.** `marmot_create_group()` and `marmot_add_members()` refuse
    a KeyPackage without a valid proof, and they refuse an Add whose Welcome
    the joiners would reject. They check the whole resulting tree with the
    joiner's rule: every leaf but our own (the sender's) needs a valid proof.
    Otherwise the operation fails with `MARMOT_ERR_KEY_PACKAGE_IDENTITY` and
    changes nothing, instead of publishing an Add whose joiner can never
    join and whose leaf would stay in the tree as a ghost. KeyPackage
    validation rejects a proof that does not verify.
- **Welcome rumors carry their sender's pubkey**, as NIP-59 requires.
  Gnostr's unwrap already rejected rumors whose pubkey is not the seal's.
- **The sender, stated by the caller.** `marmot_process_welcome_from()` takes
  the NIP-59 seal's author, which the caller verified. The join's sender
  exemption then rests on that argument rather than on the rumor, and a
  rumor naming another author fails with `MARMOT_ERR_AUTHOR_MISMATCH`.
  `marmot_process_welcome()` still trusts the rumor's `pubkey`, so feed it
  only rumors whose seal was checked. Gnostr uses the new call.
- **Legacy mode.** `MarmotConfig.allow_unproven_members` (default `false`)
  accepts leaves that carry no proof: KeyPackages and members from MDK 0.8
  or libmarmot 0.9.0 and older. A proof that does not verify is rejected
  in either mode.

#### MDK 0.8 interoperability: a deliberate default (review W20 N2)

MDK 0.8 (and White Noise built on it) publishes KeyPackages without the
proof, so its leaves carry none. By default libmarmot 0.10.0 therefore:
- refuses to add an MDK 0.8 KeyPackage;
- rejects a Commit that adds such a leaf;
- rejects a Welcome whose tree holds an MDK 0.8 leaf other than the
  sender's. A two-member group from an MDK 0.8 user still joins, because the
  sender rule covers it.

This is the security trade-off of this release, not an accident. Without
the proof nobody but the inviter can tell whether such a leaf belongs to the
account it names, which is exactly the impersonation 0.10.0 closes. Gnostr
keeps the default, so it cannot invite MDK 0.8 users or join their larger
groups.

`MarmotConfig.allow_unproven_members` restores that interoperability per
instance, without the guarantee (a bad proof is still refused). MDK master
emits the proof in its adopted profile, which libmarmot's group engine does
not support yet (nostrc-qp24.5.1).

Tested in W23 (nostrc-77pa), against MDK v0.8.0 `575ae29d` with OpenMLS
`04c50d7`:
- MDK parses a libmarmot KeyPackage. It sees the leaf's 0x0006
  app_data_dictionary and capabilities 0x0006, 0x000a, 0xf2ee.
- It adds that KeyPackage to its groups, and the libmarmot member joins
  through MDK's Welcome.
- Rejoin, messages and Commits both ways needed the 0.11.0 wire fixes.

#### Why a proof in the leaf, and what stays separate

The kind:30443 signature binds a KeyPackage only for the inviter, who sees
the event. Every other member sees only what the MLS messages carry, so
Marmot binds the leaf itself (`foundation/identity.md`, "Account identity
proof"). This release is the minimal additive form of that proof for
libmarmot's legacy (0xF2EE) groups: the LeafNode component, produced on
every leaf and checked on every Add, replaced leaf and Welcome tree, and
persisted and cloned with the tree.

The adopted profile's group-level rules stay with nostrc-qp24.5.1 (W2a),
and with them the rest of nostrc-qp24.5.1.1. The group engine refuses a
GroupContext `app_data_dictionary`, so it cannot require 0x8009 in the
GroupContext's `app_components`, run AppDataUpdate, or admit adopted-profile
groups. There is no adopted peer to test against either.

#### Compatibility

- **Wire.** KeyPackages gain the leaf dictionary and the `0x0006` id; the
  Welcome rumor gains a `pubkey`. Readers that ignore unknown leaf
  extensions listed in the capabilities (libmarmot 0.9.0 and older) are
  unaffected. Whether an OpenMLS-based peer accepts the leaf dictionary is
  untested.
- **Unproven KeyPackages are refused.** A 0.10.0 member refuses to add a
  KeyPackage from MDK 0.8 or libmarmot 0.9.0 and older. It also refuses a
  Commit that adds such a leaf, and a Welcome whose tree holds one (other
  than the sender's), unless `allow_unproven_members` is set. Upgrade every
  member and publish new KeyPackages.
- **Existing groups.** Leaves from 0.9.0 and older keep their place (a
  Commit re-checks only the leaves it changes), and those members can still
  commit and talk.
  - **Admitting members.** A joiner accepts an unproven leaf only in legacy
    mode or as the Welcome's own sender. A group with an unproven leaf other
    than the inviter's therefore cannot admit anyone in the default mode: the
    inviter refuses the Add (`MARMOT_ERR_KEY_PACKAGE_IDENTITY`).
  - **Migration.** Every member with an unproven leaf signs
    `marmot_group_account_proof_template()` (its account over its own group
    leaf key) and commits `marmot_self_update()` with it. Receivers accept
    the leaf going from no proof to a valid one. Once every leaf is proven,
    any admin can admit members again.
  - **During the transition,** or with members who cannot upgrade (MDK 0.8),
    `MarmotConfig.allow_unproven_members` on every member keeps the group
    growing, without the identity guarantee.
- **API/ABI.**
  - `MarmotConfig` gains a field: rebuild, and start from
    `marmot_config_default()`.
  - New `marmot_account_proof_template()`, `marmot_set_account_proof()`,
    `marmot_has_account_proof()`, and `marmot_group_account_proof_template()`
    with `marmot_self_update()` (also the public self-update of nostrc-yd0q),
    `marmot_process_welcome_from()`, and
    `marmot_key_package_event_has_account_proof()` (an inviter tells a
    KeyPackage it would refuse, before any Commit).
  - `marmot_create_group()` fails without an enrollment, outside legacy mode.
  - `marmot_create_key_package_unsigned()` fails without an enrollment, as
    above.
  - Internal: `marmot_commit_authorize()` takes the legacy flag; new
    `mls_group_create_with_leaf_extensions()`,
    `mls_welcome_process_parsed_signer()` and `marmot_leaf_proof_status()`.
- **State.** Unchanged by the binding: the proof lives in the leaf and is
  stored with the tree. (The retained-parent record changes; see below.)

#### A removed member learns it was removed (nostrc-xrya)

A member an admin removes cannot enter the next epoch: the removing
Commit's UpdatePath is encrypted to the remaining members only. Before, its
Commit failed with `MARMOT_ERR_MLS_PROCESS_MESSAGE`, the group stayed
active, the member could still send (nobody could read it), and every later
kind:445 failed as undecryptable.

- **Recognised.** When a Commit fails to apply, `marmot_process_message()`
  checks it as far as a removed member can, as OpenMLS does for
  `self_removed`: the PublicMessage framing for the group and epoch, the
  committer's signature and the membership tag (the same checks as full
  processing, now one internal helper), a well-formed proposal list with an
  UpdatePath, and an inline Remove of our own leaf. The committer must be an
  admin of the current GroupData, as for any Commit that changes membership.
  A removal forged by a non-admin fails with
  `MARMOT_ERR_COMMIT_FROM_NON_ADMIN` and changes nothing.
- **Judged by the Commit ordering, not by arrival** (W22 review B1). A
  pending Commit of ours that sorts first still wins (the removal is
  deferred), and a removal competing with a Commit we already applied must
  beat it. Deferred Commits are replayed winner first once our pending one is
  cleared.
- **Ended, with who and how.** The group turns inactive, as after
  `marmot_leave_group()`, any pending Commit of ours is dropped, and the
  result is `MARMOT_RESULT_COMMIT` with the inactive group.
  `marmot_get_group_removal()` says who removed us, from which epoch, and
  whether it is final. It is kept in `mls_kv` under `mls_group_removed`
  (version 3: epoch, flags, the removal's committer and digest, later-epoch
  event ids) in the same
  transaction as the inactive state; a later Welcome into the group clears
  it; a record that does not parse is `MARMOT_ERR_DESERIALIZATION`, never
  "not removed".
- **Not final while it could still lose.** Another admin whose key sorts
  below the remover's could publish a Commit of that epoch that wins. Until
  none can, the inactive group still judges that epoch's Commits (everything
  else is `MARMOT_ERR_USE_AFTER_EVICTION`): one that keeps our leaf and beats
  the removal re-activates the group in its epoch and forgets the removal; a
  removal that beats it replaces it. The same holds, against the Commit that
  led there, for Commits of the parent epoch while the retained parent is
  kept in full.
- **Final once the group moved on** (review B2). Finality judged only at
  eviction would never come in a common group: the removed member cannot
  read later epochs, so neither the retained parent retires nor does a
  lower-keyed admin stop counting. So a removal is also final after
  `MARMOT_REMOVAL_FINAL_AFTER` (5) distinct kind:445 events of the group that
  none of our exporter secrets opens. A Commit that could still beat the
  removal is of the removal's epoch (or its parent's) and opens, so it is
  never counted itself; their ids are kept in the removal record (version 3)
  so a copy from another relay counts once.
  **Residual risk** (W22 review B3): a winning Commit that reaches us only
  after `MARMOT_REMOVAL_FINAL_AFTER` unopenable events -- which may be the
  winner's own later messages, all of one epoch, or junk anyone can post
  with the group's `h` -- is refused: we then stay ended while the group,
  which is on the winner, keeps our leaf (as in N2 below). Only the removed
  member refuses it, so this is a divergence, not a case the one-epoch
  horizon covers for everyone. A winner behind 4 of its branch's messages
  still re-activates us; behind 5 it does not (tested). Groundhog narrows
  the window by applying each relay's stored answer oldest first, so a
  winner a relay holds is applied before its branch's later messages.
  Tracked as nostrc-6njv.
- **Final: the keys go** (review N1). A removal nobody can beat deletes the
  removed epoch's MLS state, the retained parent and the group's exporter
  secrets, so a stolen store no longer opens them. The group record stays.
  (`marmot_leave_group()` still keeps them; the member may be re-added.)
- **Limits** (review N2). The removed member cannot check what needs the new
  epoch -- the confirmation tag, the UpdatePath, the post-Commit policy --
  so an admin can end the group for us alone with a removal the others
  reject (our leaf then stays in their tree until an admin removes it).
  An admin can remove us anyway. Late application messages of the last
  epoch we were in are not read after the removal.

#### The retained parent retires once no competing Commit can win (nostrc-yuj2, security)

**What was exposed.** Before 0.10.0, after every Commit the store kept the
previous epoch's full state as the retained parent. Together with the
relay-visible Commit it derives the current epoch again, so whoever obtained
the store could decrypt every message of the current epoch, including
messages already read, until the next Commit (review B1; see the 0.8.0
advisory). 0.10.0 keeps that state only while it can still matter.

- **The parent's two jobs.** Marmot `protocol-core/retained-history.md`
  ("Retained cryptographic material") separates them.
  - Judging and applying a competing Commit from the parent needs its init
    secret, membership key, and the private keys that open an UpdatePath.
    That is exactly what derives the current epoch again.
  - Reading the parent epoch's late application messages needs only its
    sender-data secret and secret tree (the unconsumed ratchets). Neither
    derives anything of the current epoch.
- **Policy.** The full parent is kept while some member could still publish
  a Commit from the parent that beats the applied one (`CommitOrderingSuffix`).
  - **Who could win.** For a privileged Commit: an admin of the parent's
    GroupData whose account key does not sort above the committer's. For an
    ordinary Commit: any admin, or any member whose key does not sort above
    the committer's. The committer and we do not count.
  - **When a member stops counting.** Once one of its application messages
    decrypts and authenticates at the new epoch, on our branch. That member
    applied the Commit, so any Commit it had made from the parent lost to it.
  - **Retirement.** When nobody is left, the parent keeps only its
    late-message part, written in the same storage transaction as that
    message. This happens at once when nobody could win, for example in a
    two-member group, or when the committer is the only admin whose key sorts
    that low.
  - **At the latest,** the next Commit replaces the parent, as before.
- **After retirement.**
  - The parent no longer holds the init, membership, confirmation, exporter,
    external, resumption and authenticator secrets, the own leaf and path
    private keys, or the PSK cache (`mls_group_strip_to_reader()`).
  - The stored state and the public Commit therefore no longer derive the
    current epoch.
  - Late messages of the parent epoch still decrypt, once each.
  - The applied Commit, delivered again, is still `MARMOT_RESULT_OWN_MESSAGE`.
  - A different Commit for the parent epoch fails with
    `MARMOT_ERR_WRONG_EPOCH`. Only a member already seen at the new epoch
    could have sent it, or one that cannot win. An honest member's rival
    Commit lost to the applied one on its own client, so honest members agree
    on the outcome.
- **What a stolen store still exposes.**
  - While the full parent is kept: every message of the current epoch, as
    before.
  - At all times: the unconsumed keys of the current epoch (the live state)
    and of the parent epoch (the late-message part), until the next Commit.
  - After retirement: no consumed key of the current epoch.
  - The exporter secrets of earlier epochs stay in the exporter-secret table,
    as before. They open old kind:445 envelopes (Commits, and ciphertexts
    whose keys are gone), not message keys.
- **Alternatives considered.**
  - *Wall-clock bound: rejected.* Marmot convergence never depends on local
    time (`protocol-core/convergence.md`: deferred Commits expire by epoch).
    Members that retired at different moments would disagree about a late
    winning competitor, and the group would split.
  - *Message count: rejected.* A count of messages says nothing about who
    applied the Commit.
  - *Acknowledgment of our own message: not possible.* libmarmot never learns
    of relay acknowledgments.
- **Limitations.**
  - A group in which a member who could still win stays silent keeps the
    full parent until that member speaks or the next Commit: the 0.9.0
    exposure, no worse.
  - A malicious member already seen at the new epoch can still publish a
    competing Commit. It then wins where the parent is still full and loses
    where it has retired. Malicious members could already split groups
    (nostrc-w1m0).
- **State format.** The `mls_group_parent` record keeps version 1 and its
  layout, and gains a trailer: the tier and the parent leaves still pending.
  - A record without the trailer (0.9.0 and older) loads with the full parent
    and every member that could win pending. The committer is pending too,
    because the old record does not name its leaf.
  - libmarmot 0.9.0 cannot read a record with the trailer. After a downgrade,
    that epoch's late messages fail and its competitors are refused until the
    next Commit.
- **API/ABI.** No public change. Internal: `mls_group_strip_to_reader()`,
  `marmot_commit_note_witness()`, `MarmotCommitKey.committer_leaf`.

### 0.9.0 (unreleased): application messages are signed and bound to their author (nostrc-we6g, security)

**Security fix and wire-format change** (MINOR for 0.x: 0.9.0 and 0.8.0 or
older cannot read each other's application messages).

#### Security advisory

Before 0.9.0 **any member could post as any other member**. The inner
(rumor) event's `pubkey` was never checked against the MLS sender, and
`marmot_process_message()` reported and stored whatever author the inner
event named. Application PrivateMessages also carried no MLS signature
(RFC 9420 section 6.3.1). The secret-tree keys are shared by the whole
group, so the MLS layer did not tell members apart either: a member could
even encrypt under another member's leaf. Treat the author of every message
received before the upgrade as unauthenticated among members. Outsiders
were never able to post: that needs the epoch's secrets.

#### What changed

- **Signed content (RFC 9420 section 6.3.1).** An application message's
  PrivateMessage now encrypts a `PrivateMessageContent`:
  `application_data<V>`, then `FramedContentAuthData` (the signature
  `SignWithLabel(leaf key, "FramedContentTBS", FramedContentTBS)` over the
  FramedContent and the GroupContext), then zero padding (none is sent).
  The receiver verifies the signature with the sender leaf's signature key
  before anything is delivered; a bad signature fails with
  `MARMOT_ERR_MLS`, and the ratchet is put back.
- **Sender-data AAD (RFC 9420 section 6.3.2).** The sender data is sealed
  with `SenderDataAAD` (group id, epoch, content type). Before 0.9.0 it
  used an empty AAD, which OpenMLS and MDK reject. The RFC 9420
  `message-protection` vector now decrypts end to end in `tests/interop`,
  its signature verifies, and the content libmarmot signs is byte for byte
  the vector's.
- **Author binding (Marmot `foundation/application-messages.md`, legacy
  MIP-03).**
  - The receiver requires the inner event's `pubkey` to be the account
    identity (32-byte credential identity) of the MLS sender leaf, in the
    state that decrypted it: live, or the retained parent for a late
    message. Otherwise it fails with `MARMOT_ERR_AUTHOR_MISMATCH` (also for
    a missing pubkey, or inner JSON that is no event).
  - Nothing is stored, and the ratchet step is not kept.
  - `result->app_msg.sender_pubkey_hex` is now the authenticated author.
  - `marmot_create_message()` fills a missing inner `pubkey` with our
    account (recomputing a declared id), and refuses another account's with
    `MARMOT_ERR_AUTHOR_MISMATCH`.
- **Duplicates.** An inner event already delivered (the same NIP-01 id) in
  another envelope is `MARMOT_RESULT_OWN_MESSAGE` and is not stored again.
  Processed markers are now also kept under the inner event's id. Sending
  the same inner event twice (same author, `created_at`, kind, tags and
  content) delivers it once.
- **Handshake PrivateMessages.** Marmot sends Commits and proposals as
  PublicMessages. A PrivateMessage with a handshake content type is now
  refused (`MARMOT_ERR_UNSUPPORTED`) instead of being handed over as an
  application message.
- **Nested transactions (review N1).** When the storage's transactions are
  savepoints of an application transaction (Groundhog's GhStoreMarmot), a
  send's ratchet step is durable only when that outer transaction commits.
  Commit it before publishing; see `marmot_create_message()` and the hook
  contract in `marmot-storage.h`.

#### Compatibility

- **Wire.** 0.9.0 rejects application messages from 0.8.0 and older
  senders (unsigned, empty sender-data AAD), and 0.8.0 or older cannot read
  0.9.0's. Upgrade every member of a group together. Commits, Welcomes and
  KeyPackages are unchanged.
- **Apps.** Callers that already put the account's own pubkey in the inner
  event (Gnostr) are unaffected. Callers that leave it out get it filled in.
  A caller that sends inner events authored by another key now gets
  `MARMOT_ERR_AUTHOR_MISMATCH`.
- **State.** Unchanged (format 3).
- **API/ABI.** No change to existing functions. Additive:
  `marmot_get_group_members()` lists the account keys of the group's current
  members (the stored epoch's leaf credentials), for Groundhog's member list
  (nostrc-qp24.13).
  - Internal changes: `mls_sender_data_encrypt`/`_decrypt` take an
    `MlsSenderDataAAD`; new `mls_application_content_encode`/`_decode` and
    `marmot_mls_sender_identity`.
  - `mls_group_decrypt` returns application data only.

### 0.8.0 (unreleased): the MLS sender ratchets are stored (nostrc-ai04, security)

**Security fix and state-format change** (MINOR for 0.x: a 0.8.0 state
cannot be read by 0.7.0).

#### Security advisory

libmarmot 0.7.0 and earlier stored the MLS group state without the secret
tree's sender ratchets. Every load re-derived them from the epoch's
`encryption_secret` at generation 0, and `marmot_create_message()` loads
the state on every call. As a result:

- **All application messages a member sent in one epoch shared one
  AES-128-GCM key.** Their nonces differed only in the 4-byte random reuse
  guard. If two reuse guards collide (likely after about 2^16 messages from
  one sender in one epoch), the key and nonce repeat. Anyone who can open the
  kind:445 NIP-44 layer (every member, and anyone holding that epoch's
  exporter secret) then learns the XOR of the two plaintexts and can forge
  the AEAD layer under that key. Nothing else authenticated the sender:
  before 0.9.0 application messages carried no MLS signature, and receivers
  took the author from the inner event without checking it against the
  sender (nostrc-we6g, fixed in 0.9.0). Such a forgery, like any message
  from a member, could claim any member as its author.
- **There was no forward secrecy within an epoch.** The stored state kept the
  `encryption_secret` and the `joiner_secret` (from which it follows with
  the stored GroupContext). Whoever obtains a copy of the stored state can
  therefore decrypt every message of that epoch, including messages already
  read, and also of the retained parent epoch. This applies to backups and
  snapshots too.
- **Replays were accepted.** Anyone could re-sign a kind:445 from a relay
  with a new ephemeral key (a new event id escapes the processed marker).
  Members then accepted and stored it again, in the live epoch and through
  the retained parent.

**Recommended action.** Upgrade every member. Then move each group to a new
epoch with any Commit: a self-update once available (nostrc-yd0q), or a
metadata update or member change. The upgrade alone stops key reuse for new
messages and drops the consumed secrets from the stored state at its next
save. But a copy of the state taken before the upgrade (a backup, a
snapshot, a retained parent) still holds that epoch's `encryption_secret`,
which derives every key of the epoch, including keys used after the
upgrade. Only a new epoch ends that. Messages sent before the upgrade may
have shared a key: treat their confidentiality against members and holders
of the exporter secret as weakened.

**What 0.8.0 and 0.9.0 do not protect (review B1).** After every Commit,
libmarmot keeps the previous epoch's full state as the retained parent
(`mls_group_parent`), to judge a competing Commit and read late messages.
That state holds the parent's init secret and the private keys that open
the Commit's UpdatePath, and relays carry the Commit. Whoever obtains the
whole store can therefore process the Commit again, derive the current
epoch from scratch, and decrypt every message of the current epoch,
including messages already read, plus the parent epoch's unconsumed ones.
In 0.8.0 and 0.9.0 this lasts until the next epoch transition replaces the
parent. Forward secrecy against a stolen store holds for messages of
earlier epochs and for the parent epoch's consumed messages (the parent
holds only unconsumed values), not for the current epoch. Since 0.10.0 the
exposure ends as soon as no competing Commit can win, when the parent is
reduced to what reads late messages (nostrc-yuj2, see 0.10.0).

#### What changed

- **Sender.** Each `marmot_create_message()` uses the next generation of
  our sender ratchet. The advanced ratchet is stored in the operation's
  storage transaction before the event is returned. On any error, including
  a failed commit, no event is returned, so no event can go out under a
  generation that is not stored. A crash after the commit only leaves an
  unused generation, which receivers skip. A chain never wraps: it ends
  after generation 2^32 - 2.
- **Receiver.** A generation decrypts once. A message re-published in a new
  envelope fails with `MARMOT_ERR_MLS`, in the live epoch and through the
  retained parent. Out-of-order delivery (RFC 9420 section 15.3) works as
  follows:
  - a message may name a generation up to the group's
    `max_forward_distance` (1000) past the newest one read from that
    sender;
  - keys of skipped generations are kept only for the 32 generations below
    that newest one (`MLS_SECRET_TREE_MAX_SKIPPED_MESSAGE_KEYS`), and only
    until used;
  - anything outside that window fails closed.

  A message that fails to decrypt consumes nothing: the ratchet is put back.
  On a storage without transaction hooks, a message whose later writes fail
  (message row, processed marker, group record) does not keep its ratchet
  step. The previous state record is written back, so the event can be
  processed again rather than being lost with a consumed key. With the hooks,
  the rollback does the same.
- **Deletion (RFC 9420 section 9.2).** The live group state (`mls_group`)
  holds only unconsumed values of its epoch:
  - per sender, its unused leaf secret, or the two ratchet heads and the
    skipped keys;
  - not the `encryption_secret`, `joiner_secret` or `welcome_secret`, which
    are consumed once the epoch starts.

  In memory, the `encryption_secret` is wiped as soon as the secret tree is
  built. A joiner also wipes its joiner and welcome secrets. A committer
  keeps them in memory only for the Welcome it builds in that operation, and
  never stores them. Tests check that no used key, ratchet secret, leaf
  secret or epoch root appears in the stored bytes. That covers the live
  state only: through the retained parent and the public Commit the epoch
  can be derived again while that parent is kept in full (see "What 0.8.0
  and 0.9.0 do not protect"; since 0.10.0 only until it retires).

#### State format and migration

- The MLS group state (`mls_kv` labels `mls_group`, `mls_group_parent`,
  `mls_group_pending`) is written as format 3:
  - eight epoch secrets instead of eleven;
  - the PSK and path-key caches always present;
  - then the secret tree.

  0.7.0 cannot read it; downgrading loses the group.
- Formats 1 and 2 are still read, and the next save writes format 3.
  - **Own sender.** Both chains are moved
    `MLS_SECRET_TREE_LEGACY_OWN_STRIDE` (512) generations forward, deleting
    every key passed. Through the public API those formats never sent above
    generation 0, so 512 is far past anything they used. It is also within
    the 1000-generation forward distance that libmarmot, OpenMLS and MDK
    receivers accept, so no one has to wait for a new epoch.
  - **Why not force a new epoch?** Forcing a self-update before the next
    send would block sending until a Commit is published, and there is no
    public self-update yet.
  - **Receivers.** A migrated receiver cannot know which generations it
    already read. Every other sender's chain restarts at 0, so a message
    from before the upgrade is accepted once more: at most
    `max_forward_distance` generations per sender, in the migrated epoch
    only. The processed-event markers still catch the same event id.
    Since 0.9.0 such old messages are rejected anyway (unsigned), and an
    inner event already delivered is a duplicate by its id.
  - **Senders on 0.7.0 or older.** They send every message of an epoch at
    generation 0. A 0.8.0 receiver reads the first one and drops each later
    one as a replay of that generation. A 0.9.0 receiver rejects them all:
    they are unsigned.
- **Mixed versions.** A 0.7.0 receiver only reads generations 0-32 of an
  epoch, and a 0.8.0 sender that migrated starts at 512. Upgrade every
  member.
- **API/ABI.** No public API change. Internal:
  - `marmot_group_event_decrypt()` (test helper);
  - `mls_secret_tree_serialize`, `_deserialize`, `_skip` and the
    sender-snapshot calls.

### 0.7.0 (unreleased): one storage transaction per operation (nostrc-qp24.7)

**Additive API and storage-interface change** (MINOR). Groundhog's durable
MLS Commit lifecycle needs every multi-record update to be atomic.

- **Transaction hooks.** `MarmotStorage` gains three optional function
  pointers at its end: `begin`, `commit` and `rollback`. Set all three or
  none; `marmot_new()` refuses a storage with only some of them (it returns
  `NULL` and does not take the storage). With the hooks, every public
  operation that writes runs as exactly one transaction. That covers:
  - an epoch transition (exporter secret, retained parent, MLS state and
    group record);
  - a pending Commit, its merge and the Welcome outbox append;
  - a received message with its ratchet step, message row, processed marker
    and group record;
  - a Welcome join, KeyPackage creation, Welcome sent-marks and a reconcile.

  A crash can no longer leave a partial epoch. libmarmot never nests the
  calls. A backend may run them as savepoints of an application
  transaction; Groundhog's GhStoreMarmot does.
- **Outcome of an error.** An operation that fails rolls back. Some errors
  are deliberate outcomes, and their writes are committed:
  - a superseded pending Commit dropped by the merge (`WRONG_EPOCH`);
  - a losing inbound Commit deferred (`OWN_COMMIT_PENDING`);
  - a duplicate Welcome retired (`WELCOME_ALREADY_ACCEPTED`);
  - a Commit that can no longer be authorized, dropped;
  - a Welcome recorded as failed before anything else was written.

  A Welcome join that fails after it started writing is now rolled back
  (with transactions), so it can be accepted again; before, it was recorded
  as failed.
- **ABI.** The struct grew. A storage implementation built against an
  older header must be rebuilt, and must zero-initialize the struct (e.g.
  `calloc`). The built-in memory, SQLite and nostrdb backends have no hooks
  yet (nostrc-wf71) and keep the compensating writes of 0.5.0; they can still leave a
  partial state on a crash, which `marmot_group_reconcile()` repairs as
  before.
- **Late messages.** An application message of the previous epoch that
  arrives after the next Commit was applied is now read with the retained
  parent state. Its ratchet step is stored in the same transaction. Before,
  it failed with `MARMOT_ERR_MLS`. Two epochs back still fails: one epoch is
  libmarmot's rewind horizon.
- **Fail closed.** A received application message whose MLS ratchet step
  cannot be stored is not delivered (`MARMOT_ERR_STORAGE`). Before, the
  failure was ignored.
- **Secrets.** Serialized MLS states and KeyPackage private keys read from
  storage are wiped before they are freed.
- **Known gap (nostrc-ai04, fixed in 0.8.0).** The serialized MLS state
  does not hold the per-sender ratchet: it is re-derived from the epoch's
  encryption secret on every load. As a result, a message re-wrapped in a
  new envelope is accepted again, and a sender's messages in one epoch
  reuse a generation. See the 0.8.0 security advisory.

### 0.6.0 (unreleased): kind:445 envelopes are authenticated (nostrc-6r6s)

**Behaviour change and new API** (MINOR for 0.x). `transports/nostr.md`
(marmot `26fa6a6`) says receivers MUST verify a kind:445's event id and
signature before decrypting it. libmarmot now does.

- **`marmot_process_message()` is the relay path.** Before any storage
  access or decryption it checks the event:
  - the id must be the canonical NIP-01 hash, else `MARMOT_ERR_EVENT`;
  - the Schnorr signature by the event's pubkey must verify, else
    `MARMOT_ERR_SIGNATURE`. A missing or malformed signature counts as
    invalid.

  A rejected event changes nothing: no MLS generation is consumed, no
  Commit is applied or deferred, and nothing is marked processed. The
  canonical id then keys the processed markers.
- **`marmot_process_rumor_message()` (new) is the gift-wrap path.** It
  takes a kind:445 *rumor* from a NIP-59 gift wrap that the caller unwrapped
  and whose seal signature it verified. Rumors are unsigned by design, and
  the seal authenticates them.
  - A declared id must still be canonical (`MARMOT_ERR_EVENT`); a missing
    one is computed.
  - Never pass events that came from a relay directly: they belong on the
    signed path.
- **`marmot_save_created_message()`** verifies the signed event it
  persists in the same way.
- **What the signature proves.** It is made by a fresh ephemeral key, so it
  authenticates only the envelope. Sender authenticity still comes from the
  exporter-keyed NIP-44 layer and MLS.
- **Migration.**
  - Callers no longer need to verify kind:445 events themselves.
  - Callers that fed hand-built or unsigned events to
    `marmot_process_message()` must sign them, or use the rumor path when
    the event came out of a gift wrap.
  - marmot-gobject 1.3.0 adds
    `marmot_gobject_client_process_rumor_message_async/_finish`. Gnostr's
    gift-wrap route uses it; its relay route, fed by nostrdb, keeps
    `process_message`.

### 0.5.0 (unreleased): Commits are published and processed (nostrc-9ata)

**Breaking API and behaviour change** (MINOR for 0.x). Members now apply each
other's Commits. A Commit is applied only once a relay has accepted it. Every
kind:445 event comes back signed.

#### Migration

- **Metadata updates return the Commit.** `marmot_update_group_metadata()`
  gains a required `char **out_commit_json` (`NULL` is
  `MARMOT_ERR_INVALID_ARG`). Before 0.5.0 the metadata Commit was discarded,
  so only the committer moved to the new epoch.
- **Commits are pending until a relay accepts them** (legacy MIP-03: do not
  apply a Commit before a relay confirms it). `marmot_add_members()`,
  `marmot_remove_members()` and `marmot_update_group_metadata()` no longer
  change the group; they store a *pending* Commit and return its event.
  - Publish the event to the group relays.
  - When one relay answers NIP-01 `OK`, call `marmot_merge_pending_commit()`.
    Send an Add's Welcomes only after that.
  - When no relay accepts it, call `marmot_clear_pending_commit()` (new).
  - Until then, the next Commit is `MARMOT_ERR_OWN_COMMIT_PENDING`.
  - When a relay's answer is lost (timeout, disconnect), do not clear: the
    relay may have stored the Commit. Keep it pending and retry. When our own
    pending Commit comes back from a relay, `marmot_process_message()` merges
    it and returns `MARMOT_RESULT_COMMIT`, since a stored copy proves it was
    published.
  - A pending Commit is bound to the exact state it was built on (epoch and
    confirmed transcript hash). If a competing member's Commit replaces that
    state, even with another Commit of the same epoch, the pending one no
    longer defers anything, `marmot_get_pending_commit()` reports it
    superseded, and the merge returns `MARMOT_ERR_WRONG_EPOCH`: ours is
    discarded and the group follows the winner. Do not send that Add's
    Welcomes.
  - Merging is idempotent: a Commit already applied (by its echo, or before
    a crash) merges as `MARMOT_OK`. A Commit that can no longer pass
    authorization is discarded; a storage error leaves it pending, so merge
    again or clear it.
- **Restart path.** `marmot_get_pending_commit()` returns the pending Commit's
  signed event after a crash or a lost answer. Republish it and merge on the
  first `OK`.
- **Welcome outbox.** When a pending Add merges (by call, echo or after a
  restart), its Welcomes and their recipients are *appended* to
  `marmot_get_unsent_welcomes()`. Earlier Adds' Welcomes stay until sent.
  - Each entry has a stable `id` (SHA-256 of recipient ‖ rumor).
  - Gift-wrap and send each entry. Once that send is confirmed, pass its id
    to `marmot_mark_welcomes_sent(m, gid, ids, count)`, which removes only
    those entries.
  - Entries appended after your read stay; a failed send stays for a retry.
  - The rumors `marmot_add_members()` returns are the same Welcomes; send one
    copy only.
- **Duplicate Welcomes.** Resent Welcomes can arrive twice. A member that
  already joined through a Welcome refuses a second copy:
  `marmot_accept_welcome()` returns `MARMOT_ERR_WELCOME_ALREADY_ACCEPTED`,
  keeps its current (possibly newer) state, and retires the copy. A
  re-invite after a removal still joins, because its Welcome is for a later
  epoch.
  - `marmot_create_group()` still applies immediately: only its joiners see
    its Commit.
- **Every kind:445 is signed** with a fresh ephemeral key (MIP-03), never the
  account key and never reused. This covers `marmot_create_message()` and all
  Commit events. Callers that signed the event themselves must stop; it can be
  published as is.
- **Many members, one Commit** (nostrc-wc6v). `marmot_create_group()`,
  `marmot_add_members()` and `marmot_remove_members()` put all their Adds or
  Removes in one Commit with one Welcome. Each joiner's `EncryptedGroupSecrets`
  carries its own path secret. Before, each entry was a separate Commit and
  only the last was published, so every other member and every earlier
  invitee was left behind. Removing the same member twice is
  `MARMOT_ERR_INVALID_ARG`.

#### Wire changes

- **Commit events.** The kind:445 content is the Commit `MLSMessage`,
  NIP-44-encrypted with the exporter secret of the epoch the Commit was made
  in, like application messages. The only tag is `h`.
- **What changed.** Before, the content was the plaintext base64 MLSMessage
  with an `encoding` tag, which exposed the group's MLS state on relays. No
  libmarmot release processed those events, so none are accepted now.

#### Commit ingestion

`marmot_process_message()` applies a received Commit through
`mls_group_process_commit()` on a private copy of the stored state. It then
checks the result against MIP-01 before storing anything:
- A Commit that adds or removes members, or changes the GroupContext
  extensions, is privileged. Its committer must be an admin of the pre-Commit
  GroupData (`MARMOT_ERR_COMMIT_FROM_NON_ADMIN`). This is the producers' own
  rule, including legacy groups without admins.
- The GroupData must stay present and well formed, and keep its
  `nostr_group_id` (`MARMOT_ERR_PROTOCOL_GROUP_MISMATCH`).
- The committer keeps its account (`MARMOT_ERR_IDENTITY_CHANGE`).

On success the new MLS state, its exporter secret and the group record are
stored, and the result is `MARMOT_RESULT_COMMIT` with `commit.updated_group`.

#### Epoch rules

| The Commit is for… | Result |
|---|---|
| the current epoch, nothing pending | applied |
| the current epoch, our own Commit pending | competes with ours now (see ordering below); a loser is kept and `MARMOT_ERR_OWN_COMMIT_PENDING` returned, and `marmot_clear_pending_commit()` processes it again |
| the previous epoch, same bytes as the one applied | `MARMOT_RESULT_OWN_MESSAGE` |
| the previous epoch, a different Commit | replaces the applied one only if it wins the ordering, else `MARMOT_ERR_WRONG_EPOCH` (since 0.10.0 also `MARMOT_ERR_WRONG_EPOCH` once the retained parent has retired, nostrc-yuj2) |
| any older epoch | `MARMOT_ERR_WRONG_EPOCH` |
| a future epoch | cannot be decrypted yet (`MARMOT_ERR_NIP44`); retry after the missing Commits |

Every rejection leaves storage untouched.

**How competing Commits are ordered.** The applied Commit's parent state and
ordering key are retained under a new `mls_kv` label, `mls_group_parent`. The
ordering is the adopted spec's `CommitOrderingSuffix`
(`protocol-core/convergence.md`, "Same-epoch races"):
1. privileged before ordinary;
2. then the lower committer account key (x-only, bytewise);
3. then the lower SHA-256 of the Commit `MLSMessage`.

Transport timestamps and event ids never count.

#### Known limitations: groups can split permanently (nostrc-w1m0, P1)

libmarmot implements only the last three steps of the spec's branch selection
(the ordering above), for one-Commit branches from one retained parent. It
does not implement branch depth, witness scoring, bounded collection passes,
a rewind horizon beyond one epoch, or invalidation. **Concurrent Commits can
therefore split a group permanently, including isolating an admin.** There is
no in-band repair: the stranded members must be removed and re-added.

- **Depth-2 race.**
  1. Alice (an admin) renames while Bob self-updates.
  2. Charlie applies Bob's Commit first and commits on top of it.
  3. Alice's privileged Commit then wins at depth 1 for Alice, but Bob and
     Charlie are on a depth-2 branch. The spec would select that branch;
     libmarmot never switches Alice to it. Alice cannot decrypt Charlie's
     Commit (`MARMOT_ERR_NIP44`), and Bob and Charlie reject Alice's
     (`MARMOT_ERR_WRONG_EPOCH`).
- **Relay stored a Commit we cleared.** A relay stores a Commit whose `OK`
  never reached the committer, so the committer clears it. Members that
  received it apply it; the committer does not.
- **Joiners on a losing branch.** A joiner admitted by a Commit that later
  loses the ordering stays on the losing branch.
- **Messages across a branch switch.**
  - Application messages decrypted on a losing branch stay delivered; the
    spec requires withdrawing them.
  - Messages sent on the winning branch that arrived before the switch failed
    to decrypt and are not retained. They are lost unless a relay sends them
    again.

**Mixed-implementation groups.** The legacy MIP-03 that this wire profile
(0xF2EE group data, NIP-44 envelopes) follows orders competing Commits by
earliest outer `created_at`, then the smallest event id. libmarmot uses the
adopted spec's `CommitOrderingSuffix` instead, because the adopted spec
forbids transport metadata in branch selection. In a group that also has a
member following the legacy rule (for example MDK), concurrent Commits can
select different winners on different members, and the group splits as
described above.

#### Robustness

- **Thread safety.** A `Marmot` instance and its storage are not
  thread-safe; serialize every call on one instance. Commit processing,
  merges and message handling are multi-step read-modify-write transitions.
  marmot-gobject does this per client (every async and sync call holds the
  client's lock; direct libmarmot calls through
  `marmot_gobject_client_get_marmot()` must hold
  `marmot_gobject_client_lock()`). Its signals are emitted only in the
  thread-default main context of the thread that created the client, from
  an idle source, never on a worker thread or under the lock. Handlers may
  therefore call back into the client; emissions wait until that context
  runs.

- **Rollback of failed writes.** An epoch transition writes, in order, the
  exporter secret, the retained parent, the MLS state and the group record. A
  failed write restores the earlier ones and returns its own error. A failed
  restore returns `MARMOT_ERR_STORAGE`, because storage may then be
  inconsistent. A read error on a record about to be replaced aborts before
  anything is written; only `MARMOT_ERR_STORAGE_NOT_FOUND` counts as "absent".
- **Crash recovery.** A crash between the MLS state and the group record
  leaves the record an epoch behind. The next message, media or Commit
  operation brings it up to the MLS state (`marmot_group_reconcile()`), which
  is authoritative. Since 0.7.0, a storage with the transaction hooks never
  gets there (nostrc-qp24.7).
- **Storage labels.** `MarmotStorage` implementations that snapshot a group's
  MLS state must include the new labels `mls_group_parent` and
  `mls_group_pending` (GhStoreMarmot does).
- **Memory backend.** The in-memory backend no longer lists processed-event
  markers as messages.
- **Proposals.** Standalone Proposal messages return
  `MARMOT_ERR_UNSUPPORTED` (not queued).
- **kind:445 signatures** are verified by libmarmot since 0.6.0
  (nostrc-6r6s, above). Before 0.6.0, callers had to verify them.

### 0.4.1 (unreleased): Welcome path secrets

- **Joiners can follow every Commit.** The committer's Welcome now carries
  `GroupSecrets.path_secret` for the lowest common ancestor of itself and the
  joiner on its filtered direct path (RFC 9420 §12.4.3.1). The joiner derives
  the key pair of that node and of every node above it on the committer's
  filtered path, checks each public key against the ratchet tree (constant
  time), and keeps the private keys; a mismatching, malformed (wrong length)
  or badly framed (`optional<>` presence byte other than 0/1) path secret
  makes the Welcome invalid (`MARMOT_ERR_WELCOME_INVALID`). Before, the
  secret was never sent and was discarded on receipt, so a joiner could not
  decrypt a later Commit whose UpdatePath encrypted to that ancestor
  (`MARMOT_ERR_MLS_PROCESS_MESSAGE`, e.g. Charlie adds Dave, Alice
  self-updates). OpenMLS/MDK Welcomes, which always carried the secret, are
  now verified against it (8 of the passive-client vectors).
- **Compatibility.** No API, ABI or state-format change. A Welcome without a
  path secret is still accepted (a Commit need not carry an UpdatePath).
  0.4.0 joiners ignore the new field.
- GroupSecrets plaintext and its joiner/path secret copies are wiped after use.

### 0.4.0 (unreleased): RFC 9420 LeafNode signatures and Commit processing fixes

**Breaking wire change.** Update- and commit-source LeafNodes are now signed
and verified over a LeafNodeTBS that binds `group_id<V>` and `uint32
leaf_index` (RFC 9420 §7.2), and every Update and UpdatePath LeafNode is
validated before a Commit is applied (§7.3, §12.4.2).

- **Mixed-version groups split.** libmarmot ≤ 0.3.x signs commit leaves
  without that suffix, so a 0.4.0 member rejects every path-bearing Commit
  (Add, Remove, self-update, metadata update) from a ≤ 0.3.x member with
  `MARMOT_ERR_MLS_PROCESS_MESSAGE` and stays in the old epoch while the
  sender moves on. (0.3.x members still accept 0.4.0 Commits, so the break is
  one-directional.) There is no in-band recovery: the stranded member must be
  removed and re-added. **Upgrade every member to ≥ 0.4.0 before anyone sends
  a Commit.** Add Commits from ≤ 0.3.7 whose UpdatePath also encrypts to the
  new member are rejected as well (they were already undecryptable for some
  members and are rejected by OpenMLS/MDK).
- **Persisted groups.** The state format is unchanged and loads as before.
  But every leaf signed by ≤ 0.3.x -- the creator's leaf and each member's
  last commit leaf -- stays in the tree until that member commits again.
  RFC-conformant joiners (MDK/OpenMLS) validate every LeafNode of a Welcome
  ratchet tree (§12.4.3.1) and reject such trees; libmarmot joiners will too
  once `nostrc-3hzu` lands. **After upgrading, have each member self-update**
  so its leaf is re-signed. Trees persisted by ≤ 0.3.6 can also hold non-blank
  parents over empty copaths; the member's next Commit now blanks them, as
  receivers always did (§7.4/§7.5).
- **Interop gain.** MDK/OpenMLS now accept libmarmot path Commits; before,
  the missing suffix made them reject every one.
- **UpdatePath shape.** A lone committer sends a zero-node UpdatePath
  (§7.6); parent hashes link along the filtered direct path (§7.9); path
  secrets are not encrypted to leaves the Commit adds (§12.4.2), and
  receivers check the ciphertext count of every UpdatePathNode.
- **Commit processing fixes.** The committer keeps the private keys of the
  path nodes it installs (it could not follow the next Commit encrypting to
  them); the Add producer records the new leaf in `unmerged_leaves` like
  receivers do; Commit producers build on a staged copy and leave the group
  unchanged on failure.
- **Metadata updates.** `marmot_update_group_metadata` now commits a
  GroupContextExtensions proposal (§12.1.7) carrying the stored GroupData with
  only the given fields changed, and changes nothing if the Commit cannot be
  made. Receivers validate such proposals (one per Commit, supported by every
  member). The API did not yet return the Commit for publication, and
  `marmot_process_message` did not ingest Commits (`nostrc-9ata`, fixed in
  0.5.0).
- **Known gap** (fixed in 0.4.1). Welcomes did not carry
  `GroupSecrets.path_secret` (`nostrc-il4i`): a joiner could not follow a
  later Commit that encrypts to its common ancestor with the member who added
  it.

### 0.3.1 (unreleased): AppDataUpdate wire recognition (not adopted group support)

The MLS draft-10 AppDataUpdate `update` and `remove` proposal bodies have
strict wire parsing and round-trip tests. An internal, non-publishing state
transition now replaces `0x8003` admin-policy bytes for a same-epoch proposal
only after checking canonical dictionary/policy encoding, parent-epoch sender
and committer admin authority, and resulting-member invariants. It preserves
other dictionary entries byte-for-byte. This helper is not wired into Commit
processing: the group engine still returns `MARMOT_ERR_UNSUPPORTED` for
AppDataUpdate proposals. No adopted-profile state can be admitted yet.
To avoid silently accepting a group it cannot maintain, it also rejects a
GroupContext `app_data_dictionary` during creation, Welcome join, local-state
load, or a GroupContextExtensions commit. Legacy `0xF2EE` groups remain
readable; kind-30443 MDK 0.8 KeyPackage selection and the kind-443 rejection
policy are unchanged. The adopted KeyPackage producer remains OFF by default
and must not be treated as interoperable without a pinned independent peer.

### 0.2.0 (unreleased): KeyPackages move to addressable kind 30443

**Breaking wire change.** KeyPackage events are now kind **30443** instead of 443
(`MARMOT_KIND_KEY_PACKAGE`). The reference is the Marmot spec
([marmot-protocol/marmot](https://github.com/marmot-protocol/marmot) at `26fa6a6`),
`transports/nostr.md` sections "KeyPackage publication" and "Event identity and
tag cardinality". Tags and values match the MDK 0.8 events in
`tests/vectors/mdk/protocol-vectors.json`.

- **Emit.** Tags are emitted in the order `d`, `mls_protocol_version`,
  `mls_ciphersuite`, `mls_extensions`, `mls_proposals`, `relays` (only when
  relays are given), `i`, `encoding`. The NIP-70 `-` tag is gone: it is not
  part of the 30443 tag set, and relays without AUTH reject protected events.
- **Stable slot.** The `d` value is 32 random bytes, generated once per account
  and kept in the storage backend's MLS key store (label `kp_slot`, so the
  storage schema is unchanged). Every rotation reuses it, which lets relays
  replace the old KeyPackage.
- **Parse.** A KeyPackage event must be kind 30443 with exactly one `d` (64
  lowercase hex), `i`, `mls_protocol_version` and `encoding` tag, each with a
  single value. `mls_ciphersuite`, `mls_extensions` and `mls_proposals` must
  each be one tag holding distinct `0x`-prefixed ids. A `relays` tag is
  optional, but if present it must hold valid `ws`/`wss` URLs.
- **Select.** New `marmot_select_key_package_event()`: keeps the newest
  authenticated event per `(pubkey, d)` slot (ties go to the lower event id).
  A slot whose newest event is invalid yields nothing; older events in that
  slot are never used. Across slots it picks the newest valid event (ties go to
  the lower KeyPackageRef).
- **Legacy 443 is not accepted.** The adopted spec removed kind 443. MDK
  accepted it only until 2026-05-31, and MDK master rejects it. Welcomes for
  KeyPackages you already published as 443 still work, because private key
  material is looked up by KeyPackageRef and not by event kind. Peers that
  still publish only 443 must upgrade before you can invite them.
- **Not yet the adopted strict profile.** Content is still the raw
  `KeyPackage` rather than an `MLSMessage`, the `encoding`/`relays` tags remain
  for MDK 0.8 compatibility, and there is no `app_components`/`0x8009`
  account-identity proof (tracked as `nostrc-prqu.9`). The
  `mls_extensions`/`mls_proposals` tags advertise more than the LeafNode
  capabilities list (tracked as `nostrc-prqu.10`).

## Test Suite

16 test files with ~200+ test cases:

| Test File | Focus | Count |
|-----------|-------|-------|
| `test_mls_tls` | TLS presentation language codec | 11 |
| `test_mls_crypto` | Crypto primitives (SHA-256, HKDF, AES-GCM, X25519, Ed25519) | 14 |
| `test_mls_tree` | TreeKEM ratchet tree operations | 30 |
| `test_mls_key_schedule` | Key schedule derivation, secret tree, exporter | 21 |
| `test_mls_framing` | PrivateMessage encrypt/decrypt, sender data, reuse guard | 14 |
| `test_mls_key_package` | KeyPackage creation, validation, serialization | 12 |
| `test_mls_group` | Group state machine, add/remove members, commits | 21 |
| `test_mls_welcome` | Welcome construction, processing, joining | 8 |
| `test_extension` | NostrGroupDataExtension (0xF2EE) serialize/deserialize | 7 |
| `test_storage` | In-memory storage backend | 6 |
| `test_storage_contract` | Parametric tests across all backends (memory, SQLite, nostrdb) | 21 |
| `test_types` | Type lifecycle, config defaults, error strings | 15 |
| `test_protocol` | MIP-00 through MIP-03 end-to-end | 24 |
| `test_media` | MIP-04 encrypt/decrypt, tamper detection | 11 |
| `test_rfc9420_vectors` | RFC 9420 crypto validation (HKDF, Ed25519, AES-GCM, tree math) | 38 |
| `test_interop` | MDK interoperability vectors, self-consistency | 9 |

Run all tests:
```bash
ctest --test-dir build -R marmot --output-on-failure
```

## GObject Integration

For GTK/GNOME applications, use `marmot-gobject` which provides:
- GObject type wrappers (`MarmotGobjectGroup`, `MarmotGobjectMessage`, etc.)
- GTask-based async API (`_async` / `_finish` pattern)
- GObject signals (`group-joined`, `message-received`, `welcome-received`)
- GObject Introspection (GIR) for language bindings
- Vala bindings (VAPI)

See `marmot-gobject/` directory for the full wrapper library.

## pkg-config

```bash
pkg-config --cflags --libs marmot        # C library
pkg-config --cflags --libs marmot-gobject-1.0  # GObject wrapper
```

## License

MIT
