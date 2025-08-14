# NIP Tag Helpers Audit (Phase 0)

Scope: NIP-01, NIP-02, NIP-10, NIP-31 across libnostr core, NIP modules, tests, and examples.

Ground rules reflected:
- All NIP logic stays under `libnostr/nips/nipXX/` with public headers in `libnostr/include/nips/`.
- JSON usage must go through the canonical facade (`libnostr/include/json.h`). No direct `jansson` or other JSON APIs in NIPs.
- Replace hand-built tag logic and ad-hoc kind checks with typed helpers.

---

## 1) Inventory of current usage sites

This section lists where tags `e`, `p`, `a`, `alt`, `q` are manipulated, where kind classes are checked ad‑hoc, and where direct JSON APIs touch event tags/content. Paths are relative to repo root.

### A. Tag construction/inspection (manual)

- NIP-10: marked and positional e-tags parsing
  - `nips/nip10/src/nip10.c`
    - Checks `tag->data[0]` for "e"/"a" via `strcmp` in multiple loops.
    - Interprets positional semantics and markers.
- Tag utilities handling `e`/`p` specially
  - `libnostr/src/tag.c`
    - Special-cases when tag key is `"e"` or `"p"` (`nostr_tag_size`, `nostr_tag_get`).
- Examples/tests building `e`/`p` tags directly
  - `nips/nip10/examples/example.c` — constructs `e` tags with and without markers.
  - `tests/test_json_event.c` — builds `e` and `p` via `nostr_tag_new()`.
  - `tests/test_json_filter.c`, `tests/test_json_filter_tags.c` — create `e`/`p` tags.
  - `nips/nip47/src/core/nwc_envelope.c` — creates `"e"` and `"p"` tags for routing.
  - `nips/nip29/src/nip29.c` — uses `nostr_event_add_tag(..., "p", ...)` and matches key `"p"`.
- `a` tags
  - `nips/nip10/src/nip10.c` — accepts either `"e"` or `"a"` while scanning.
  - `nips/nip34/src/nip34.c` — checks event tags for `"a"`.
- `alt` tag (NIP‑31)
  - `nips/nip31/include/nip31.h` and `nips/nip31/src/nip31.c` — provide `nostr_get_alt()`; example in `nips/nip31/examples/example.c` builds `"alt"` via `nostr_event_add_tag`.
- `q` tag
  - No direct usage found in repo search (to be covered by helpers for completeness).

### B. Ad‑hoc kind class checks

- Core predicates already exist in libnostr (good centralization):
  - `libnostr/src/event.c`
    - `nostr_event_is_replaceable()` → kind `== 0 || == 3 || [10000,20000)`
    - `event_is_addressable()` → kind in `[30000,40000)`
    - `nostr_event_is_ephemeral()` → kind in `[20000,30000)`
- Other scattered kind checks exist in UI/examples (informational — outside helpers scope):
  - `apps/gnostr/src/ui/gnostr-main-window.c` — comments and conditionals on `kind` values.

### C. Direct JSON calls in NIPs touching tags/content (needs refactor to facade)

- `nips/nip53/src/nip53.c` — includes `<jansson.h>` and accesses `tags` via `json_object_get(root, "tags")`.
- `nips/nip52/src/nip52.c` — includes `<jansson.h>`; constructs `p` tags via `json_pack`.
- `nips/nip47/src/core/nwc_envelope.c` — includes `<jansson.h>` and uses JSON for envelope composition; also creates `"p"` tags.
- `nips/nip5f/src/core/sock_conn.c` — includes `<jansson.h>` (network framing; verify scope but should still use facade).

Notes:
- Backend JSON adapter `libnostr/src/json_backend_libjson.c` is allowed to include Jansson; it's the backend for the facade.
- UI app `apps/gnostr/...` includes Jansson — acceptable for app layer, but out of scope for NIP helpers.

---

## 2) Call‑site intent → proposed helper API mapping

This lists the desired behavior observed and maps it to the Phase 1 header APIs that will be introduced.

- NIP‑10 thread context and reply semantics
  - Intent: Mark and parse e‑tags with `root`/`reply` markers; fallback to positional if unmarked; include participants.
  - Mapping:
    - `nostr_nip10_add_marked_e_tag(...)` to add typed e‑tag with optional marker and relay.
    - `nostr_nip10_get_thread(...)` to extract `{root, reply}` IDs, preferring markers.
    - `nostr_nip10_ensure_p_participants(reply_ev, parent_ev)` to union parent author + parent `p` tags (dedup).

- Basic `e`/`p`/`a` tag construction
  - Intent: Build tags without manual arrays/`strcmp`.
  - Mapping:
    - `nostr_nip01_add_e_tag(ev, event_id, relay_opt, author_pk_opt)`.
    - `nostr_nip01_add_p_tag(ev, pubkey, relay_opt)`.
    - `nostr_nip01_add_a_tag(ev, kind, pubkey, d_tag_opt, relay_opt)` with proper `a` formatting and trailing `:` when `d` empty.

- `alt` tag management (NIP‑31)
  - Intent: Set/get a single `alt` summary string.
  - Mapping:
    - `nostr_nip31_set_alt(ev, alt)` (replace if present).
    - `nostr_nip31_get_alt(ev, &out)` and convenience `nostr_nip01_get_alt(...)`.

- Filter construction for `#e/#p/#a/ids/authors/kinds/since/until/limit`
  - Intent: Avoid ad‑hoc filter JSON; provide deterministic, typed builders.
  - Mapping:
    - `NostrFilterBuilder` with `nostr_nip01_filter_*` functions and `nostr_nip01_filter_build(..., NostrFilter*)`.

- Kind class checks centralization
  - Intent: Single source of truth for replaceable/addressable/ephemeral logic.
  - Mapping:
    - `nostr_nip01_is_replaceable(int kind)`, `nostr_nip01_is_addressable(int kind)`, `nostr_nip01_is_ephemeral(int kind)` (mirroring existing core semantics for non‑invasive usage by callers that only have kind).

- NIP‑02 follow list (kind 3)
  - Intent: Build/parse/append follow lists with relay and petname fields; dedup by pubkey, preserve order.
  - Mapping:
    - `nostr_nip02_build_follow_list(...)`, `nostr_nip02_parse_follow_list(...)`, `nostr_nip02_append(...)`, plus `nostr_nip02_free_follow_list(...)`.

---

## 3) Refactor targets (high‑priority)

- Remove direct Jansson usage in NIPs and switch to facade:
  - `nips/nip53/src/nip53.c` — replace `json_*` calls with event/tag helpers and facade getters.
  - `nips/nip52/src/nip52.c` — replace `json_pack` construction of `p` tags with typed tag builders.
  - `nips/nip47/src/core/nwc_envelope.c` — refactor envelope JSON composition to use facade serialization and `nostr_tag_new`/helpers.
  - `nips/nip5f/src/core/sock_conn.c` — audit and move any JSON logic to facade.

- Replace hand manipulation with helpers at call sites:
  - `nips/nip10/src/nip10.c` — switch scanning/strcmp to `nostr_nip10_get_thread()` and typed tag addition.
  - Tests/examples building `e/p/a/alt` via raw arrays should use helpers once available for golden determinism.

---

## 4) Risks & edge cases to cover in helpers/tests

- `a` tag formatting: ensure trailing `:` when `d` is empty per spec.
- NIP‑10 precedence: when both marked and positional present, prefer marked and ignore positional ambiguity.
- `p` participant union: dedup by pubkey, preserve stable order (parent author first, then existing unique `p`).
- `alt` handling: single tag only — replace existing; UTF‑8 content.
- Filters: stable/compact JSON output; arrays must be deterministic and validated.
- Error codes: all helpers return negative on invalid inputs (NULLs, wrong kind, malformed tags).

---

## 5) Next steps

- Phase 1 — add headers only (no implementation):
  - `libnostr/include/nips/nip01.h`, `nip02.h`, `nip10.h`, `nip31.h`.
  - Optional GLib wrappers headers under `libnostr/include/nips-glib/` for GI.
- After headers are in place, proceed to Phase 2 implementations with facade‑only JSON usage.

