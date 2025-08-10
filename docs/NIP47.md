# NIP-47 (Nostr Wallet Connect) – Helpers, Sessions, and Examples

This document describes the NIP-47 envelope helpers, encryption negotiation, session helpers, and GLib bindings provided by this repo.

## Overview
- Request kind: 23194
- Response kind: 23195
- Supported encryption labels: `"nip44-v2"` (preferred), `"nip04"` (fallback)

## Envelopes
- Request build: `nostr_nwc_request_build(wallet_pub_hex, enc, body, &out_json)`
- Request parse: `nostr_nwc_request_parse(json, &wallet_pub_hex, &enc, &out_body)`
- Response build: `nostr_nwc_response_build(client_pub_hex, req_event_id, enc, body, &out_json)`
- Response parse: `nostr_nwc_response_parse(json, &client_pub_hex, &req_event_id, &enc, &out_body)`

Clear helpers for parsed bodies:
- `nostr_nwc_request_body_clear(&body)`
- `nostr_nwc_response_body_clear(&body)`

## Encryption negotiation
Use `nostr_nwc_select_encryption(client_supported, client_n, wallet_supported, wallet_n, &enc)`
- Chooses the strongest mutually supported mode.
- Current policy: prefer `nip44-v2`, else `nip04`, else error.

## Sessions
### Client
Header: `nips/nip47/include/nostr/nip47/nwc_client.h`
- `NostrNwcClientSession { wallet_pub_hex, enc }`
- `nostr_nwc_client_session_init(&s, wallet_pub_hex, client_supported, client_n, wallet_supported, wallet_n)`
- `nostr_nwc_client_build_request(&s, &body, &out_json)`
- `nostr_nwc_client_session_clear(&s)`

### Wallet
Header: `nips/nip47/include/nostr/nip47/nwc_wallet.h`
- `NostrNwcWalletSession { client_pub_hex, enc }`
- `nostr_nwc_wallet_session_init(&s, client_pub_hex, wallet_supported, wallet_n, client_supported, client_n)`
- `nostr_nwc_wallet_build_response(&s, req_event_id, &body, &out_json)`
- `nostr_nwc_wallet_session_clear(&s)`

## Crypto helpers (canonical)

Header(s):

- `nips/nip47/include/nostr/nip47/nwc_client.h`
- `nips/nip47/include/nostr/nip47/nwc_wallet.h`

APIs:

- Client-side
  - `nostr_nwc_client_encrypt(const NostrNwcClientSession *s, const char *client_sk_hex, const char *wallet_pub_hex, const char *plaintext, char **out_ciphertext)`
  - `nostr_nwc_client_decrypt(const NostrNwcClientSession *s, const char *client_sk_hex, const char *wallet_pub_hex, const char *ciphertext, char **out_plaintext)`
- Wallet-side
  - `nostr_nwc_wallet_encrypt(const NostrNwcWalletSession *s, const char *wallet_sk_hex, const char *client_pub_hex, const char *plaintext, char **out_ciphertext)`
  - `nostr_nwc_wallet_decrypt(const NostrNwcWalletSession *s, const char *wallet_sk_hex, const char *client_pub_hex, const char *ciphertext, char **out_plaintext)`

Behavior:

- The `s->enc` field selects the encryption scheme: `nip44-v2` (preferred) or `nip04` (fallback).
- For NIP-44 v2, keys are canonical x-only:
  - `client_sk_hex`: 64-hex secret key (32 bytes)
  - `peer_pub_hex`: accepts x-only (64 hex) or SEC1 (33/65); SEC1 inputs are auto-converted to x-only before ECDH/HKDF
- For NIP-04, `peer_pub_hex` may be provided as x-only (64 hex) or SEC1 (33/65 hex):
  - If x-only (64 hex) is given, the helper auto-builds a SEC1-compressed form by trying `0x02||x` and, if needed, falling back to `0x03||x` for ECDH

There are no thin wrappers; these are the canonical `nostr_*` APIs.

## Examples
- Client: `nips/nip47/examples/nwc_client_example.c` (target: `nwc_client_example`)
- Wallet: `nips/nip47/examples/nwc_wallet_example.c` (target: `nwc_wallet_example`)

Both demonstrate negotiation, canonical encrypt/decrypt round-trips in both directions, and request/response building. Run after building:
```
./build/nips/nip47/nwc_client_example
./build/nips/nip47/nwc_wallet_example
```

Expected example behavior (illustrative):

- Negotiate `encryption="nip44-v2"` if both sides support it, else `"nip04"`.
- Show ciphertext and recovered plaintext for client→wallet and wallet→client.
- Print a valid NIP-47 request/response event JSON with `created_at` and `encryption` tag.

## GLib bindings
Headers:
- `nips/nip47/include/nostr/nip47/nwc_client_g.h`
- `nips/nip47/include/nostr/nip47/nwc_wallet_g.h`

Implementation:
- `nips/nip47/src/glib/nwc_client_g.c`
- `nips/nip47/src/glib/nwc_wallet_g.c`

API:
- Client:
  - `gpointer nostr_nwc_client_session_init_g(const gchar *wallet_pub_hex, const gchar **client_supported, gsize client_n, const gchar **wallet_supported, gsize wallet_n, GError **error)`
  - `void nostr_nwc_client_session_free_g(gpointer session)`
  - `gboolean nostr_nwc_client_build_request_g(gpointer session, const gchar *method, const gchar *params_json, gchar **out_event_json, GError **error)`
- Wallet:
  - `gpointer nostr_nwc_wallet_session_init_g(const gchar *client_pub_hex, const gchar **wallet_supported, gsize wallet_n, const gchar **client_supported, gsize client_n, GError **error)`
  - `void nostr_nwc_wallet_session_free_g(gpointer session)`
  - `gboolean nostr_nwc_wallet_build_response_g(gpointer session, const gchar *req_event_id, const gchar *result_type, const gchar *result_json, gchar **out_event_json, GError **error)`

Link target for GLib wrappers: `nostr_nip47_glib`.

## Testing
- Session: `nips/nip47/tests/test_nwc_session.c` → `test_nip47_nwc_session`.
- Envelope/info/URI tests: `test_nip47_nwc_envelope`, `test_nip47_nwc_info`, `test_nip47_nwc_uri`.
- Crypto roundtrip: `nips/nip47/tests/test_nwc_crypto.c` → `test_nip47_nwc_crypto` covers:
  - client→wallet and wallet→client for both `nip44-v2` and `nip04`
  - acceptance of SEC1-compressed peer key for the NIP-44 path

## Notes & invariants
- Kind checks enforced in parse helpers: requests must be 23194, responses 23195.
- Result/params JSON are embedded verbatim; caller is responsible for providing valid JSON strings.
- All canonical APIs are named `nostr_*` with no thin wrappers.
 - NIP-44 convkey uses libsecp256k1 ECDH with x-only public keys and HKDF(Extract/Expand) via OpenSSL EVP_MAC; vectors and roundtrips are covered under sanitizers.

## Roadmap
- Expand examples to cover error envelopes and COUNT.
- Optional: higher-level stateful client with signing and transport.
