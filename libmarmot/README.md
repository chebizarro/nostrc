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

All backends implement the same `MarmotStorage` vtable (25 operations):
- Group CRUD (save, find by MLS ID, find by Nostr ID, list all, update relays)
- Message operations (save, find, pagination, last message, processed tracking)
- Welcome operations (save, find, pending list, processed tracking)
- MLS key-value store (label+key → value, for MLS internal state)
- Exporter secrets (per-group per-epoch secret storage)
- Snapshots (for commit race resolution)

## MDK Interoperability

libmarmot is designed for byte-level interoperability with the [MDK](https://github.com/marmot-org/mdk) reference implementation:

- **Type mapping**: All C types mirror MDK's Rust structs field-for-field
- **Config defaults**: `MarmotConfig` defaults match `MdkConfig` exactly
- **Storage interface**: `MarmotStorage` vtable maps 1:1 to MDK's `MdkStorageProvider` trait
- **Extension format**: TLS serialization of `NostrGroupDataExtension` (0xF2EE) is byte-identical
- **Error codes**: `MarmotError` enum mirrors MDK's `MdkError` variants
- **Protocol constants**: kind:30443/444/445, extension type 0xF2EE

Test vectors from MDK can be placed in `tests/vectors/mdk/` for automated cross-validation.

## Changelog

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
| the previous epoch, a different Commit | replaces the applied one only if it wins the ordering, else `MARMOT_ERR_WRONG_EPOCH` |
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
  is authoritative. Real transactions are nostrc-qp24.7.
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
