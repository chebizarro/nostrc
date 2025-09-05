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

## Notes and Future Work
- Add more RL logs to other high-churn paths as needed.
- Expand counters for queue drops/timeouts and rate-limit closes.
- Add sanitizer/fuzz test targets around JSON→event pipelines.
