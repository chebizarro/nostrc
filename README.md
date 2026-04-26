# Nostr C Library

![libgo CI](https://github.com/chebizarro/nostrc/actions/workflows/libgo-ci.yml/badge.svg)

The Nostr C library provides an implementation of the Nostr protocol, including various NIPs (Nostr Improvement Proposals). This library aims to be highly portable, suitable for use in IoT environments, and provides bindings for integration with the GNOME desktop environment.

## Features

- Nostr event handling
- JSON (de)serialization with optional NSON support
- 54 NIP implementations (see [full list](#nip-implementations) below)
- Optional memory management handled by the library
 - NIP-47 (Wallet Connect): canonical helpers for encrypt/decrypt supporting NIP-44 v2 (preferred) and NIP-04 fallback, with automatic key format handling (x-only and SEC1) and full tests/examples.

## Security Hardening and Migration Notes

- Canonical NIP-01 hashing/signing/verification
  - Event IDs and signatures now use the canonical preimage array `[0, pubkey, created_at, kind, tags, content]` and ignore any caller-provided `id` when verifying. This prevents trusting untrusted `id` fields and aligns with the NIP-01 spec. Existing APIs continue to work; `nostr_event_check_signature()` always recomputes the hash.

- NIP-04 AEAD migration (v2)
  - New default envelope format: `v=2:base64(nonce(12) || ciphertext || tag(16))` using AES-256-GCM.
  - Keys are derived via HKDF-SHA256 with `info="NIP04"` for domain separation from the ECDH shared secret.
  - Encryption now emits AEAD v2 only. Legacy AES-CBC `?iv=` is no longer produced by library APIs.
  - Decrypt fallback remains: legacy AES-CBC `?iv=` content is still accepted to preserve interop with older peers.
  - All decryption failures return a unified error string ("decrypt failed") to reduce side-channel leakage.

  Optional hardening:
  - Build-time switch to disable legacy decrypt entirely: set `-DNIP04_STRICT_AEAD_ONLY=ON` when configuring CMake to reject any non-`v=2:` envelopes at decrypt.

  Migration guidance:
  - Prefer `nostr_nip04_encrypt`/`nostr_nip04_encrypt_secure` and `nostr_nip04_decrypt`/`nostr_nip04_decrypt_secure`.
  - The helper `nostr_nip04_shared_secret_hex` is deprecated and should not be used by new code; exposing raw ECDH shared secrets increases attack surface.
  - Tests and examples have been updated to expect `v=2:` envelopes and avoid `?iv=` parsing.

- Relay ingress hardening
  - Adds an in-memory replay cache (TTL 15 minutes) keyed by canonical event `id` to avoid redundant storage/flood.
  - Enforces timestamp skew limits (+10 minutes future, -24 hours past) on `created_at`.
  - On startup, the relay prints a concise security posture banner, for example:
    - `nostrc-relayd: security AEAD=v2 replayTTL=900s skew=+600/-86400`

See `tests/test_event_canonical.c` and `tests/test_nip04_aead.c` for minimal validation of these behaviors.

## Quick Start

Build the libraries and tests with CMake:

```sh
git clone https://github.com/chebizarro/nostrc.git
cd nostrc
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j
ctest --output-on-failure
```

Install system-wide (optional):

```sh
sudo make install
```

Link in your C project:

```cmake
find_library(NOSTR_LIB libnostr REQUIRED)
find_library(NOSTR_JSON_LIB nostr_json REQUIRED)
find_library(NSYNC_LIB nsync REQUIRED)
find_package(OpenSSL REQUIRED)
pkg_check_modules(SECP256K1 REQUIRED libsecp256k1)

add_executable(my_app main.c)
target_link_libraries(my_app PRIVATE ${NOSTR_LIB} ${NOSTR_JSON_LIB} ${NSYNC_LIB} OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
```

## Documentation

- See `docs/LIBJSON.md` for libjson API, NIP-01 #tag mapping, robustness rules, and tests.
- See `docs/SHUTDOWN.md` for libnostr/libgo shutdown order, invariants, and troubleshooting.
- See `docs/NIP47.md` for NIP-47 (Wallet Connect) envelope helpers, negotiation, canonical crypto helpers (NIP-44 v2/NIP-04), accepted key formats (x-only/SEC1), sessions, GLib bindings, and examples.
- See `docs/NIP04_MIGRATION.md` for migrating to NIP-04 AEAD v2 envelopes and deprecation details.

## Installation

### Dependencies

- C compiler (GCC/Clang)
- CMake
- libsecp256k1
- libjansson (optional, for JSON parsing)

### Building

```sh
git clone https://github.com/chebizarro/nostrc.git
cd nostrc
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j
sudo make install
```

## NIP Implementations

This library includes 54 Nostr Improvement Proposals (NIPs), each in its own
directory under `nips/`:

| NIP | Description |
|-----|-------------|
| [01](nips/nip01/) | Basic Protocol Flow |
| [02](nips/nip02/) | Follow List |
| [04](nips/nip04/) | Encrypted Direct Messages (legacy) |
| [05](nips/nip05/) | DNS-based Internet Identifiers |
| [06](nips/nip06/) | Key Derivation from Mnemonic Seed Phrase |
| [10](nips/nip10/) | Text Note Replies |
| [11](nips/nip11/) | Relay Information Document |
| [13](nips/nip13/) | Proof of Work |
| [14](nips/nip14/) | Subject Tag in Text Events |
| [17](nips/nip17/) | Private Direct Messages |
| [18](nips/nip18/) | Reposts |
| [19](nips/nip19/) | Bech32-Encoded Entities |
| [21](nips/nip21/) | nostr: URI Scheme |
| [22](nips/nip22/) | Comment |
| [23](nips/nip23/) | Long-Form Content |
| [24](nips/nip24/) | Extra Metadata Fields |
| [25](nips/nip25/) | Reactions |
| [27](nips/nip27/) | Text Note References |
| [28](nips/nip28/) | Public Chat |
| [29](nips/nip29/) | Relay-Based Groups |
| [30](nips/nip30/) | Custom Emoji |
| [31](nips/nip31/) | Dealing with Unknown Events |
| [34](nips/nip34/) | Git Stuff |
| [39](nips/nip39/) | External Identities |
| [40](nips/nip40/) | Expiration Timestamp |
| [42](nips/nip42/) | Authentication of Clients to Relays |
| [44](nips/nip44/) | Versioned Encryption |
| [45](nips/nip45/) | Counting Results |
| [46](nips/nip46/) | Nostr Connect |
| [47](nips/nip47/) | Wallet Connect |
| [49](nips/nip49/) | Private Key Encryption |
| [50](nips/nip50/) | Search Capability |
| [51](nips/nip51/) | Lists |
| [52](nips/nip52/) | Calendar Events |
| [53](nips/nip53/) | Live Activities |
| [54](nips/nip54/) | Wiki |
| [55L](nips/nip55l/) | Linux Signer Application (DBus) |
| [57](nips/nip57/) | Lightning Zaps |
| [58](nips/nip58/) | Badges |
| [59](nips/nip59/) | Gift Wrap |
| [5F](nips/nip5f/) | Local Signer via Unix Socket |
| [60](nips/nip60/) | Cashu Wallet |
| [61](nips/nip61/) | Nutzaps |
| [65](nips/nip65/) | Relay List Metadata |
| [70](nips/nip70/) | Protected Events |
| [73](nips/nip73/) | External Content IDs |
| [75](nips/nip75/) | Zap Goals |
| [77](nips/nip77/) | Negentropy Syncing |
| [86](nips/nip86/) | Relay Management API |
| [92](nips/nip92/) | Media Attachments |
| [94](nips/nip94/) | File Metadata |
| [98](nips/nip98/) | HTTP Auth |
| [99](nips/nip99/) | Classified Listings |
| [B0](nips/nipb0/) | Blossom Integration |

## Contributing

Contributions are welcome! Please open issues or submit pull requests on GitHub.

Guidelines:

1. Fork and create a topic branch.
2. Add focused changes with tests in `tests/` or `libgo/tests/`.
3. Update docs (README, ARCHITECTURE, API) when public APIs change.
4. Run `ctest` and ensure no regressions.
5. Follow the style in `CODING_STANDARDS.md`.

### Usage Examples

See `examples/` for basic JSON integration and event serialization. For NIP-47 examples, see:

```
./build/nips/nip47/nwc_client_example
./build/nips/nip47/nwc_wallet_example
```

A minimal flow:

```c
#include "nostr-event.h"
#include "keys.h"

int main(void) {
    NostrEvent *ev = create_event();
    // set fields on ev...
    char *json = event_serialize(ev);
    // use json...
    free(json);
    free_event(ev);
    return 0;
}
```

### Adding New NIPs

To add a new NIP:

1. Create a new folder in the `nips` directory.
2. Implement the required functionality in C.
3. Update the headers and add test cases.
4. Ensure all tests pass and submit a pull request.

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.

## Shutdown Quick Reference

For correct relay/connection/subscription teardown and to avoid hangs or use-after-free during shutdown, see:

- docs/SHUTDOWN.md

Key points:

- Cancel context, close relay queues, snapshot+null the connection.
- Wait for relay workers to exit, then free `conn` channels.
- Finally call `connection_close(conn)`; it closes channels but does not free them.

The official Nostr NIPs are vendored as a git submodule under `docs/nips`.
- Update with `scripts/update_nips.sh`
- Keep the submodule pinned; bump deliberately in separate commits
- Code under `nips/nipXX/` MUST reference the matching `docs/nips/XX.md`

### Developer Notes (libgo)

- Sanitizers (Debug):

```
# AddressSanitizer + UndefinedBehaviorSanitizer
cmake -S libgo -B build -DCMAKE_BUILD_TYPE=Debug -DGO_ENABLE_ASAN=ON -DGO_ENABLE_UBSAN=ON
cmake --build build -j && ctest --test-dir build --output-on-failure

# ThreadSanitizer
cmake -S libgo -B build_tsan -DCMAKE_BUILD_TYPE=Debug -DGO_ENABLE_TSAN=ON
cmake --build build_tsan -j && ctest --test-dir build_tsan --output-on-failure
```

- Warnings:

```
cmake -S libgo -B build -DCMAKE_BUILD_TYPE=Debug -DGO_WARNINGS_AS_ERRORS=ON
```
