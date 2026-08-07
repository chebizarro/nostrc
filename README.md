# Nostr C Library

![libgo CI](https://github.com/chebizarro/nostrc/actions/workflows/libgo-ci.yml/badge.svg)

The Nostr C library provides an implementation of the Nostr protocol, including various NIPs (Nostr Improvement Proposals). This library aims to be highly portable, suitable for use in IoT environments, and provides bindings for integration with the GNOME desktop environment.

## Features

- Nostr event handling
- JSON (de)serialization with optional NSON support
- 54 NIP implementations (see [full list](#nip-implementations) below)
- Optional memory management handled by the library
 - NIP-47 (Wallet Connect): canonical helpers for encrypt/decrypt supporting NIP-44 v2 (preferred) and NIP-04 fallback, with automatic key format handling (x-only and SEC1) and full tests/examples.

## Security Posture and Migration Notes

Security boundaries are component-specific. The event library, relay client,
relay servers, storage backends, encryption modules, and GNostr media UI do not
substitute for one another.

### NIP-01 event admission

- `nostr_event_validate()` requires all signed-event fields, recomputes the
  canonical `[0, pubkey, created_at, kind, tags, content]` SHA-256 ID, requires
  the declared `id` to match, and verifies the Schnorr signature against that
  same hash. `nostr_event_get_id()` also recomputes instead of trusting or
  caching a caller-supplied ID.
- The libnostr WebSocket receive path validates network `EVENT` frames before
  subscription dispatch; its legacy `assume_valid` field does not bypass
  validation for network events. Envelope parsing remains structurally
  separate from cryptographic validation, so non-network callers must invoke
  the validator at their own trust boundary.
- `relayd` and `grelay` validate before storage, acknowledgement, or fanout.
  Both bound event bytes before JSON allocation/signature work and apply
  monotonic weighted frame and signature-verification budgets at connection,
  source-IP, and global levels.

### Relay replay and timestamp defaults

The shipped `relayd` defaults and `apps/relayd/relay.toml.example` agree:

| Setting | Default | Meaning |
|---|---:|---|
| `replay_cache_capacity` | 65536 | Process-local canonical-ID entries |
| `replay_ttl_seconds` | 900 | 15-minute replay window |
| `future_skew_seconds` | 600 | Reject events over 10 minutes in the future |
| `past_skew_seconds` | 0 | Disabled for historical sync and randomized gift-wrap timestamps |

Replay IDs are reserved before storage, committed only after successful
storage, and rolled back on failure. A full replay set fails closed rather than
silently shortening the TTL. This state is bounded and process-local; it is not
a cross-process or permanent replay database. `grelay` ships the same replay
TTL and skew defaults through its environment configuration. `relayd` prints
its effective posture at startup, for example:

`nostrc-relayd: security validator=canonical replayTTL=900s replayCapacity=65536 skew=+600/-0 ...`

### NIP-04, NIP-44, and NIP-46

- Generic `nostr_nip04_encrypt()` and `nostr_nip04_encrypt_secure()` emit the
  nostrc `v=2:` AES-256-GCM extension. Its key is derived from ECDH shared X
  with HKDF-SHA256 and `info="NIP04"`. This is a project extension, not a wire
  format implied by generic NIP-04 support.
- Original NIP-04 AES-256-CBC/PKCS#7 emission remains available only through
  the explicit `nostr_nip04_encrypt_legacy_secure()` compatibility API. CBC is
  unauthenticated and malleable. The former `NIP04_LEGACY_CBC` environment
  downgrade no longer changes generic encryption output.
- Decryption accepts AEAD v2 and, by default, original `?iv=` CBC payloads for
  interoperability. Every public decrypt failure returns the same
  `"decrypt failed"` error, including legacy padding failures, to avoid a
  format/padding oracle. This uniform error contract does not add integrity to
  CBC; authenticate and validate the outer event. Build with
  `-DNIP04_STRICT_AEAD_ONLY=ON` to reject CBC decrypt entirely.
- NIP-46 sessions default to standard NIP-44 v2 and own an explicit transport
  mode. Legacy NIP-04 CBC or the nostrc AEAD extension must be selected after
  peer capability negotiation; ciphertext shape cannot change the mode.
  NIP-44 decoding is strict and propagates KDF/HMAC/provider failures.

See `docs/NIP04_MIGRATION.md` for the API and interoperability matrix.

### GNostr remote media and metadata privacy

- A single global setting (`load-remote-media`) controls automatic fetching of
  remote media, previews, and avatars. It is **enabled by default**, so media,
  previews, and avatars load automatically. Users who disable it switch to a
  privacy/consent mode: cached content still renders, automatic network fetches
  stop, and explicit per-item media/preview actions (click-to-load) can still
  request uncached content on demand.
- HTTP(S) media URLs reject embedded credentials, non-public literal/resolved
  addresses, reserved/private destinations, and ports other than 80/443.
  Redirects are manual, same-origin, revalidated, and limited to five hops.
  Avatar requests use the same intent and URL policy.
- Fetches are still direct: an allowed origin can observe the receiver's IP and
  request timing. No privacy proxy is configured, and DNS is validated before
  the later HTTP connection rather than pinned, so DNS rebinding remains a
  residual risk.
- Kind:0 profile metadata is public and tied to a stable pubkey. Names, NIP-05
  identifiers, biographies, contact/payment identifiers, and avatar URLs can
  become long-lived correlated PII. GNostr warns before profile publication
  and does not log complete profile events or full pubkeys from the profile
  list model. The model retains loaded metadata and copied pubkey/search state
  until explicitly cleared or finalized; clearing it does not delete nostrdb,
  media-cache, relay, or peer copies.

### nostrdb validation scope

The nostrdb wrapper normally leaves nostrdb ID/signature validation enabled.
Its unsafe test path activates only when configuration requests
`ingest_skip_validation` **and** `NOSTR_ALLOW_UNSAFE_INGEST=1` is present; it
then emits a prominent warning. This is storage-layer defense in depth only.
Do not claim or assume nostrdb protects client dispatch, relay
acknowledgements, replay state, pre-store work, or alternate storage drivers;
each ingress path must validate before those actions.

Relevant regression coverage includes `libnostr/tests/test_event_validation.c`,
`apps/relayd/tests/test_ingress_security.c`,
`apps/relayd/tests/test_rate_limit_security.c`,
`apps/grelay/tests/test_ingress_security.c`,
`nips/nip04/tests/test_nip04_error_uniformity.c`,
`nips/nip44/tests/test_nip44_vectors.c`,
`nips/nip46/tests/test_transport_modes.c`,
`apps/gnostr/tests/test_media_url_policy.c`, and
`apps/gnostr/tests/test_profile_cache_privacy.c`.

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
