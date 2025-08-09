## Utils (Thin Wrappers)

- `memhash()` -> `nostr_memhash()`
- `named_lock()` -> `nostr_named_lock()`
- `similar()` -> `nostr_similar()`
- `escape_string()` -> `nostr_escape_string()`
- `are_pointer_values_equal()` -> `nostr_pointer_values_equal()`
- `normalize_url()` -> `nostr_normalize_url()`
- `normalize_ok_message()` -> `nostr_normalize_ok_message()`
- `hex2bin()` -> `nostr_hex2bin()`
- `sub_id_to_serial()` -> `nostr_sub_id_to_serial()`

## Pointers (Thin Wrappers)

- `create_profile_pointer()` -> `nostr_profile_pointer_new()` / `nostr_profile_pointer_free()`
- `create_event_pointer()` -> `nostr_event_pointer_new()` / `nostr_event_pointer_free()`
- `create_entity_pointer()` -> `nostr_entity_pointer_new()` / `nostr_entity_pointer_free()`

## SimplePool (Thin Wrappers)

- `create_simple_pool()` -> `nostr_simple_pool_new()` / `nostr_simple_pool_free()`
- `simple_pool_ensure_relay()` -> `nostr_simple_pool_ensure_relay()`
- `simple_pool_start()` -> `nostr_simple_pool_start()`
- `simple_pool_stop()` -> `nostr_simple_pool_stop()`
- `simple_pool_subscribe()` -> `nostr_simple_pool_subscribe()`
- `simple_pool_query_single()` -> `nostr_simple_pool_query_single()`

Note: Thin wrappers are preferred for bindings. Legacy macros may exist briefly under `NOSTR_ENABLE_LEGACY_ALIASES` but will be removed before release.
Important: JSON-layer functions declared in `nostr-json.h`/`json.h` — such as `nostr_event_serialize()`, `nostr_event_deserialize()`, and their envelope/filter counterparts — are not and will not be remapped by legacy macros. This avoids conflicts with prototypes consumed by GI and downstreams.
See also: `docs/GLIB_INTEGRATION.md` for GLib provider, GI annotations, and usage notes.

# libnostr API Rename Map (Work-In-Progress)
## Envelope Accessors (New Public API)

Defined in `libnostr/include/nostr-envelope.h`, implemented in `libnostr/src/envelope.c`.

- `nostr_envelope_get_type(const NostrEnvelope *env)` → returns `NostrEnvelopeType`
- EVENT: `nostr_event_envelope_get_subscription_id`, `nostr_event_envelope_get_event`
- REQ: `nostr_req_envelope_get_subscription_id`, `nostr_req_envelope_get_filters`
- COUNT: `nostr_count_envelope_get_subscription_id`, `nostr_count_envelope_get_filters`, `nostr_count_envelope_get_count`
- NOTICE: `nostr_notice_envelope_get_message`
- EOSE: `nostr_eose_envelope_get_message`
- CLOSE: `nostr_close_envelope_get_message`
- CLOSED: `nostr_closed_envelope_get_subscription_id`, `nostr_closed_envelope_get_reason`
- OK: `nostr_ok_envelope_get_event_id`, `nostr_ok_envelope_get_ok`, `nostr_ok_envelope_get_reason`
- AUTH: `nostr_auth_envelope_get_challenge`, `nostr_auth_envelope_get_event`

GI notes: pointer returns are (transfer none) (nullable); events/filters are owned by the envelope unless otherwise documented.

## Timestamp and Keys (Transitional)

- `nostr-timestamp.h`: aliases
  - `Timestamp` → `NostrTimestamp`
  - `Now()` → `nostr_timestamp_now()`
  - `TimestampToTime()` → `nostr_timestamp_to_time()`

- `nostr-keys.h`: aliases
  - `generate_private_key()` → `nostr_key_generate_private()`
  - `get_public_key()` → `nostr_key_get_public()`
  - `is_valid_public_key_hex()` → `nostr_key_is_valid_public_hex()`
  - `is_valid_public_key()` → `nostr_key_is_valid_public()`

## RelayStore / MultiStore Functions
- create_multi_store() -> nostr_multi_store_new()
- free_multi_store() -> nostr_multi_store_free()
- multi_store_publish() -> nostr_multi_store_publish()
- multi_store_query_sync() -> nostr_multi_store_query_sync()

### RelayStore / MultiStore Accessors (New Public API)

Defined in `libnostr/include/nostr-relay-store.h` and implemented in `libnostr/src/relay_store.c`:

- `nostr_multi_store_get_count(const NostrMultiStore *multi)`
  - Returns: number of stores
- `nostr_multi_store_get_nth(const NostrMultiStore *multi, size_t index)`
  - Returns: (transfer none) (nullable) store pointer at index


This document tracks public symbol renames as part of the GLib/GTK+-ready refactor.
Compatibility macros may temporarily exist behind `NOSTR_ENABLE_LEGACY_ALIASES`,
which will be removed before release.

## Types
- Relay -> NostrRelay
- Subscription -> NostrSubscription
- Filter -> NostrFilter
- Filters -> NostrFilters
- Tags -> NostrTags
- Connection -> NostrConnection
- EnvelopeType -> NostrEnvelopeType (values: NOSTR_ENVELOPE_*)
- NostrEvent (unchanged prefix already correct)

## Relay Functions
- new_relay() -> nostr_relay_new()
- free_relay() -> nostr_relay_free()
- relay_connect() -> nostr_relay_connect()
- relay_disconnect() -> nostr_relay_disconnect()
- relay_close() -> nostr_relay_close()
- relay_subscribe() -> nostr_relay_subscribe()
- relay_prepare_subscription() -> nostr_relay_prepare_subscription()
- relay_publish() -> nostr_relay_publish()
- relay_auth() -> nostr_relay_auth()
- relay_count() -> nostr_relay_count()
- relay_is_connected() -> nostr_relay_is_connected()

### Relay Accessors (New Public API)

Defined in `libnostr/include/nostr-relay.h` and implemented in `libnostr/src/relay.c`:

- `nostr_relay_get_url_const(const NostrRelay *relay)`
  - Returns: internal URL string
  - GI: (transfer none) (nullable)
- `nostr_relay_get_context(const NostrRelay *relay)`
  - Returns: connection GoContext used for worker lifecycles
  - GI: (transfer none) (nullable)
- `nostr_relay_get_write_channel(const NostrRelay *relay)`
  - Returns: internal write queue channel; owned by relay
  - GI: (transfer none) (nullable)
- `nostr_relay_enable_debug_raw(NostrRelay *relay, int enable)` and `nostr_relay_get_debug_raw_channel(NostrRelay *relay)`
  - When enabled, returns a relay-owned channel that emits concise envelope summaries
  - GI: (transfer none) (nullable)

Notes:
- All getters are NULL-tolerant and return NULL when appropriate.
- All returned pointers are borrowed; do not free them.

## Subscription Functions
- create_subscription() -> nostr_subscription_new()
- free_subscription() -> nostr_subscription_free()
- subscription_fire() -> nostr_subscription_fire()
- subscription_unsub() -> nostr_subscription_unsubscribe()
- subscription_close() -> nostr_subscription_close()
- subscription_get_id() -> nostr_subscription_get_id()

### Subscription Accessors (New Public API)

The following GLib-style accessors are provided in `libnostr/include/nostr-subscription.h` and implemented in `libnostr/src/subscription.c`:

- `nostr_subscription_get_id_const(const NostrSubscription *sub)`
  - Returns: internal ID string
  - GI: (transfer none) (nullable)
- `nostr_subscription_get_relay(const NostrSubscription *sub)`
  - Returns: associated relay
  - GI: (transfer none) (nullable)
- `nostr_subscription_get_filters(const NostrSubscription *sub)`
  - Returns: associated filters
  - GI: (transfer none) (nullable)
- `nostr_subscription_set_filters(NostrSubscription *sub, NostrFilters *filters)`
  - Ownership: takes full ownership of `filters`; frees previous if different
  - GI: @filters: (transfer full) (nullable)
- `nostr_subscription_get_events_channel(const NostrSubscription *sub)`
  - Returns: event queue channel (owned by subscription)
  - GI: (transfer none) (nullable)
- `nostr_subscription_get_eose_channel(const NostrSubscription *sub)`
  - Returns: EOSE notify channel (owned by subscription)
  - GI: (transfer none) (nullable)
- `nostr_subscription_get_closed_channel(const NostrSubscription *sub)`
  - Returns: CLOSED reason channel (owned by subscription), carries `const char *`
  - GI: (transfer none) (nullable)
- `nostr_subscription_get_context(const NostrSubscription *sub)`
  - Returns: cancellation context for lifecycle
  - GI: (transfer none) (nullable)
- `nostr_subscription_is_live(const NostrSubscription *sub)`
- `nostr_subscription_is_eosed(const NostrSubscription *sub)`
- `nostr_subscription_is_closed(const NostrSubscription *sub)`

Notes:
- All getters tolerate `NULL` and return sensible defaults or NULL.
- Channels are returned as borrowed pointers; do not free them. Subscription owns and frees on `nostr_subscription_free()`.
- Minimal GTK-Doc/GI annotations are present to support future GObject/introspection.

## Event/Filter Functions
- create_event() -> nostr_event_new()
- free_event() -> nostr_event_free()
- create_filter() -> nostr_filter_new()
- free_filter() -> nostr_filter_free()

### Event Extra Helpers (Thin Wrappers)

- `set_extra()` -> `nostr_event_set_extra()`
- `remove_extra()` -> `nostr_event_remove_extra()`
- `get_extra()` -> `nostr_event_get_extra()`
- `get_extra_string()` -> `nostr_event_get_extra_string()`
- `get_extra_number()` -> `nostr_event_get_extra_number()`
- `get_extra_boolean()` -> `nostr_event_get_extra_bool()`

Notes: These are real functions (not macros) exported for GI-friendly bindings.

## JSON Interface (kept)
- nostr_set_json_interface()
- nostr_json_init()
- nostr_json_cleanup()

## Connection Functions
- new_connection() -> nostr_connection_new()
- connection_close() -> nostr_connection_close()
- connection_write_message() -> nostr_connection_write_message()
- connection_read_message() -> nostr_connection_read_message()

### Connection Accessors (New Public API)

Defined in `libnostr/include/nostr-connection.h` and implemented in `libnostr/src/connection.c`:

- `nostr_connection_get_send_channel(const NostrConnection *conn)`
  - Returns: internal send channel; owned by connection
  - GI: (transfer none) (nullable)
- `nostr_connection_get_recv_channel(const NostrConnection *conn)`
  - Returns: internal receive channel; owned by connection
  - GI: (transfer none) (nullable)
- `nostr_connection_is_running(const NostrConnection *conn)`
  - Returns: whether the background service thread is running (false in test mode)

## Notes
- Add GLib/GError variants where appropriate in `nostr-glib.h` during Phase 3.
- Hyphenated header renames will follow (e.g., `nostr-relay.h`, `nostr-event.h`).
- Update examples and tests as each module is migrated.
