# NIP-77 (Negentropy)

This document describes the optional NIP-77 module implemented under `nips/nip77/`.

- Scope: optional, does not modify libnostr core. JSON interop goes through the canonical `NostrJsonInterface` (or GLib bridge).
- Public headers live under `nips/nip77/include/nostr/nip77/`.
- Optional backends and wrappers are gated via CMake options.

## Build options

- `WITH_NIP77` (ON): enable the module.
- `WITH_NIP77_GLIB` (OFF): build GLib/GObject wrapper.
- `WITH_NIP77_NOSTRDB` (ON): enable the `nostrdb` backend.

## Public API sketch

```c
// include/nostr/nip77/negentropy.h

// Opaque session
typedef struct NostrNegSession NostrNegSession;

// 32-byte Event ID
typedef struct {
  unsigned char bytes[32];
} NostrEventId;

// Index item (timestamp + id)
typedef struct {
  uint64_t created_at; // seconds since epoch
  NostrEventId id;     // 32 bytes
} NostrIndexItem;

// Datasource interface
typedef struct {
  void *ctx;
  int (*begin_iter)(void *ctx);
  int (*next)(void *ctx, NostrIndexItem *out);
  void (*end_iter)(void *ctx);
} NostrNegDataSource;

// Options and Stats
typedef struct {
  uint32_t max_ranges;
  uint32_t max_idlist_items;
  uint32_t max_round_trips;
} NostrNegOptions;

typedef struct {
  uint64_t bytes_sent;
  uint64_t bytes_recv;
  uint32_t rounds;
  uint32_t ranges_sent;
  uint32_t ids_sent;
} NostrNegStats;

NostrNegSession *nostr_neg_session_new(const NostrNegDataSource *ds, const NostrNegOptions *opts);
void nostr_neg_session_free(NostrNegSession *s);

// Hex messages are malloc'd; caller frees
char *nostr_neg_build_initial_hex(NostrNegSession *s);
int   nostr_neg_handle_peer_hex(NostrNegSession *s, const char *hex_msg);
char *nostr_neg_build_next_hex(NostrNegSession *s);

// Shallow copy of stats
void  nostr_neg_get_stats(const NostrNegSession *s, NostrNegStats *out);
```

## CLI demo

`tools/gnostr-neg.c` provides a tiny local demo for validation. It accepts `--db=PATH` when built with `WITH_NIP77_NOSTRDB=ON` and `--filter=JSON`.

## Testing

- Unit tests for varint, bound, fingerprint, message, and session.
- Integration test for `nostrdb` backend (guarded by `WITH_NIP77_NOSTRDB`).
