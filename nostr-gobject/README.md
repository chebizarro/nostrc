# nostr-gobject

GObject wrapper library for libnostr — provides GLib/GObject/GIO bindings for the Nostr protocol.

## 2.0.0 ABI note (unreleased)

The installed `signer_proxy.h` is generated from `org.nostr.Signer.xml`. Adding
`EnableTypedApprovalErrors` inserts a handler into its public
`NostrOrgNostrSignerIface` vtable before existing handlers. This changes the
stable 1.0.0 ABI: rebuild any consumer that implements that interface, and do
not load a 1.0.0 interface implementation against the 2.0.0 library. The CMake
and Meson shared libraries use SONAME 2; `Latest release` is unchanged until
there is a published component tag. The `nostr-gobject-1.0` package and GIR
names remain compatibility namespaces, not a claim of ABI 1.

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

### CMake build-tree executable contract

With `BUILD_GROUNDHOG=ON` on ELF, the shared GObject runtime intentionally
leaves core/archive symbols for its executable to supply (one libnostr per
process). In-tree CMake executables can link `nostrc::gobject_executable`;
existing `nostr_gobject` consumers retain the same transitive requirements.
External build systems must not link just the shared library or guess its
archive closure:

```bash
cmake --build build --target nostr-gobject-build-tree-pc
pkg-config --libs "$PWD/build/nostr-gobject/nostr-gobject-build-tree.pc"
```

This **uninstalled**, build-tree-only `.pc` is generated from the same owning
archive list as the CMake executable link interface, using exact target artifacts,
optional dependencies, rpaths and the one-core linker policy. Build the matching
CMake libraries before linking an external consumer. For a multi-config tree,
the `.pc` lives under `nostr-gobject/<configuration>/`; select it with Grotto's
`-Dcmake_configuration=Debug` (or the configuration you built) and the runtime
check's `--configuration Debug`. Grotto's Meson frontend
and the GIR scanner's temporary executable use this same contract. Do not link
a second copy of the core into the shared GObject library.

### GObject Introspection
- GIR: `GNostr-1.0.gir`
- Typelib: `GNostr-1.0.typelib`

Enable CMake generation with `-DNOSTR_GOBJECT_ENABLE_GIR=ON`. A static build's
scanner wrapper and its GIR/typelib are build-tree-only: none is installed.
Only a shared runtime's GIR/typelib is installed.

In the executable-owned ELF layout, an embedding host must also supply the
executable link contract. A plain Python executable does not supply it; fixing
the scanner link does not make that process an owning host. The explicit local
PyGObject check builds a Python embedding executable against the `.pc` above
(requires matching `python3-embed` development files and PyGObject):

```bash
python3 scripts/check-gobject-gir-runtime.py build
python3 scripts/check-gir-exports.py build/nostr-gobject/gir/GNostr-1.0.gir \
    build/nostr-gobject/libnostr-gobject-1.0.so
```

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
