# Security Hardening Overview

This document summarizes the parser bounds, rate limiting, logging, and ban controls recently added to the codebase.

## Parser and Structural Limits
- Events
  - Max event JSON size: `NOSTR_MAX_EVENT_SIZE_BYTES` (default 256 KiB)
  - Max tags per event: `NOSTR_MAX_TAGS_PER_EVENT` (default 100)
  - Max tag nesting depth: `NOSTR_MAX_TAG_DEPTH` (default 4)
- Filters
  - Max filters per `REQ`: `NOSTR_MAX_FILTERS_PER_REQ` (default 20)
  - Max IDs per filter: `NOSTR_MAX_IDS_PER_FILTER` (default 500)

Locations:
- `libnostr/src/event.c` (compact tags path limits)
- `libnostr/src/filter.c` (IDs/tags limits)
- `libnostr/src/json.c` (early oversize input rejection)
- Central config: `libnostr/include/security_limits.h`

## WebSocket Ingress Limits
- Hard frame size cap: `NOSTR_MAX_FRAME_LEN_BYTES` (default 2 MiB)
- Token-bucket rate limiters:
  - Frames/sec: `NOSTR_MAX_FRAMES_PER_SEC`
  - Bytes/sec: `NOSTR_MAX_BYTES_PER_SEC`

Location: `libnostr/src/connection.c`

## Invalid Signature Bans
- Sliding window: `NOSTR_INVALID_SIG_WINDOW_SECONDS` (default 60s)
- Threshold to ban: `NOSTR_INVALID_SIG_THRESHOLD` (default 20 fails/window)
- Ban duration: `NOSTR_INVALID_SIG_BAN_SECONDS` (default 5 minutes)

Location: `libnostr/src/relay.c` with tracker state declared in `libnostr/src/relay-private.h`.

Behavior:
- Events from banned pubkeys are dropped early.
- Each invalid signature increments a counter; if threshold is exceeded within the window, a temporary ban is applied.

## Rate-Limited Logging
- API: `nostr_rl_log(level, tag, fmt, ...)` and `SECURE_LOG(tag, fmt, ...)`
- Default window: `NOSTR_LOG_WINDOW_SECONDS=1` second
- Default max per window: `NOSTR_LOG_MAX_PER_WINDOW=50`

Location: `libnostr/include/nostr_log.h`, `libnostr/src/nostr_log.c`

## Metrics
- Counters added in various paths (names are stable but subject to expansion):
  - WS: `ws_tx_bytes`, `ws_rx_bytes`, `ws_rx_messages`
  - JSON: `json_event_oversize_reject`, `json_envelope_oversize_reject`, `json_filter_oversize_reject`, `json_*_compact_ok`, `json_*_compact_fail`, `json_*_backend_used`
  - Verify: `event_verify_count`, `event_verify_ok`, `event_verify_fail`
  - Bans: `event_ban_drop`, `event_invalidsig_record`

## Signer Process Hardening
- Core dumps disabled at startup: `setrlimit(RLIMIT_CORE, 0)`

Location: `apps/gnostr-signer/daemon/main_daemon.c`

## Signer Key Handling (Secure Memory)

- Key resolution now prefers secure buffers for all sensitive operations:
  - `resolve_seckey_secure()` in `nips/nip55l/src/core/signer_ops.c` converts resolved hex (env/libsecret/keychain) into a 32-byte `nostr_secure_buf` and wipes transient hex.
  - Signing uses `nostr_event_sign_secure()` in `libnostr/src/event.c` (declared in `libnostr/include/nostr-event.h`), avoiding hex private keys during signing.
  - NIP-44 encrypt/decrypt and zap-decrypt (NIP-44 branch) use `nostr_secure_buf` for the private key.
  - NIP-04 encrypt/decrypt have secure variants in the NIP module and the signer uses them (see below).

- Zeroization and mlock:
  - `nostr_secure_buf` attempts to allocate locked memory (mlock where available) and guarantees best-effort zeroization via `secure_free()` / `secure_wipe()`.
  - All stack copies of keys and derived materials are wiped explicitly on all paths.

Locations:
- `nips/nip55l/src/core/signer_ops.c`
- `libnostr/src/event.c` and `libnostr/include/nostr-event.h`
- `libnostr/src/secure_buf.c`

## Secure NIP Implementations (kept within NIPs)

- NIP-44 (v2) secure handling (binary private key):
  - Used by signer for `nostr_nip55l_nip44_encrypt/decrypt()` and zap-decrypt path.
  - Location: `nips/nip44` (derivation/HKDF) and `nips/nip55l/src/core/signer_ops.c` (callers).

- NIP-04 secure variants (binary private key):
  - New APIs declared in `nips/nip04/include/nostr/nip04.h`:
    - `nostr_nip04_encrypt_secure(const nostr_secure_buf *sender_seckey, ...)`
    - `nostr_nip04_decrypt_secure(const nostr_secure_buf *receiver_seckey, ...)`
  - Implemented in `nips/nip04/src/nip04.c` with explicit zeroization of ECDH-derived key and IV handling identical to hex path.
  - Signer now calls these secure variants in `nips/nip55l/src/core/signer_ops.c`.

Rationale:
- Keep secure implementations close to their respective NIP modules, while signer orchestrates resolution and lifecycle using secure memory.

## Logging and Secrets Hygiene

- No secret keys are logged. The signer service (`nips/nip55l/src/glib/signer_service_g.c`) only logs:
  - `app_id`, `identity`, `request_id`, and a short preview fragment of `content` (never private keys).
- Internal modules use `secure_wipe()` for temporary secret buffers and scrub hex strings before `free()`.
- Rate-limited logging is available via `nostr_rl_log` to avoid log flooding.

## Notes and Future Work
- Add more RL logs to other high-churn paths as needed.
- Expand counters for queue drops/timeouts and rate-limit closes.
- Add sanitizer/fuzz test targets around JSON→event pipelines.

## TLS Posture (OpenSSL via libwebsockets)

- Versions
  - Minimum: TLS 1.2 (TLS 1.3 preferred automatically during negotiation).
  - Options: compression and renegotiation disabled.

- Cipher suites
  - TLS 1.3: `TLS_AES_128_GCM_SHA256`, `TLS_CHACHA20_POLY1305_SHA256`, `TLS_AES_256_GCM_SHA384`.
  - TLS 1.2 fallback (AEAD only, no CBC/SHA-1/RSA key exchange):
    - `ECDHE-ECDSA-AES128-GCM-SHA256`, `ECDHE-RSA-AES128-GCM-SHA256`.
    - `ECDHE-ECDSA-CHACHA20-POLY1305`, `ECDHE-RSA-CHACHA20-POLY1305`.
    - `ECDHE-ECDSA-AES256-GCM-SHA384`, `ECDHE-RSA-AES256-GCM-SHA384`.

- Key exchange groups
  - `X25519` (preferred), `P-256` fallback.

- Early data (0-RTT)
  - Not enabled to avoid replay risk for WebSocket handshakes.

- Session resumption
  - `SSL_SESS_CACHE_SERVER` enabled; `SSL_CTX_set_num_tickets(ctx, 2)` configured.
  - Recommend rotating ticket keys externally; enable OCSP stapling if serving TLS directly.

- Operational guidance
  - Prefer TLS offload (nginx/caddy/traefik) in front of the relay where possible.
  - If serving TLS directly, mirror the above profile and present ECDSA P-256 certificates (optionally dual-cert ECDSA+RSA for legacy clients).
