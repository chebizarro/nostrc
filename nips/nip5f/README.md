# NIP-5F Unix Socket Signer

A local Unix-domain socket JSON-RPC signer service and client for Nostr.

- Server: `nostr-signer-sockd`
- Client library: `nostr_nip5f_core` (`nips/nip5f/src/core/`)
- Example client: `nip5f_client_example`

## Socket path
- Default: `$HOME/.local/share/nostr/signer.sock`
- Override via: `NOSTR_SIGNER_SOCK`

## Environment variables
- NOSTR_SIGNER_KEY: preferred; secret key as 64-hex or bech32 `nsec1...`
- NOSTR_SIGNER_SECKEY_HEX: legacy hex fallback
- NOSTR_SIGNER_NSEC: legacy nsec fallback
- NOSTR_SIGNER_LOG: set to `1` to enable debug logs

The server resolves the secret key in this order: KEY → SECKEY_HEX → NSEC. Secrets are zeroized best-effort after use.

## JSON-RPC API
All requests/answers are single-line JSON frames over the Unix socket.

- Common envelope
  - Request: `{ "id": "<string>", "method": "<name>", "params": <json|null> }`
  - Response: `{ "id": "<same-id>", "result": <json>, "error": null }` on success

### get_public_key
- Request: `{ "id": "1", "method": "get_public_key", "params": null }`
- Result: hex pubkey string
  - Response: `{ "id":"1", "result":"<64-hex>", "error":null }`

### sign_event
- Params: JSON event object (may omit `pubkey`, `id`, `sig`)
  - `{ "event": <event-json>, "pubkey": "<optional 64-hex>" }`
- Response `result`: full signed event object JSON

### nip44_encrypt
- Params: `{ "peer_pub":"<64-hex>", "plaintext":"<utf8 string>" }`
- Result: base64 ciphertext string

### nip44_decrypt
- Params: `{ "peer_pub":"<64-hex>", "cipher_b64":"<base64>" }`
- Result: plaintext string

### list_public_keys
- Params: `null`
- Result: JSON array of known pubkeys, e.g. `["<64-hex>"]`

## Build & run
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
```

Run loopback test (no network):
```
NOSTR_SIGNER_KEY=<hex-or-nsec> build/nips/nip5f/test_nip5f_loopback
```

Run the server and example client:
```
# Terminal A: server
NOSTR_SIGNER_KEY=<hex-or-nsec> build/nips/nip5f/nostr-signer-sockd

# Terminal B: example client
NOSTR_SIGNER_SOCK=$HOME/.local/share/nostr/signer.sock build/nips/nip5f/nip5f_client_example
```

## Notes
- This NIP-5F client centralizes JSON request building and IO framing; helpers are internal-only to keep public APIs minimal.
- See `nips/nip5f/ADR-0001-api.md` and `docs/proposals/5F.md` for design background.
