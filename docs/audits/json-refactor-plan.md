# GNostr / libnostr — Canonical JSON Facade Audit (Phase 0)

This document inventories all direct JSON library usages (Jansson and others) outside the backend, and proposes the exact replacement via the canonical JSON facade.

Authoritative facades:
- C: `libnostr/include/json.h` (`NostrJsonInterface`, `nostr_*serialize/deserialize`, `nostr_json_get_*`)
- GLib: `libnostr/include/nostr-json.h` (provider bridge)

Non‑negotiables:
- No module outside the JSON backend may include or call Jansson (or any other JSON lib) directly.
- Use only the C facade or the GLib provider bridge.
- Fail fast; no ad-hoc fallbacks.

---

## 1) Inventory of forbidden includes/usages

Legend: [I] include, [S] symbol usage.

### A) libnostr/ (core + nips)

- `libnostr/src/json.c` — [I][S]
  - Direct Jansson construction/parsing and helpers. Must be split: router/helpers stay; Jansson backend moves to `json_backend_libjson.c`.
- `libnostr/src/event_extra.c` — [I][S]
  - Stores/reads extra fields via `json_object_get`. Must route through facade or typed accessors.

NIPs (core submodules):
- `nips/nip46/src/core/nip46_msg.c` — [I][S]
  - Builds/parses NIP-46 messages using Jansson. Replace with event/envelope helpers or facade getters.
- `nips/nip47/src/core/nwc_envelope.c` — [I][S]
  - Parses/serializes RPC-like envelopes; uses `json_loads`, `json_dumps`.
- `nips/nip52/src/nip52.c` — [I][S]
  - Parses/serializes via `json_loads`, `json_dumps`.
- `nips/nip53/src/nip53.c` — [I][S]
  - Event JSON parsing via `json_loads`.
- `nips/nip5f/src/core/sock_conn.c` — [I][S]
  - Server-side request parse/emit with Jansson (`json_loads`, `json_dumps`).

Tests under nips:
- `nips/nip44/tests/test_nip44_vectors.c` — [I][S]
  - Loads JSON test vectors via Jansson. Tests should switch to facade helpers for parsing, or internal helper specific to test that uses the backend through facade.

### B) apps/

- `apps/gnostr/src/ui/gnostr-main-window.c` — [I][S]
  - Builds filter arrays and parses profile content using Jansson in UI layer. Must call `nostr_filter_serialize()` and `nostr_json_get_*()` helpers instead.

### C) examples/

- `examples/json_glib.c` — [S] (GLib Json-GLib)
  - Uses Json-GLib directly. Should be reworked to demonstrate `nostr-json.h` provider or use C facade; direct Json-GLib is not allowed outside provider bridge.

### D) tests/

- `tests/test_event_extra.c` — [I][S]
  - Uses Jansson for convenience; should instead exercise `nostr_*` facade or local test utility that routes through the backend facade.

### E) Backend (allowed)

- `libjson/src/json.c` — [I][S]
  - This is the Jansson-based backend. It is the only place that may include `<jansson.h>` alongside the new backend file.

> Note: The above inventory is based on repository-wide searches for `#include <jansson.h>`, `json_*` construction/parsing functions (`json_load*`, `json_dumps`, `json_object_get`, array ops, etc.), and alternative JSON libs. No usage of cJSON/yyjson/rapidjson/nlohmann was found. One example uses Json-GLib and must be migrated to the GLib provider bridge.

---

## 2) Call-site purpose table and exact replacements

For each area, the replacement must use the C facade (`nostr_*serialize/deserialize`, `nostr_json_get_*`) or install a GLib provider which bridges into the C facade.

### libnostr/src/json.c (split responsibilities)
- Purpose: Implements event/envelope/filter (de)serialization + generic helpers.
- Replacement plan:
  - Move all direct Jansson usage into `libnostr/src/json_backend_libjson.c`.
  - Keep `json.c` hosting only:
    - `NostrJsonInterface *json_interface` storage, `nostr_set_json_interface()`.
    - Thin forwarders: `nostr_event_serialize()`, `nostr_event_deserialize()`, `nostr_envelope_*()`, `nostr_filter_*()`.
    - Backend-agnostic helpers implemented via `json_interface` calls (never `<jansson.h>`).

### libnostr/src/event_extra.c
- Purpose: Accessor for vendor-specific fields.
- Replacement:
  - Remove direct `json_object_get`/storage of `json_t`.
  - Store extra as serialized JSON string or a small internal struct; provide typed getters via `nostr_json_get_*()`.

### nips/*
- nip46_msg.c — builds/parses NIP-46 JSON messages.
  - Replace Jansson nodes with struct population and `nostr_envelope_serialize()` or a dedicated facade helper if format is not a nostr event/envelope.
  - For field extraction from JSON strings, use `nostr_json_get_string(_at)`, `nostr_json_get_int(_at)`, arrays via `nostr_json_get_*array*`.
- nwc_envelope.c (NIP-47)
  - Use `nostr_envelope_serialize/deserialize()` for envelope shapes; for params/results inside content, add facade helpers if needed.
- nip52.c / nip53.c
  - Route all parsing via `nostr_event_deserialize()`; content subfields via `nostr_json_get_*()`.
- nip5f sock_conn.c
  - Replace direct `json_loads/dumps` with facade getters and serializers. If shape is not Event/Envelope/Filter, add a small facade function (Phase 4) implemented in backend.

### apps/gnostr/src/ui/gnostr-main-window.c
- Purpose: Build filters, parse profile JSON.
- Replace:
  - Filter building: create `NostrFilter` and call `nostr_filter_serialize()`; do not assemble JSON by hand.
  - Profile parsing: use `nostr_json_get_string()`/`_at()` (e.g., `display_name`, `name`, `username`, `picture`).

### examples/json_glib.c
- Purpose: Demonstrate JSON parsing.
- Replace:
  - Either: demonstrate `nostr_json_provider_install()` usage, mapping provider vfuncs to Json-GLib, then consume via C facade.
  - Or: replace with a C-facade example and move provider demo to a new `examples/json_provider_glib.c`.

### tests/* (including nip44 vectors)
- Purpose: Load/inspect JSON.
- Replace:
  - Use facade functions to parse vectors where possible.
  - For arbitrary JSON in vectors, add small test-only helpers that call the facade generic getters (no Jansson includes).

---

## 3) Next steps (Phase 1 preview; no edits yet)

- Create `libnostr/src/json_backend_libjson.c` and move all direct Jansson code from `libnostr/src/json.c`.
- Ensure `libnostr/src/json.c` references no Jansson types or headers.
- Add CMake option `-DLIBNOSTR_JSON_BACKEND=libjson|flatcc` and only compile the selected backend.
- Provide `nostr_json_backend_libjson_install()` which installs a static `NostrJsonInterface` into the router.

---

## 4) CI guard idea

- Add a CI script/target scanning for forbidden patterns (`#include <jansson.h>`, `json_t`, `json_loads`, `json_dumps`, etc.) outside of the backend file(s) and GLib provider file when enabled.

---

## 5) Notes

- 64-bit integer handling must be lossless across backends; avoid double coercion.
- Deterministic serialization is required for any signed preimage.
- Helpers like `nostr_json_get_int_array[_at]` have strict numeric semantics and always return non-NULL buffers on success (even when empty); callers must free.
