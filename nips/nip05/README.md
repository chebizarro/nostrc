# NIP-05 (Mapping Nostr Identifiers to Pubkeys)

This module implements NIP-05 lookup and validation using the project's JSON abstraction and key utilities.

- JSON: uses `libnostr` JSON interface (`libnostr/include/json.h`) — no direct JSON library usage in this module.
- Keys: uses `libnostr/include/keys.h` for pubkey validation.

## API

- `nostr_nip05_parse_identifier(id, &name, &domain)`
  - Parses `name@domain` or `domain` (yields name "_"). Returns lower-cased parts.
- `nostr_nip05_fetch_json(domain, &json, &err)`
  - Fetches `https://domain/.well-known/nostr.json` via libcurl.
- `nostr_nip05_lookup(identifier, &hexpub, &relays, &count, &err)`
  - Resolves identifier, trying `?name=...` first, then full document.
- `nostr_nip05_resolve_from_json(name, json, &hexpub, &relays, &count, &err)`
  - Resolve from an already-fetched JSON string (no network). Great for tests.
- `nostr_nip05_validate(identifier, hexpub, &match, &err)`
  - Validates that identifier maps to the given hex pubkey.

All returned strings must be freed with `free()`.

## Environment variables

- `NIP05_TIMEOUT_MS` — HTTP timeout in milliseconds (default 5000).
- `NIP05_ALLOW_INSECURE=1` — disable TLS verification (testing only).

## Linking

- Link targets: `nip05`, `nostr`, `nostr_json`, and `CURL::libcurl`.
- Example target `nip05_example` demonstrates usage.

## Tests

Unit tests are network-free and use `nostr_nip05_resolve_from_json`:

- `test_nip05_parse`
- `test_nip05_resolve_json_names`
- `test_nip05_resolve_json_relays`
- `test_nip05_resolve_json_errors`

Run all tests via CTest:

```
cmake --build build -j
ctest --test-dir build -j
```
