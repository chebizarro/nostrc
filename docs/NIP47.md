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

## Examples
- Client: `nips/nip47/examples/nwc_client_example.c` (target: `nwc_client_example`)
- Wallet: `nips/nip47/examples/nwc_wallet_example.c` (target: `nwc_wallet_example`)

Both demonstrate negotiation and request/response building. Run after building:
```
./build/nips/nip47/nwc_client_example
./build/nips/nip47/nwc_wallet_example
```

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
- Session test: `nips/nip47/tests/test_nwc_session.c` → `test_nip47_nwc_session` (passes under sanitizers).
- Envelope tests: `test_nip47_nwc_envelope`, info/URI tests also included.

## Notes & invariants
- Kind checks enforced in parse helpers: requests must be 23194, responses 23195.
- Result/params JSON are embedded verbatim; caller is responsible for providing valid JSON strings.
- All canonical APIs are named `nostr_*` with no thin wrappers.

## Roadmap
- Expand examples to cover error envelopes and COUNT.
- Optional: higher-level stateful client with signing and transport.
