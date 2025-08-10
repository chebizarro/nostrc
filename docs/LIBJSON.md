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

## Array-of-object helpers

Some NIPs use arrays of objects nested under an object key (e.g., NIP-11 `fees`). libnostr exposes convenience helpers to access these safely by index without leaking implementation details:

- `nostr_json_get_array_length_at(json, object_key, entry_key, out_len)`
- `nostr_json_get_int_in_object_array_at(json, object_key, entry_key, index, field_key, out_val)`
- `nostr_json_get_string_in_object_array_at(json, object_key, entry_key, index, field_key, out_str)`
- `nostr_json_get_int_array_in_object_array_at(json, object_key, entry_key, index, field_key, out_items, out_count)`

Behavior:
- All functions return 0 on success, -1 on failure or type mismatch.
- For string results, the callee allocates with `strdup`; the caller must free.
- For int array results, the callee allocates; the caller must free the array.

Example (NIP-11 fees publication kinds):
```c
size_t len = 0; // number of publication fee entries
if (nostr_json_get_array_length_at(json, "fees", "publication", &len) == 0 && len > 0) {
    int *kinds = NULL; size_t kinds_n = 0;
    if (nostr_json_get_int_array_in_object_array_at(json, "fees", "publication", 0, "kinds", &kinds, &kinds_n) == 0) {
        // use kinds[0..kinds_n)
        free(kinds);
    }
}
```

## NIP-11 usage

The NIP-11 module (`nips/nip11/`) consumes the JSON interface exclusively (no direct Jansson) to parse Relay Info Documents:

- Strings: `nostr_json_get_string()` for `name`, `description`, `software`, `version`, `posting_policy`, `payments_url`, `icon`.
- Int arrays: `nostr_json_get_int_array()` for `supported_nips`.
- String arrays: `nostr_json_get_string_array()` for `relay_countries`, `language_tags`, `tags`.
- Arrays-of-objects: helpers above for `fees.admission[]`, `fees.subscription[]`, and `fees.publication[]`.

Memory ownership: the NIP-11 free function `nostr_nip11_free_info()` performs deep-free of all nested arrays and strings.
