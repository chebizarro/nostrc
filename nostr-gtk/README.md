# nostr-gtk

GTK4/libadwaita widget library for displaying Nostr content — reusable UI components representing NIPs and universal Nostr concepts.

## What it provides

- **NostrGtkTimelineView** — Universal scrollable event list with subscription-driven updates
- **NostrGtkNoteCardRow** — NIP-01 event rendering (notes, reposts, reactions)
- **NostrGtkThreadView** — NIP-10 threaded conversation display
- **NostrGtkComposer** — NIP-01 event composition widget
- **NostrGtkProfilePane** — NIP-01 kind:0 profile display
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
meson setup build -Dintrospection=true -Dvapi=true
meson compile -C build
```

## Consuming

### pkg-config
```bash
pkg-config --cflags --libs nostr-gtk-1.0
```

### GObject Introspection
- GIR: `GNostrGtk-1.0.gir`
- Typelib: `GNostrGtk-1.0.typelib`

### C include style
```c
#include <nostr-gtk-1.0/gnostr-timeline-view.h>
#include <nostr-gtk-1.0/gnostr-note-card-row.h>
#include <nostr-gtk-1.0/content_renderer.h>
```

## Dependencies

- nostr-gobject-1.0
- GTK4 ≥ 4.6
- libadwaita ≥ 1.2
- GLib/GObject/GIO ≥ 2.70
- json-glib, libsoup-3.0 (optional, for async image loading)

## License

GPL-3.0-or-later
