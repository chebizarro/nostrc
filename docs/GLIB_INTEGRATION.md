# GLib/GObject Integration

This document explains how libnostr integrates with GLib/GObject for a pluggable JSON provider and how to consume it from GLib-friendly code and language bindings via GObject Introspection (GI).

## Build options

- NOSTR_WITH_GLIB=ON: Enables optional GLib code in libnostr (default: auto if GLib found).
- Defines NOSTR_HAVE_GLIB at compile time for conditional code.
- CMake links GLib transitively on the `nostr` target so downstreams inherit it automatically.

## Generating GIR/typelib

You can generate a Nostr GObject Introspection file (`Nostr-1.0.gir`) and its typelib (`Nostr-1.0.typelib`) via a dedicated CMake target.

Steps:

1. Configure and build with GLib enabled:

   ```sh
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DNOSTR_WITH_GLIB=ON
   cmake --build build -j
   ```

2. Generate GIR/typelib:

   ```sh
   cmake --build build --target nostr-gir -j
   ```

Artifacts are written to `build/gir/Nostr-1.0.gir` and `build/gir/Nostr-1.0.typelib`.

Notes:

- The `nostr-gir` target runs `g-ir-scanner` and `g-ir-compiler` with include paths from `libnostr/include/` and the generated `libnostr/generated/` directory. It links against the in-tree built `nostr` and `nostr_json` libraries.
- Ensure `gobject-introspection` is installed and discoverable (`g-ir-scanner`, `g-ir-compiler`).

## Pkg-config

Both libraries provide pkg-config files after install:

- `nostr.pc` (core library)
- `nostr_json.pc` (JSON layer)

Example usage:

```sh
pkg-config --cflags --libs nostr nostr_json
```

## JSON provider (GInterface)

- GInterface name: `NostrJsonProviderInterface` declared in `libnostr/include/nostr-json.h`.
- Bridge file: `libnostr/src/nostr_json_glib.c` registers the interface (manual, no G_DECLARE_INTERFACE) and connects it to the existing C JSON interface (`NostrJsonInterface` from `json.h`).
- Install/remove functions:
  - `nostr_json_provider_install(gpointer provider)`
  - `nostr_json_provider_uninstall(void)`

### Vfuncs

Implement any subset; missing vfuncs fall back to NULL and will return failure from the bridge:

- `char *(*serialize_event)(const NostrEvent *event);`
- `int   (*deserialize_event)(NostrEvent *event, const char *json);`
- `char *(*serialize_envelope)(const Envelope *envelope);`
- `int   (*deserialize_envelope)(Envelope *envelope, const char *json);`
- `char *(*serialize_filter)(const Filter *filter);`
- `int   (*deserialize_filter)(Filter *filter, const char *json);`

All returned strings are (transfer full) and must be freed by the caller with `free()`.

### Installing a provider (C)

```c
#include <glib-object.h>
#include "nostr-json.h"

// Suppose MyProviderType is a GObject that implements NostrJsonProviderInterface
MyProvider *p = my_provider_new();
nostr_json_provider_install(p);
// libnostr now routes nostr_event_serialize/deserialize through your provider

// When done
nostr_json_provider_uninstall();
g_object_unref(p);
```

Notes:
- The interface is registered manually to avoid macro pitfalls and ease GI.
- Thread-safe install/uninstall: libnostr maintains a single strong ref to the active provider.
- JSON-layer functions declared in `nostr-json.h`/`json.h` (e.g., `nostr_event_serialize()` and deserialize counterparts) are not subject to any legacy macro remapping. This is intentional to keep prototypes stable for GI extraction and downstream usage.

## GI/GTK-Doc annotations

Public headers under `libnostr/include/` include transitional `nostr-*` headers with GTK-Doc/GI annotations for ownership/transfer and out parameters. Examples:

- `nostr-envelope.h`, `nostr-event.h`, `nostr-filter.h`
- `nostr-pointer.h`, `nostr-utils.h`, `nostr-event-extra.h`, `nostr-simple-pool.h`
- `nostr-json.h` (both C API annotations and the GInterface declarations)

These annotations enable language bindings (e.g., Python via PyGObject) to infer correct memory ownership and parameter behavior.

## Troubleshooting

- Ensure your build finds GLib (pkg-config gobject-2.0). If not present, the optional provider API is compiled out, but the base C JSON interface remains available.
- If you see warnings around GType/class init, ensure you’re compiling with the current sources; the code uses a wrapper `GClassInitFunc` to avoid pointer cast warnings and compatible `g_once` usage.
- When switching providers, always call `nostr_json_provider_uninstall()` before unref’ing your provider object to avoid dangling pointers.

## Examples

- See `examples/json_glib.c` for a JSON-GLib based demonstration of serialize/deserialize logic.
- See `examples/relay_smoke.c` for an end-to-end relay usage example with CLI flags.

## Boxed types and GI-friendly wrappers

### NostrFilter (GBoxed) and GI wrappers

- Boxed registration: `nostr_filter_get_type()`; type name `NostrFilter`.
- Deep copy/free: `nostr_filter_copy()` / `nostr_filter_free()`; used by the boxed type.
- For GI, avoid exposing internal arrays/structs directly. Use the wrapper header `libnostr/include/nostr-filter-wrap.h` which provides read and write helpers with primitive types only.

Read helpers (all `(transfer none)`):

```c
size_t     nostr_filter_ids_len(const NostrFilter *filter);
const char *nostr_filter_ids_get(const NostrFilter *filter, size_t index);
size_t     nostr_filter_kinds_len(const NostrFilter *filter);
int        nostr_filter_kinds_get(const NostrFilter *filter, size_t index);
size_t     nostr_filter_authors_len(const NostrFilter *filter);
const char *nostr_filter_authors_get(const NostrFilter *filter, size_t index);
int64_t    nostr_filter_get_since_i64(const NostrFilter *filter);
int64_t    nostr_filter_get_until_i64(const NostrFilter *filter);
size_t     nostr_filter_tags_len(const NostrFilter *filter);
size_t     nostr_filter_tag_len(const NostrFilter *filter, size_t tag_index);
const char *nostr_filter_tag_get(const NostrFilter *filter, size_t tag_index, size_t item_index);
```

Write helpers:

```c
void nostr_filter_set_since_i64(NostrFilter *filter, int64_t since);     // (transfer none)
void nostr_filter_set_until_i64(NostrFilter *filter, int64_t until);     // (transfer none)
void nostr_filter_add_id(NostrFilter *filter, const char *id);           // copies string
void nostr_filter_add_kind(NostrFilter *filter, int kind);
void nostr_filter_add_author(NostrFilter *filter, const char *author);   // copies string
void nostr_filter_tags_append(NostrFilter *filter,                      
                              const char *key,                          
                              const char *value,                        
                              const char *relay);                       // copies strings
```

Notes:

- Strings passed in are copied, so caller retains ownership.
- Returned strings are internal and valid while the filter lives; treat as `(transfer none)`.
- Tags are exposed as a 2D accessor API; use `tags_len`, `tag_len`, `tag_get` to iterate.

Minimal example (C):

```c
NostrFilter *f = create_filter();
nostr_filter_add_author(f, "npub1...");
nostr_filter_add_kind(f, 1);
nostr_filter_set_since_i64(f, (int64_t)time(NULL) - 3600);
nostr_filter_tags_append(f, "#e", "<event-id>", NULL);
// ... use f ... then
nostr_filter_free(f);
```

### Relay (GBoxed) — ref-counted semantics

- Relay is now reference counted (`refcount` field). Public APIs:

```c
Relay *relay_ref(Relay *relay);     // increments refcount
void   relay_unref(Relay *relay);   // decrements; frees when hits 0
```

- Boxed registration (guarded by GLib): the boxed copy uses `relay_ref()` and boxed free uses `relay_unref()`. This ensures GI languages can hold references safely without duplicating threads or connection state.

Ownership rules:

- `new_relay()` returns ownership to the caller — treat as `(transfer full)`.
- Passing a relay into GI-managed fields/containers typically adds a ref; always call `relay_unref()` (or `free_relay()` which calls unref) when you are done.
- Do not attempt to deep-copy a relay; copying threads/connection is undefined. Use refs.

Shutdown order and safety:

- `relay_close()` cancels the connection context, closes queues, waits worker threads, and then closes the network connection in a safe order. See `docs/SHUTDOWN.md` for details.

