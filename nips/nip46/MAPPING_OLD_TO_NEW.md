# NIP-46 Pre-existing → New Layout/Symbols Mapping

This document inventories the existing NIP-46 attempt and maps files/symbols to the new per-NIP structure and API per ADR-0001 and guardrails.

Spec source: see `SPEC_SOURCE` → `../../docs/nips/46.md`.

## Inventory (pre-existing)

- include/nip46.h
- src/nip46.c
- src/dynamic_signer.c
- src/static_key_signer.c
- src/well_known_nostr.c
- CMakeLists.txt
- examples/example.c

## Header (old → new)

- include/nip46.h → include/nostr/nip46/nip46.h
  - Types:
    - nip46_request_t → NostrNip46Request
    - nip46_response_t → NostrNip46Response
    - nip46_session_t → NostrNip46Session (opaque)
    - nip46_relay_readwrite_t → Removed or folded into URI helpers (if needed), otherwise move to `nip46_uri.h`.
    - nip46_bunker_client_t → Split into explicit Client/Bunker session structs
  - Functions:
    - nip46_is_valid_bunker_url → nostr_nip46_uri_is_bunker
    - nip46_create_session → nostr_nip46_client_new / bunker_new (separate roles)
    - nip46_parse_request → nostr_nip46_envelope_parse_request
    - nip46_make_response → nostr_nip46_envelope_make_response
    - nip46_connect_bunker → nostr_nip46_client_connect (bunker://)
    - nip46_new_bunker → nostr_nip46_bunker_new
    - nip46_rpc → nostr_nip46_client_rpc (internal); public typed entries below
    - nip46_get_public_key → nostr_nip46_client_get_public_key
    - nip46_sign_event → nostr_nip46_client_sign_event

New public API will be split by role and documented in `include/nostr/nip46/`:

- nip46_types.h: constants, enums, structs (Request/Response), errors
- nip46_uri.h: bunker:// and nostrconnect:// tokens parse/build
- nip46_client.h: client session APIs (connect, get_public_key, sign_event, nip04/44, ping)
- nip46_bunker.h: bunker session APIs (listen, callbacks, issue_bunker_uri, reply)

## Sources (old → new)

- src/nip46.c → src/core/nip46_session.c + src/core/nip46_envelope.c
- src/dynamic_signer.c → src/core/nip46_bunker.c (handlers; integrates with callbacks)
- src/static_key_signer.c → src/core/nip46_client_crypto.c (adapters that call existing libnostr NIP-04/44 helpers)
- src/well_known_nostr.c → src/core/nip46_uri.c (URI parsing/issuance; any .well-known lookup becomes an adapter that uses existing DNS/NIP-05 if present; otherwise TODO)

GLib layer (new):

- src/glib/nip46_client_g.c: GObject `NostrNip46Client`
- src/glib/nip46_bunker_g.c: GObject `NostrNip46Bunker`

## Symbol migrations summary

- Requests/Responses keep JSON shapes but move to libnostr JSON interface builders/parsers. No hand-rolled JSON.
- `nip46_bunker_client_t` responsibilities split:
  - network I/O via `NostrRelay`/`NostrClient`
  - permissions CSV helper module
  - session state and outstanding request map

## Tests & Examples

- examples/example.c → examples/nostr-nip46-client (CLI) and examples/nostr-nip46-bunker (CLI)
- Add unit tests under tests/ for:
  - URI parse/build
  - Envelope build/parse (kind 24133, p-tag)
  - Handshake flows (bunker:// and nostrconnect://)
  - Permissions and errors
  - NIP-44 payload encrypt/decrypt (via adapters)

## Open TODOs/Decisions

- Define exact error taxonomy: NOSTR_ERROR_NIP46_* in shared error space; adapters return NostrError* or int + Error*
- Confirm whether `well_known_nostr.c` is kept; if it performs discovery, wrap or mark TODO with spec citation.
- Verify constant-time secret compare helper (in core) and zeroization of secrets.

All new files live under `nips/nip46/` per guardrails; link against libnostr core/GLib only.
