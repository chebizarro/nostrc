# libmarmot

Pure C implementation of the [Marmot protocol](https://github.com/marmot-org/mdk) (MLS + Nostr) for secure group messaging.

## Overview

libmarmot implements the Marmot Improvement Proposals (MIPs) for encrypted group messaging over Nostr using the Messaging Layer Security (MLS) protocol (RFC 9420):

| MIP | Description | Event Kind | Status |
|-----|-------------|------------|--------|
| MIP-00 | Credentials & KeyPackages | 443 | ✅ Complete |
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

### MIP-00: Credentials & KeyPackages (kind 443)

```c
MarmotKeyPackageResult result;
int rc = marmot_create_key_package(m, my_pubkey, relay_urls, relay_count, &result);
// result.event → unsigned kind:443 event to sign and publish
// result.key_package_ref → 32-byte identifier for this key package
marmot_key_package_result_clear(&result);
```

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
- **Protocol constants**: kind:443/444/445, extension type 0xF2EE

Test vectors from MDK can be placed in `tests/vectors/mdk/` for automated cross-validation.

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
