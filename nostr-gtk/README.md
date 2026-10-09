# nostr-gtk

GTK4/libadwaita widget library for displaying Nostr content — reusable UI components representing NIPs and universal Nostr concepts.

## What it provides

- **NostrGtkTimelineView** — Universal scrollable event list with subscription-driven updates
- **NostrGtkNoteCardRow** — NIP-01 event rendering (notes, reposts, reactions)
- **NostrGtkThreadView** — NIP-10 threaded conversation display
- **NostrGtkComposer** — NIP-01 event composition widget
- **NostrGtkProfilePane** — NIP-01 kind:0 profile display
- **Portable library (`nostr_gtk_portable`)** — content parser; bounded GTK-free Markdown tokens; NIP-21 references and NIP-18 repost/quote descriptors and unsigned event templates; UI-only OG card, media viewer and NIP-34 issue fields. It has no Gnostr app symbols or network client.
- **Content renderer** — Pango markup + media URL extraction from note content
- **Blueprint templates** — Compiled `.blp` → `.ui` for all widgets

## Building

### CMake (primary)
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

### Meson (standalone, with GIR/VAPI)
```bash
cd nostr-gtk
# Requires the sibling nostr-gobject and nip19 CMake libraries built first.
meson setup build -Dintrospection=true -Dvapi=true
meson compile -C build
```

## Consuming

### pkg-config
```bash
pkg-config --cflags --libs nostr-gtk-portable-1.0  # portable API only
pkg-config --cflags --libs nostr-gtk-1.0           # legacy widgets
```

### CMake
```cmake
find_package(NostrGtkPortable CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE NostrGtk::Portable)
```

### GObject Introspection
- GIR: `GNostrGtk-1.0.gir`
- Typelib: `GNostrGtk-1.0.typelib`

### C include style
```c
#include <nostr-gtk-1.0/gnostr-timeline-view.h>
#include <nostr-gtk-1.0/gnostr-note-card-row.h>
#include <nostr-gtk-1.0/content_renderer.h>
#include <nostr-gtk-1.0/gn-markdown.h>
#include <nostr-gtk-1.0/gn-nostr-reference.h>
```

## Dependencies

- nostr-gobject-1.0
- GTK4 ≥ 4.6
- libadwaita ≥ 1.2
- GLib/GObject/GIO ≥ 2.70
- json-glib and NIP-19 for the portable parser/reference APIs
- libsoup-3.0 (optional, legacy widgets only; portable widgets never fetch)

## License

GPL-3.0-or-later
