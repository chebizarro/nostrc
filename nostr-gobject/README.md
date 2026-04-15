# nostr-gobject

GObject wrapper library for libnostr — provides GLib/GObject/GIO bindings for the Nostr protocol.

## What it provides

- **GObject wrappers** for libnostr types: events, relays, subscriptions, pools, filters
- **GNostrStore interface** with NDB backend for event storage, search, and reactive subscriptions
- **Services**: identity, relay management, mute lists, sync, profile provider/service
- **Models**: timeline queries, profiles, thread graph, NIP-10 thread manager
- **NDB subscription dispatcher** for efficient subscription-driven UI updates

## Building

### CMake (primary)
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

### Meson (standalone, with GIR/VAPI)
```bash
cd nostr-gobject
meson setup build -Dintrospection=true -Dvapi=true
meson compile -C build
```

## Consuming

### pkg-config
```bash
pkg-config --cflags --libs nostr-gobject-1.0
```

### GObject Introspection
- GIR: `GNostr-1.0.gir`
- Typelib: `GNostr-1.0.typelib`

### Vala
```vala
// nostr-gobject-1.0.vapi
using GNostr;
```

### C include style
```c
#include <nostr-gobject-1.0/nostr_event.h>
#include <nostr-gobject-1.0/nostr_pool.h>
#include <nostr-gobject-1.0/nostr_store.h>
```

## Dependencies

- GLib/GObject/GIO ≥ 2.70
- libnostr (sibling library)
- libgo (sibling library)
- OpenSSL, libsecp256k1, libwebsockets, nsync
- json-glib (optional), libsodium (optional)

## License

GPL-3.0-or-later
