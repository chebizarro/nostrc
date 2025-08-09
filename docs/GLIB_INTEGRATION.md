# GLib/GObject Integration

This document explains how libnostr integrates with GLib/GObject for a pluggable JSON provider and how to consume it from GLib-friendly code and language bindings via GObject Introspection (GI).

## Build options

- NOSTR_WITH_GLIB=ON: Enables optional GLib code in libnostr (default: auto if GLib found).
- Defines NOSTR_HAVE_GLIB at compile time for conditional code.
- CMake links GLib transitively on the `nostr` target so downstreams inherit it automatically.

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
