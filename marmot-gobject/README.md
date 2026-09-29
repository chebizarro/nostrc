# marmot-gobject

GObject wrapper library for [libmarmot](../libmarmot/) — secure group messaging over Nostr using MLS (RFC 9420).

## Overview

`marmot-gobject` provides GObject type wrappers around the pure C `libmarmot` library, enabling:

- **GTask-based async API** — Non-blocking group creation, message sending, welcome processing
- **GObject signals** — `group-joined`, `message-received`, `welcome-received`
- **GObject Introspection (GIR)** — Language bindings for Python, JavaScript, Lua, etc.
- **Vala bindings (VAPI)** — First-class Vala support
- **GObject property system** — All fields accessible via `g_object_get()`/`g_object_set()`

## Types

| GObject Type | Wraps | Purpose |
|-------------|-------|---------|
| `MarmotGobjectClient` | `Marmot` | Main API entry point (async operations + signals) |
| `MarmotGobjectGroup` | `MarmotGroup` | Group metadata and state |
| `MarmotGobjectMessage` | `MarmotMessage` | Decrypted group message |
| `MarmotGobjectWelcome` | `MarmotWelcome` | Group invitation |
| `MarmotGobjectStorage` | `MarmotStorage` | GInterface for storage backends |
| `MarmotGobjectStorageMemory` | In-memory backend | Testing and ephemeral use |
| `MarmotGobjectStorageSqlite` | SQLite backend | Persistent storage |

## Building

### Meson (recommended — includes GIR + VAPI)

```bash
cd nostrc/marmot-gobject
meson setup build -Dintrospection=true -Dvapi=true
ninja -C build
ninja -C build test
```

### CMake (in-tree)

```bash
cd nostrc
cmake -B build -DBUILD_MARMOT_GOBJECT=ON -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build -R marmot_gobject
```

### CMake (standalone with GIR)

```bash
cd nostrc/marmot-gobject
cmake -B build -DBUILD_TESTING=ON -DMARMOT_GOBJECT_INTROSPECTION=ON
cmake --build build
```

## Usage

### C

```c
#include <marmot-gobject-1.0/marmot-gobject.h>

MarmotGobjectStorageMemory *storage = marmot_gobject_storage_memory_new();
MarmotGobjectClient *client = marmot_gobject_client_new(
    MARMOT_GOBJECT_STORAGE(storage));

g_signal_connect(client, "message-received",
    G_CALLBACK(on_message_received), NULL);

// Async group creation
marmot_gobject_client_create_group_async(client,
    member_pubkeys, n_members, "My Group", "Description",
    NULL, on_group_created, NULL);

g_object_unref(client);
```

### Python (via GI)

```python
from gi.repository import Marmot

storage = Marmot.StorageMemory.new()
client = Marmot.Client.new(storage)
client.connect("message-received", lambda c, msg: print(msg.props.content))
```

### Vala

```vala
var storage = new Marmot.StorageMemory();
var client = new Marmot.Client(storage);
client.message_received.connect((msg) => {
    print("From %s: %s\n", msg.sender_pubkey, msg.content);
});
```

## pkg-config

```bash
pkg-config --cflags --libs marmot-gobject-1.0
```

## Dependencies

- GLib ≥ 2.70
- GObject ≥ 2.70
- GIO ≥ 2.70
- libmarmot

## Generated Bindings

When built with Meson and `introspection=true`:

| Output | Path | Purpose |
|--------|------|---------|
| `Marmot-1.0.gir` | `$prefix/share/gir-1.0/` | GObject Introspection XML |
| `Marmot-1.0.typelib` | `$prefix/lib/girepository-1.0/` | Compiled GIR for runtime binding |
| `marmot-gobject-1.0.vapi` | `$prefix/share/vala/vapi/` | Vala bindings (if `vapi=true`) |

## Changelog

### 1.4.0 (unreleased)

- Added `marmot_gobject_client_get_account_proof_template()`,
  `marmot_gobject_client_set_account_proof()` and
  `marmot_gobject_client_has_account_proof()`, which wrap libmarmot 0.10.0's
  account-proof enrollment (nostrc-7vyi). A signer-only client signs the
  kind:450 template once per client; its unsigned KeyPackages and the groups
  it creates then carry the account-identity proof that other members require.
- `marmot_gobject_client_create_key_package_unsigned_async()` fails with
  `MARMOT_ERR_KEY_PACKAGE_IDENTITY` until the client is enrolled.

### 1.3.0 (unreleased)

- Added `marmot_gobject_client_process_rumor_message_async()` and
  `_finish()` for kind:445 rumors from NIP-59 gift wraps. They take
  libmarmot 0.6.0's `marmot_process_rumor_message()`.
- `marmot_gobject_client_process_message_async()` now rejects kind:445 events
  whose id or signature does not verify (libmarmot 0.6.0, nostrc-6r6s). The
  error codes are `MARMOT_ERR_EVENT` and `MARMOT_ERR_SIGNATURE`.

### 1.1.0 (unreleased)

- Added `MARMOT_GOBJECT_KIND_KEY_PACKAGE` (30443) and
  `marmot_gobject_select_key_package_event()`.
- KeyPackage events from `marmot_gobject_client_create_key_package*_async()` now
  use kind 30443 with a stable per-account `d` slot (libmarmot 0.2.0). Relay
  filters that fetch KeyPackages must use the new constant, because kind 443 is
  no longer accepted.

## Tests

43 test cases covering:
- Enum type registration (4 types)
- Group/Message/Welcome lifecycle and properties
- Storage GInterface hierarchy
- Client lifecycle, signals, synchronous queries
- Stress tests (1000 objects, 4 concurrent threads)
- Edge cases (empty strings, large kinds, negative timestamps)

```bash
ctest -R marmot_gobject --output-on-failure
```

## License

MIT
