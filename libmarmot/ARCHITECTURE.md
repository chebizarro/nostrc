# libmarmot Architecture

Internal design guide for libmarmot — a pure C implementation of the Marmot protocol (MLS + Nostr).

## Design Decisions

### Why a focused MLS subset?

Full MLS (RFC 9420) supports multiple ciphersuites, extensions, proposals, and credential types. The Marmot protocol only requires ciphersuite 0x0001 (`MLS_128_DHKEMX25519_AES128GCM_SHA256_Ed25519`), basic credentials, and a fixed extension (0xF2EE). By implementing only what the protocol needs, we avoid the complexity of a general-purpose MLS library while maintaining byte-level interoperability with the MDK reference implementation.

### Why C, not Rust?

libmarmot is consumed by a GTK application written in C/GObject. A pure C library avoids FFI friction, integrates naturally with GLib's memory model, and can be wrapped by GObject Introspection for language bindings (Python, Vala, JavaScript).

### Storage vtable pattern

Rather than coupling to a specific database, the `MarmotStorage` vtable lets callers plug in any backend (memory, SQLite, nostrdb/LMDB, or custom). The vtable mirrors MDK's `MdkStorageProvider` trait, so the same storage semantics work across implementations.

## Layer Diagram

```
┌─────────────────────────────────────────────────────────┐
│                Public API (marmot.h)                     │
│  Lifecycle: marmot_new / marmot_free                     │
│  Groups:    create_group / process_welcome / accept       │
│  Messages:  create_message / process_message              │
│  Media:     encrypt_media / decrypt_media                 │
│  Creds:     create_key_package                            │
├─────────────────────────────────────────────────────────┤
│              Protocol Layer (MIP-00 through MIP-04)       │
│                                                           │
│  credentials.c  — MIP-00: KeyPackage creation (kind:443) │
│  extension.c    — MIP-01: NostrGroupDataExt (0xF2EE)     │
│  groups.c       — MIP-01: Group construction              │
│  welcome.c      — MIP-02: Welcome events (kind:444)      │
│  messages.c     — MIP-03: Group messages (kind:445)       │
│  media.c        — MIP-04: Encrypted media                 │
├─────────────────────────────────────────────────────────┤
│                MLS Layer (RFC 9420)                        │
│                                                           │
│  mls_crypto.c      — X25519, Ed25519, AES-128-GCM, HKDF │
│  mls_tls.c         — TLS codec: VLI, opaque<V>, u8..u64 │
│  mls_tree.c        — TreeKEM left-balanced ratchet tree   │
│  mls_key_schedule.c — Epoch secrets, sender ratchets      │
│  mls_framing.c     — PrivateMessage encrypt/decrypt       │
│  mls_key_package.c — KeyPackage create/validate/ref       │
│  mls_group.c       — Group state machine, commits         │
│  mls_welcome.c     — Welcome message construction         │
├─────────────────────────────────────────────────────────┤
│           Storage Layer (MarmotStorage vtable)            │
│                                                           │
│  storage_memory.c  — In-memory (testing/ephemeral)        │
│  storage_sqlite.c  — SQLite3 (persistent, optional enc)   │
│  storage_nostrdb.c — nostrdb + LMDB (shares NDB instance)│
├─────────────────────────────────────────────────────────┤
│               Crypto Dependencies                         │
│                                                           │
│  libsodium: Ed25519, X25519, ChaCha20-Poly1305, CSPRNG  │
│  OpenSSL:   AES-128-GCM, HKDF-SHA256, SHA-256           │
└─────────────────────────────────────────────────────────┘
```

## MLS Layer Design

### TLS Serialization (`mls_tls.c`)

The MLS protocol uses a TLS presentation language for wire encoding. OpenMLS (and MDK) use QUIC-style Variable-Length Integer encoding (RFC 9000 §16) for all `opaque<V>` length prefixes:

| Value Range | Encoding | MSB Prefix |
|-------------|----------|------------|
| 0–63 | 1 byte | `00` |
| 64–16,383 | 2 bytes | `01` |
| 16,384–2^30 | 4 bytes | `10` |

This was discovered during MDK interop testing and is critical for byte-level compatibility.

### TreeKEM Ratchet Tree (`mls_tree.c`)

The ratchet tree is a left-balanced binary tree where leaf nodes hold member keys and parent nodes hold shared secrets derived via X25519.

Key operations:
- **Resolution**: Find non-blank nodes in a subtree
- **Filtered direct path**: Compute the path to root, skipping nodes with empty copath resolutions
- **Tree hash**: SHA-256 hash of the tree structure (for GroupContext)
- **Parent hash**: Verify commit validity

### Key Schedule (`mls_key_schedule.c`)

Each epoch derives a tree of secrets from `init_secret + commit_secret`:

```
init_secret ──┐
              ├─► joiner_secret ──► member_secret ──► epoch_secret
commit_secret ┘                                          │
                                              ┌──────────┼──────────┐
                                         sender_data  encryption  exporter
                                           secret      secret     secret
```

The sender ratchet derives per-message keys using a secret tree indexed by leaf position, with forward secrecy through hash ratcheting.

### Group State Machine (`mls_group.c`)

Groups progress through epochs via commits. Each commit:
1. Applies pending proposals (Add/Remove/Update)
2. Updates the ratchet tree (new path secrets)
3. Derives the next epoch's key schedule
4. Produces an encrypted commit message

### Framing (`mls_framing.c`)

MLS messages are framed as `PrivateMessage`:
- Content type: application (1), proposal (2), commit (3)
- Encrypted with sender-derived key from the secret tree
- Includes a 4-byte reuse guard to prevent nonce collision

## Protocol Layer Design

### NIP-44 / NIP-59 Integration

- **MIP-02 (Welcome)**: The MLS Welcome message is serialized, then wrapped in a NIP-59 gift wrap (kind:1059) addressed to each invited member. The inner rumor is kind:444.
- **MIP-03 (Messages)**: Group messages are MLS PrivateMessages, base64-encoded as NIP-44 encrypted content in a kind:445 event. Each member decrypts using their leaf's sender ratchet.

### Group Data Extension (0xF2EE)

The `NostrGroupDataExtension` is embedded in the MLS GroupContext and carries Nostr-specific metadata:

```
struct {
    uint16   version;          // Currently 2
    uint8    nostr_group_id[32];
    opaque   name<V>;
    opaque   description<V>;
    opaque   admins<V>;        // Concatenated 32-byte pubkeys
    opaque   relays<V>;        // VLI-prefixed URL list
    // v2 fields:
    opaque   image_hash[32];   // Optional
    opaque   image_key[32];    // Optional
    opaque   image_nonce[12];  // Optional
    opaque   image_upload_key[32]; // Optional
}
```

### MIP-04 Encrypted Media

Media files are encrypted with ChaCha20-Poly1305 using keys derived from the MLS exporter secret:

```
media_key = HMAC-SHA256(exporter_secret[epoch], "marmot-media-key" || 0x01)
nonce = random(12)
ciphertext = ChaCha20-Poly1305(media_key, nonce, plaintext, aad=mime_type)
```

The `imeta` tag carries the nonce, file hash, epoch, and MIME type for decryption.

## File Map

### Public Headers (`include/marmot/`)

| File | Purpose |
|------|---------|
| `marmot.h` | Main API: lifecycle, groups, messages, media |
| `marmot-types.h` | All public types (MarmotGroup, MarmotMessage, etc.) |
| `marmot-storage.h` | Storage vtable and backend constructors |
| `marmot-error.h` | Error codes (MarmotError enum) |

### Internal Headers (`src/` and `src/mls/`)

| File | Purpose |
|------|---------|
| `marmot-internal.h` | Internal Marmot struct, helper functions |
| `mls-internal.h` | MLS crypto, TLS codec, constants |
| `mls_tree.h` | TreeKEM types (MlsLeafNode, MlsParentNode, etc.) |
| `mls_group.h` | MLS group state, proposals, UpdatePath |
| `mls_key_schedule.h` | Key schedule types, sender ratchet |
| `mls_key_package.h` | KeyPackage types, private key material |
| `mls_framing.h` | PrivateMessage framing types |
| `mls_welcome.h` | Welcome message types |

## MDK Interoperability

Byte-level interop with MDK is validated by:

1. **RFC 9420 test vectors** (`test_rfc9420_vectors.c`) — crypto primitives, tree math, GroupContext serialization
2. **MDK-generated vectors** (`tests/vectors/mdk/`) — key schedule, secret tree, tree operations, message protection, passive client scenarios
3. **Protocol vectors** (`tests/vectors/mdk/protocol-vectors.json`) — KeyPackage serialization, RefHash, Nostr event structure, group lifecycle

Key interop discoveries:
- OpenMLS uses **QUIC VLI** (not fixed-width) for TLS `opaque<V>` length prefixes
- RefHash uses VLI-encoded label and value lengths
- MLS Capabilities struct has 5 vectors (versions, ciphersuites, extensions, proposals, credentials)
- LeafNode includes Lifetime struct for key_package source
