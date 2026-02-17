# libmarmot

Pure C implementation of the [Marmot protocol](https://github.com/marmot-org/mdk) (MLS + Nostr) for secure group messaging.

## Overview

libmarmot implements the Marmot Improvement Proposals (MIPs) for encrypted group messaging over Nostr using the Messaging Layer Security (MLS) protocol (RFC 9420):

| MIP | Description | Event Kind |
|-----|-------------|------------|
| MIP-00 | Credentials & KeyPackages | 443 |
| MIP-01 | Group Construction (Extension 0xF2EE) | — |
| MIP-02 | Welcome Events (NIP-59 gift-wrapped) | 444 |
| MIP-03 | Group Messages (NIP-44 encrypted) | 445 |
| MIP-04 | Encrypted Media | — |

**Ciphersuite**: `MLS_128_DHKEMX25519_AES128GCM_SHA256_Ed25519` (0x0001)

## Architecture

```
┌─────────────────────────────────────────────┐
│             Public API (marmot.h)           │
│  marmot_create_group / process_welcome / …  │
├─────────────────────────────────────────────┤
│            Protocol Layer                    │
│  credentials · groups · welcome · messages  │
├─────────────────────────────────────────────┤
│              MLS Layer (RFC 9420)            │
│  mls_crypto · mls_tls · mls_tree ·          │
│  mls_key_schedule · mls_framing ·           │
│  mls_key_package · mls_group · mls_welcome  │
├─────────────────────────────────────────────┤
│         Storage Interface (vtable)           │
│  memory backend · sqlite backend             │
├─────────────────────────────────────────────┤
│  libsodium (Ed25519/X25519)   OpenSSL (AES/HKDF/SHA)  │
└─────────────────────────────────────────────┘
```

## Dependencies

- **libsodium** — Ed25519 signing, X25519 DH, CSPRNG
- **OpenSSL** — AES-128-GCM, HKDF-SHA256, SHA-256
- **libnostr** (optional) — Nostr event creation and signing
- **NIP-44** (optional) — Content encryption for group messages
- **NIP-59** (optional) — Gift wrapping for welcomes and messages

## Building

### CMake (in-tree, part of nostrc)

```bash
cd nostrc
cmake -B build -DBUILD_LIBMARMOT=ON
cmake --build build
ctest --test-dir build -R marmot
```

### CMake (standalone)

```bash
cd nostrc/libmarmot
cmake -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build
```

### Meson (standalone)

```bash
cd nostrc/libmarmot
meson setup build -Dtests=true
ninja -C build
ninja -C build test
```

## MDK Interoperability

libmarmot is designed to be fully interoperable with the [MDK](https://github.com/marmot-org/mdk) reference implementation from day one:

- **Type mapping**: All C types (MarmotGroup, MarmotMessage, MarmotWelcome, etc.) mirror MDK's Rust structs field-for-field
- **Config defaults**: MarmotConfig defaults match MdkConfig exactly
- **Storage interface**: MarmotStorage vtable maps 1:1 to MDK's MdkStorageProvider trait
- **Extension format**: TLS serialization of NostrGroupDataExtension (0xF2EE) is byte-identical
- **Error codes**: MarmotError enum mirrors MDK's MdkError variants

## Tests

```
test_mls_tls      — TLS presentation language codec (round-trip, error cases)
test_mls_crypto   — Crypto primitives (SHA-256, HKDF, AES-GCM, X25519, Ed25519)
test_extension    — Extension serialize/deserialize (minimal, image, v2)
test_storage      — Memory storage backend (groups, messages, MLS key store)
test_types        — Type lifecycle, config defaults, error strings
```

## Status

🚧 **Under development** — scaffold complete with crypto foundation, storage, and extension serialization. MLS protocol operations (TreeKEM, key schedule, framing) are next.

## License

MIT
