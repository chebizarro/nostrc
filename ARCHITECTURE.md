# Architecture

## Overview

Nostrc is a monorepo implementing the Nostr protocol in C, from low-level crypto primitives to full GTK4 desktop applications. It is built via CMake (primary) and Meson (per-library), with optional GObject Introspection (GIR) and Vala (VAPI) bindings.

### Libraries

| Library | Directory | Purpose | Dependencies |
|---------|-----------|---------|-------------|
| **libgo** | `libgo/` | Go-like concurrency: channels, contexts, wait groups, fibers | nsync |
| **libnostr** | `libnostr/` | Core Nostr types: events, relays, subscriptions, filters, keys | libgo, OpenSSL, libsecp256k1, libwebsockets |
| **libjson** | `libjson/` | JSON serialization/deserialization via jansson | libnostr, jansson, nsync |
| **NIP modules** | `nips/` | Per-NIP implementations (NIP-01 through NIP-98) | libnostr |
| **libmarmot** | `libmarmot/` | Marmot protocol: MLS (RFC 9420) + Nostr for group messaging | libsodium, OpenSSL, libnostr, NIP-44, NIP-59 |
| **nostr-gobject** | `nostr-gobject/` | GObject wrappers for libnostr + models + services | libnostr, GLib/GObject/GIO |
| **nostr-gtk** | `nostr-gtk/` | GTK4/libadwaita widget library | nostr-gobject, GTK4, libadwaita |
| **marmot-gobject** | `marmot-gobject/` | GObject wrappers for libmarmot | libmarmot, GLib/GObject/GIO |

### Applications

| App | Directory | Purpose |
|-----|-----------|---------|
| **gnostr** | `apps/gnostr/` | GTK4 Nostr desktop client |
| **gnostr-signer** | `apps/gnostr-signer/` | D-Bus signing service (NIP-46 bunker) |
| **relayd** | `apps/relayd/` | Nostr relay daemon |
| **relayctl** | `apps/relayctl/` | Relay management CLI |
| **grelay** | `apps/grelay/` | GTK relay monitor |

### GNOME Integration

| Component | Directory | Purpose |
|-----------|-----------|---------|
| GNOME Online Accounts provider | `gnome/goa/` | Nostr identity via GOA |
| Seahorse helpers | `gnome/seahorse/` | Secret Service / GNOME Keyring integration |
| PKCS#11 module | `pkcs11/` | Hardware token support |
| nostr-homed | `gnome/nostr-homed/` | GNOME roaming home via Nostr |
| GOA overlay | `gnome/nostr-goa-overlay/` | GOA provider overlay |

---

## Dependency Graph

```
                        ┌─────────────┐
                        │   libgo     │  Go-like concurrency (channels, fibers)
                        └──────┬──────┘
                               │
                        ┌──────▼──────┐
                ┌──────►│  libnostr   │◄──────┐
                │       │  (core C)   │       │
                │       └──────┬──────┘       │
                │              │              │
         ┌──────┴──────┐ ┌────▼─────┐ ┌──────┴──────┐
         │   libjson   │ │  NIP-44  │ │  NIP-59     │
         │  (jansson)  │ │  NIP-19  │ │  NIP-51     │
         └─────────────┘ │  NIP-46  │ │   etc.      │
                         │  etc.    │ └──────┬──────┘
                         └────┬─────┘        │
                              │              │
                       ┌──────▼──────────────▼───┐
                       │      libmarmot          │  MLS + Nostr group messaging
                       │  (MIP-00 ─ MIP-04)      │
                       └──────────┬──────────────┘
                                  │
          ┌───────────────────────┼───────────────────────┐
          │                       │                       │
   ┌──────▼──────┐        ┌──────▼──────┐        ┌───────▼──────┐
   │nostr-gobject│        │marmot-gobject│       │  nostr-gtk   │
   │  (GObject)  │        │  (GObject)   │       │ (GTK4/Adw)   │
   └──────┬──────┘        └──────┬───────┘       └──────┬───────┘
          │                      │                       │
          └──────────────────────┼───────────────────────┘
                                 │
                    ┌────────────▼─────────────┐
                    │     gnostr / gnostr-signer│
                    │   relayd / relayctl       │
                    │   (applications)          │
                    └──────────────────────────┘
```

---

## Data Flow

### Event Flow (read path)
1. **Relay** → WebSocket message received by `libnostr` relay connection
2. **Validation** → Event signature verified (`nostr_event_check_signature`), invalid events logged and discarded
3. **Ingestion** → Valid events queued to background ingest thread → stored in nostrdb (NDB)
4. **Subscription** → NDB notifies subscribed models via poll/callback
5. **Model** → `GnNostrEventModel` (nostr-gobject) maintains a windowed view into NDB
6. **Widget** → `GtkListView` (nostr-gtk) binds model items to `NoteCardRow` widgets via factory

### Event Flow (write path)
1. **Compose** → User creates note in `GnostrComposer` (nostr-gtk)
2. **Sign** → Event signed via local signer or D-Bus signer proxy (NIP-46)
3. **Publish** → `relay_publish_thread` sends to all write relays via `g_task_run_in_thread`
4. **Reconcile** → Per-relay OK/CLOSED responses collected, toast shows success/failure breakdown

### Group Messaging Flow (Marmot)
1. **Create group** → `marmot_create_group()` initializes MLS group state → produces kind:443 KeyPackages + kind:444 Welcome events
2. **Join** → `marmot_process_welcome()` decrypts NIP-59 gift-wrapped Welcome → initializes member's group state
3. **Send** → `marmot_create_message()` encrypts inner event with NIP-44 using MLS exporter secret → kind:445 event
4. **Receive** → `marmot_process_message()` decrypts kind:445 → recovers inner event + sender identity

---

## libmarmot MLS Architecture

libmarmot implements a focused subset of RFC 9420 (MLS) supporting exactly one ciphersuite: `MLS_128_DHKEMX25519_AES128GCM_SHA256_Ed25519` (0x0001).

```
┌─────────────────────────────────────────────────────┐
│                Public API (marmot.h)                 │
│  marmot_create_group / process_welcome / …           │
├─────────────────────────────────────────────────────┤
│               Protocol Layer (MIP-00─04)             │
│  credentials · groups · welcome · messages · media   │
├─────────────────────────────────────────────────────┤
│                 MLS Layer (RFC 9420)                  │
│  mls_crypto    — X25519, Ed25519, AES-128-GCM, HKDF │
│  mls_tls       — TLS presentation language codec     │
│  mls_tree      — TreeKEM ratchet tree (left-balanced)│
│  mls_key_sched — Epoch secrets, sender ratchets      │
│  mls_framing   — PrivateMessage encrypt/decrypt      │
│  mls_key_pkg   — KeyPackage creation/validation      │
│  mls_group     — Group state machine                 │
│  mls_welcome   — Welcome message construction        │
├─────────────────────────────────────────────────────┤
│             Storage Interface (vtable)                │
│  memory · sqlite · nostrdb  backends                 │
├─────────────────────────────────────────────────────┤
│  libsodium (Ed25519/X25519)  OpenSSL (AES/HKDF/SHA) │
└─────────────────────────────────────────────────────┘
```

### Storage Backends

| Backend | Use Case | Persistence | Encryption |
|---------|----------|-------------|------------|
| Memory | Testing, ephemeral | ❌ | N/A |
| SQLite | Standalone apps | ✅ | Optional (via SQLCipher) |
| nostrdb | gnostr app integration | ✅ (via LMDB) | Via LMDB encryption |

### GObject Integration (marmot-gobject)

`marmot-gobject` wraps libmarmot in GObject types for use in GTK applications:

| C Type | GObject Wrapper | GIR Type |
|--------|----------------|----------|
| `Marmot` | `MarmotGobjectClient` | `Marmot.Client` |
| `MarmotGroup` | `MarmotGobjectGroup` | `Marmot.Group` |
| `MarmotMessage` | `MarmotGobjectMessage` | `Marmot.Message` |
| `MarmotWelcome` | `MarmotGobjectWelcome` | `Marmot.Welcome` |
| `MarmotStorage` | `MarmotGobjectStorage` (GInterface) | `Marmot.Storage` |

All async operations use `GTask` for non-blocking integration with the GTK main loop:
- `marmot_gobject_client_create_group_async()` / `_finish()`
- `marmot_gobject_client_process_welcome_async()` / `_finish()`
- `marmot_gobject_client_send_message_async()` / `_finish()`

Signals: `group-joined`, `message-received`, `welcome-received`

---

## Concurrency Architecture

### libgo Runtime

libgo provides Go-like concurrency primitives in C:

| Primitive | Header | Purpose |
|-----------|--------|---------|
| `GoChannel` | `go_channel.h` | Bounded/unbounded channels with nsync CVs |
| `GoContext` | `go_context.h` | Cancellation propagation |
| `GoWaitGroup` | `go_wait_group.h` | Fork-join synchronization |
| `go()` / `go_fiber_compat()` | `go.h` | Launch goroutines (OS thread or fiber) |
| `go_select()` | `select.h` | Multi-channel select with waiter signaling |
| `go_blocking_submit()` | `blocking_executor.h` | Submit work to bounded thread pool |

### Fiber Runtime (libgo/fiber/)

A cooperative work-stealing fiber scheduler for lightweight concurrency:

- **Workers**: N OS threads (default: CPU count) running the work-stealing scheduler
- **Fibers**: Lightweight stacks (256KB default) parked/woken cooperatively
- **`go_fiber_compat()`**: Migration shim — uses fibers when available, falls back to `go()` (OS threads)
- **`gof_start_background()`**: Start scheduler without blocking (GTK-compatible)
- **Netpoll**: epoll/kqueue integration for I/O

Thread count reduction: ~187 OS threads → ~12 (8 fiber workers + 4 blocking executor threads)

---

## Security Posture

- **Event integrity**: Canonical NIP-01 preimage for id/signature; verification recomputes hash
- **Encryption**: NIP-04 AEAD v2 (AES-256-GCM), NIP-44 v2 (ChaCha20-Poly1305 + HKDF), NIP-59 gift wrapping
- **MLS (RFC 9420)**: Forward secrecy, post-compromise security via TreeKEM epoch rotation
- **Key storage**: libsecret (Linux/GNOME Keyring) or macOS Keychain; never logged
- **Ingress mitigations**: Replay TTL + timestamp skew checks with metrics
- **Build hardening**: `-fstack-protector-strong`, `-D_FORTIFY_SOURCE=2`, RELRO, ASan/UBSan/TSan support

---

## Build System

### CMake (primary)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DBUILD_LIBMARMOT=ON \
  -DBUILD_MARMOT_GOBJECT=ON \
  -DBUILD_NOSTR_GTK=ON \
  -DBUILD_APPS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

### Meson (per-library)

Each library has a standalone `meson.build`:
- `nostr-gobject/meson.build` — GIR + VAPI generation
- `nostr-gtk/meson.build` — Blueprint compilation, GIR + VAPI
- `libmarmot/meson.build` — Crypto + storage tests
- `marmot-gobject/meson.build` — GIR + VAPI generation

### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_LIBMARMOT` | ON | Build libmarmot library |
| `BUILD_MARMOT_GOBJECT` | ON | Build marmot-gobject GObject wrapper |
| `BUILD_NOSTR_GTK` | ON | Build nostr-gtk widget library |
| `BUILD_APPS` | ON | Build application binaries |
| `BUILD_TESTING` | ON | Build test executables |
| `BUILD_BENCHMARKS` | OFF | Build performance benchmarks |
| `GNOSTR_ENABLE_ASAN` | OFF | AddressSanitizer |
| `GNOSTR_ENABLE_UBSAN` | OFF | UndefinedBehaviorSanitizer |
| `GNOSTR_ENABLE_TSAN` | OFF | ThreadSanitizer |
| `NOSTR_ENABLE_GI` | OFF | GObject Introspection for libnostr |
| `MARMOT_GOBJECT_INTROSPECTION` | OFF | GIR for marmot-gobject (CMake) |

---

## Test Architecture

See [docs/TESTING.md](docs/TESTING.md) for the comprehensive test strategy.

```
┌──────────────────────────────────────────┐
│  Real-Component Benchmarks (nightly)     │
├──────────────────────────────────────────┤
│  Real-Component Integration (CI per-PR)  │
├──────────────────────────────────────────┤
│  Mock-Based Integration (CI per-PR)      │
├──────────────────────────────────────────┤
│  Widget Tests (Xvfb, CI per-PR)          │
├──────────────────────────────────────────┤
│  Unit Tests (no display, fast)           │
└──────────────────────────────────────────┘
```

### Test Suites by Library

| Library | Test Files | Test Count | Focus |
|---------|-----------|------------|-------|
| libmarmot | 16 files | ~200+ | MLS crypto, tree, key schedule, framing, group, welcome, protocol, storage, interop |
| nostr-gobject | 6 files | ~35 | Store contract, lifecycle, profile service, event flow |
| nostr-gtk | 6 files | ~25 | Widget recycling, sizing, bind latency, thread views |
| apps/gnostr | 5 files | ~30 | Event model windowing, NDB violations, real-bind latency |
| tests/ | 40+ files | ~200+ | Core libnostr, relay, subscription, JSON, crypto |

---

## ASCII Module Map

```
/ (root)
├── libgo/              # Concurrency primitives + fiber runtime
├── libnostr/           # Core Nostr types, relay/subscription, keys
├── libjson/            # JSON integration via jansson
├── nips/               # NIP-specific extensions (01─98)
├── libmarmot/          # Marmot protocol: MLS + Nostr group messaging
│   └── src/mls/        # Focused RFC 9420 implementation
├── nostr-gobject/      # GObject wrappers + models + services
├── nostr-gtk/          # GTK4/libadwaita widgets
├── marmot-gobject/     # GObject wrappers for libmarmot
├── apps/
│   ├── gnostr/         # GTK4 desktop client
│   ├── gnostr-signer/  # D-Bus signing service
│   ├── relayd/         # Relay daemon
│   ├── relayctl/       # Relay management CLI
│   └── grelay/         # GTK relay monitor
├── gnome/              # GNOME platform integration
├── components/nostrdb/ # nostrdb storage component
├── testing/            # Mock relay testing framework
├── tests/              # Global unit/integration tests
├── tests/testkit/      # Shared test infrastructure
├── benchmark/          # Performance benchmarks
├── tools/              # Development tools
├── docs/               # Documentation
├── skills/             # LLM skill documents
├── cmake/              # Shared CMake modules (GnTest.cmake, Blueprint.cmake)
└── examples/           # Example programs
```
