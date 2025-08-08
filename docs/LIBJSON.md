# libjson (Jansson interop)

libjson provides a Jansson-backed implementation of the `NostrJsonInterface` declared in `libnostr/include/json.h`.
It serializes/deserializes Nostr events, envelopes, and filters and is built as a shared library: `nostr_json`.

## Registration

- libjson auto-registers itself as the default JSON interface via a constructor in `libjson/src/json.c`:
  - Calls `nostr_set_json_interface(jansson_impl)` so `nostr_event_serialize()` etc. work without manual setup.

## API surface

- `jansson_event_serialize(const NostrEvent*) -> char*`
- `jansson_event_deserialize(NostrEvent*, const char*) -> int`
- `jansson_envelope_serialize(const Envelope*) -> char*`
- `jansson_envelope_deserialize(Envelope*, const char*) -> int`
- `jansson_filter_serialize(const Filter*) -> json_t*` and string wrappers set on the interface
- `jansson_filter_deserialize(Filter*, json_t*)` and string wrappers set on the interface

Consumers use the libnostr wrappers:
- `nostr_event_serialize()` / `nostr_event_deserialize()`
- `nostr_envelope_serialize()` / `nostr_envelope_deserialize()`
- `nostr_filter_serialize()` / `nostr_filter_deserialize()`

## Behavior & invariants

- Events
  - `kind` serialized as integer.
  - `created_at` serialized as integer.
  - Optional fields (`id`, `pubkey`, `content`, `tags`, `sig`) included when present.
  - Deserializer accepts `kind` as int or string, `created_at` as int or string (numeric), others as strings.
- Envelopes (arrays per NIP-01/15)
  - EVENT: ["EVENT", sub_id?, event-object]
  - NOTICE/EOSE/CLOSE: label + message string required.
  - CLOSED: label + subscription id string + reason string required.
  - OK: label + event id string + boolean required; optional reason string allowed.
  - AUTH: label + event-object or challenge string.
- Filters
  - Serialize includes only recognized keys when present: `ids`, `kinds`, `authors`, `since`, `until`, `limit`, `search`.
  - Non-standard keys are omitted by design (e.g., `limit_zero`).
  - `tags` are optional on input. If present, parsed via `jansson_tags_deserialize()`.
  - Deserializer initializes arrays/tags for zeroed `Filter` structs to prevent overflows (`ids`, `kinds`, `authors`, and `tags`).
  - Fields handled on input: `ids`, `kinds`, `authors`, `tags` (array-of-arrays), `since`, `until`, `limit`, `search`, `limit_zero` (boolean flag only; not serialized).

## NIP-01 #tag mapping

- Tags are emitted as dynamic NIP-01 keys: `#<letter>` arrays (e.g., `"#e":["..."], "#p":["..."]`).
  - Source is `Filter.tags`, each tag is a 2-element string array `[name, value]`.
  - Only single-letter names are mapped (e.g., `"e"`, `"p"`). Others are ignored.
- Dynamic `#<letter>` keys are supported. Each string value becomes a tag `[letter, value]` in `Filter.tags`.

## Robustness rules

- `kinds` must be an array of integers. Any non-integer causes failure.
- `ids` and `authors` must be arrays of strings. Any non-string causes failure.
- Dynamic `#<letter>` arrays must contain only strings. Any non-string causes failure.
- Large arrays are handled without overflow.

## Memory ownership

- Filters
  - `create_filter()` → free with `free_filter()`.
  - `Filter.tags`, `Filter.ids`, `Filter.authors`, and `Filter.search` are owned by the filter.
- Envelopes
  - `create_envelope()` → free with `free_envelope()`.
  - If using a stack-allocated envelope for deserialization, free only dynamically allocated members (e.g., `subscription_id`, `event`) and do not call `free_envelope()` on the stack object.
- Events
  - `create_event()` → free with `free_event()`.

## Testing

- Tests cover:
  - Event round-trip including tags.
  - OK envelope boolean strictness and optional reason.
  - CLOSED requires reason.
  - REQ/COUNT: filter arrays parsed into `Filters` using `create_filters()` and `filters_add()`; COUNT `count` defaults to 0 when missing.
  - EVENT envelope with and without subscription id.
  - AUTH envelope supports either challenge string or full event object.
- `test_json_filter.c`: Round-trip and minimal fields.
- `test_json_filter_tags.c`: NIP-01 dynamic `#tag` mapping round-trip.
- `test_json_envelope.c`: Envelope edge cases.
- `test_json_filter_robust.c`: Malformed array types and large array stress.

## CI and sanitizers

- All tests run under ASAN/UBSAN (and optionally TSAN where applicable) on macOS and Linux.
