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
| **libnostr** | Nostr event creation, signing, secp256k1 | Optional (for MIP-00─03) |
| **NIP-44** | Content encryption for group messages | Optional (for MIP-03) |
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

```bash
cd nostrc/libmarmot
meson setup build -Dtests=true
ninja -C build
ninja -C build test
```

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
// out.event → kind:445 event to publish

// Receive
MarmotMessageResult result;
rc = marmot_process_message(m, received_event, &result);
if (result.type == MARMOT_MSG_APPLICATION) {
    // result.inner_event → decrypted inner event
    // result.sender_pubkey → verified sender
}
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

### 0.3.1 (unreleased): AppDataUpdate wire recognition (not adopted group support)

The MLS draft-10 AppDataUpdate `update` and `remove` proposal bodies now have
strict wire parsing and round-trip tests. The group engine still returns
`MARMOT_ERR_UNSUPPORTED` for such proposals: it does **not** apply component
state, validate the `0x8003` admin policy, or authorize component mutations.
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
