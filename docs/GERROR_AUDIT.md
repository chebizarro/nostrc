# GError Usage Audit — nostr-gobject, nostr-gtk, apps

**Date**: 2026-02-17
**Scope**: All public APIs in nostr-gobject, nostr-gtk, apps/gnostr, apps/gnostr-signer

---

## Executive Summary

GError usage is **inconsistent and incomplete** across the library stack. The GObject-layer APIs that were properly designed (NIP-19, NIP-46, NIP-49, GNostrStore, GNostrPool) have excellent GError support with a well-defined `NOSTR_ERROR` domain (18 error codes). However, the majority of nostr-gobject services and **all** of nostr-gtk lack GError integration entirely, relying instead on `g_warning()` log messages, boolean returns, or bare int status codes.

For a reusable library intended for third-party consumers, plugins, and language bindings (Python, Vala, JS via GIR), this is a significant gap — callers cannot programmatically distinguish error types or provide user-facing error messages.

---

## Metrics

| Area | Headers with GError | Headers without GError | g_set_error calls | g_warning (error logs) | g_autoptr(GError) |
|------|--------------------|-----------------------|-------------------|------------------------|-------------------|
| **nostr-gobject/include/** | 8 of 41 (20%) | 33 (80%) | 50 | 56 | **0** |
| **nostr-gtk/include/** | 0 of 11 (0%) | 11 (100%) | **0** | 27 | **0** |
| **apps/gnostr/src/** | N/A (consumer) | — | — | ~200+ | — |
| **apps/gnostr-signer/** | N/A (consumer) | — | — | ~100+ | — |

---

## What's Done Well ✅

### nostr-gobject: Good GError Coverage

| Header | Functions with GError | Error Domain |
|--------|----------------------|--------------|
| `nostr_nip19.h` | 8 functions | `NOSTR_ERROR` |
| `nostr_nip46_client.h` | 5 functions | `NOSTR_ERROR` |
| `nostr_nip46_bunker.h` | 3 functions | `NOSTR_ERROR` |
| `nostr_nip49.h` | 3 functions | `NOSTR_ERROR` |
| `nostr_event.h` | 3 functions (from_json, sign, verify) | `NOSTR_ERROR` |
| `nostr_filter.h` | 1 function (from_json) | `NOSTR_ERROR` |
| `nostr_pointer.h` | 2 functions | `NOSTR_ERROR` |
| `gnostr-identity.h` | 6 functions (async import/export) | `NOSTR_ERROR` |
| `nostr_store.h` | 8 interface vfuncs (CRUD + search) | `NOSTR_ERROR` |
| `nostr_pool.h` | 4 functions (query, connect, subscribe) | `NOSTR_ERROR` |

### Error Domain is Well-Designed

`nostr-error.h` defines 18 error codes with clear semantics:

```
NOSTR_ERROR_INVALID_EVENT, NOSTR_ERROR_INVALID_FILTER,
NOSTR_ERROR_SIGNATURE_INVALID, NOSTR_ERROR_SIGNATURE_FAILED,
NOSTR_ERROR_CONNECTION_FAILED, NOSTR_ERROR_CONNECTION_CLOSED,
NOSTR_ERROR_TIMEOUT, NOSTR_ERROR_PERMISSION_DENIED,
NOSTR_ERROR_RELAY_ERROR, NOSTR_ERROR_PARSE_FAILED,
NOSTR_ERROR_ENCRYPTION_FAILED, NOSTR_ERROR_DECRYPTION_FAILED,
NOSTR_ERROR_INVALID_KEY, NOSTR_ERROR_NOT_FOUND,
NOSTR_ERROR_INVALID_STATE, NOSTR_ERROR_MESSAGE_TOO_LARGE,
NOSTR_ERROR_SUBSCRIPTION_LIMIT, NOSTR_ERROR_AUTH_REQUIRED,
NOSTR_ERROR_PAYMENT_REQUIRED
```

### GError Handling Patterns are Correct (where used)

- `g_clear_error()` used consistently (no bare `g_error_free` without NULL-setting)
- `g_task_return_error()` transfers ownership correctly in async operations
- Error propagation through D-Bus proxy calls is clean

---

## 🔴 Issues Found

### Issue 1: nostr-gtk Has Zero GError Support

**Severity**: HIGH (blocks reusable library goal)

All 11 public headers in nostr-gtk return `void` or objects directly with no error mechanism. Errors are logged via `g_warning()` and silently swallowed.

**Affected APIs (should have GError)**:

| Function | Current Pattern | Problem |
|----------|----------------|---------|
| `gnostr_render_content()` | Returns `NULL` on failure | Caller can't distinguish "empty content" from "parse error" |
| `nostr_gtk_profile_pane_update_from_json()` | `g_warning("invalid JSON")` | Caller gets no feedback on malformed input |
| `nostr_gtk_timeline_view_*()` | void returns | Various load/filter failures are silent |
| `gnostr_thread_view_load_thread()` | void, logs errors | Caller can't know if load failed |
| `nostr_gtk_note_card_row_*()` | void, logs errors | Content render failures are silent |

**Impact**: Language binding consumers (Python, Vala, JS) cannot handle errors programmatically. They see either a NULL return or nothing.

### Issue 2: storage_ndb.h Uses Bare Int Returns

**Severity**: HIGH (blocks GNostrStore error propagation)

`storage_ndb.h` is the primary storage backend with 40+ functions returning `int` (0=success, nonzero=failure). No error descriptions, no error categorization.

**Examples**:
```c
int storage_ndb_init(const char *dbdir, const char *opts_json);  // 0 or nonzero
int storage_ndb_begin_query(void **txn_out);                      // 0 or nonzero
int storage_ndb_get_note_by_id(void *txn, ...);                  // 0 or nonzero
```

When `GNostrNdbStore` (the GObject wrapper) implements `GNostrStore` interface methods with GError, it has to fabricate generic errors:
```c
g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_NOT_FOUND, "NDB query failed");
```
...losing the actual reason (transaction contention, corrupted index, invalid key format, etc.).

### Issue 3: Service APIs Use Custom Callback Types Instead of GTask/GError

**Severity**: MEDIUM (non-standard, hard for language bindings)

| Service | Callback Pattern | Problem |
|---------|-----------------|---------|
| `GnostrProfileService` | `GnostrProfileServiceCallback(pubkey, meta, user_data)` | No GError — failure = `meta==NULL` with no reason |
| `GNostrMuteList` | `GNostrMuteListFetchCallback(self, success, user_data)` | Boolean success, no reason |
| `GNostrMuteList` | `GNostrMuteListSaveCallback(self, success, error_msg, user_data)` | Raw `const char *error_msg` instead of GError |
| `GNostrSyncService` | EventBus topics like `"sync::error"` | Error as JSON string instead of GError |

These custom callback signatures are:
1. Not introspectable via GIR (no `(scope async)` annotation possible)
2. Not composable with `GTask`/`GAsyncResult`
3. Missing structured error information

### Issue 4: Zero g_autoptr(GError) Usage

**Severity**: LOW (correctness risk, not a current bug)

All 33 `GError *error = NULL` declarations in nostr-gobject/src/ use bare pointers. While `g_clear_error()` is used correctly throughout, adding early returns to these functions in the future would leak the GError if the developer forgets `g_clear_error` on the new path.

**Pattern to adopt**:
```c
// Before (current):
GError *error = NULL;
...
g_clear_error(&error);  // manual cleanup needed on every path

// After (recommended):
g_autoptr(GError) error = NULL;
// Automatically freed when leaving scope
```

### Issue 5: Inconsistent Error Codes in nostr_query_batcher.c

**Severity**: LOW

Uses `G_IO_ERROR` domain instead of `NOSTR_ERROR`:
```c
GError *gerr = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED, "Batcher shutdown");
```

Should use the library's own error domain for consistency.

---

## Recommendations

### R1: Add NOSTR_GTK_ERROR Domain to nostr-gtk (HIGH)

Create `nostr-gtk-error.h`:
```c
#define NOSTR_GTK_ERROR (nostr_gtk_error_quark())
GQuark nostr_gtk_error_quark(void);

typedef enum {
    NOSTR_GTK_ERROR_RENDER_FAILED,     // Content rendering failure
    NOSTR_GTK_ERROR_INVALID_INPUT,     // Bad JSON, empty pubkey, etc
    NOSTR_GTK_ERROR_LOAD_FAILED,       // Thread/profile/media load failure
    NOSTR_GTK_ERROR_RESOURCE_MISSING,  // Blueprint/icon resource not found
    NOSTR_GTK_ERROR_STORAGE_FAILED,    // NDB query failure from widget
} NostrGtkError;
```

Then add `GError **error` to the 6 most important APIs:
- `gnostr_render_content()` — `GError **error`
- `nostr_gtk_profile_pane_update_from_json()` — `GError **error`
- `nostr_gtk_timeline_view_load()` — `GError **error` (if applicable)
- `gnostr_thread_view_load_thread()` — `GError **error`
- `gnostr_render_content_markup()` — `GError **error`
- `gnostr_extract_media_urls()` — `GError **error`

### R2: Add GError to storage_ndb.h (HIGH)

Option A (preferred): Add `GError **` parameter to existing functions:
```c
int storage_ndb_init(const char *dbdir, const char *opts_json, GError **error);
int storage_ndb_get_note_by_id(void *txn, const unsigned char id32[32],
                                char **json_out, int *json_len, GError **error);
```

Option B: Add a companion `storage_ndb_get_last_error()` function for the current session:
```c
const char *storage_ndb_get_last_error(void);   // Thread-local error string
int storage_ndb_get_last_error_code(void);       // Error category
```

Option A is better for GObject integration since the NdbStore can propagate directly to the GNostrStore GError.

### R3: Migrate Services to GTask/GAsyncReadyCallback Pattern (MEDIUM)

Replace custom callback types with standard GLib async patterns:

```c
// Before:
void gnostr_mute_list_fetch_async(GNostrMuteList *self,
                                   const char *pubkey_hex,
                                   const char * const *relays,
                                   GNostrMuteListFetchCallback callback,
                                   gpointer user_data);

// After:
void gnostr_mute_list_fetch_async(GNostrMuteList *self,
                                   const char *pubkey_hex,
                                   const char * const *relays,
                                   GCancellable *cancellable,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data);

gboolean gnostr_mute_list_fetch_finish(GNostrMuteList *self,
                                        GAsyncResult *result,
                                        GError **error);
```

This makes the APIs:
1. GIR-introspectable with proper async annotations
2. Cancellable via GCancellable
3. Error-propagating via GError
4. Composable with GTask chains

### R4: Adopt g_autoptr(GError) Everywhere (LOW)

Convert all 33 `GError *error = NULL` to `g_autoptr(GError) error = NULL` in nostr-gobject/src/.

### R5: Standardize on NOSTR_ERROR Domain (LOW)

Replace `G_IO_ERROR` usage in `nostr_query_batcher.c` with `NOSTR_ERROR`.

---

## Priority Order

| # | Recommendation | Impact | Effort | Status |
|---|----------------|--------|--------|--------|
| R1 | nostr-gtk GError domain + 6 API additions | HIGH | 2-3 days | ✅ DONE |
| R2 | storage_ndb GError support | HIGH | 3-5 days | ✅ DONE |
| R3 | Service GTask migration | MEDIUM | 4-6 days | ✅ DONE |
| R4 | g_autoptr(GError) adoption | LOW | 0.5 days | ✅ DONE |
| R5 | Error domain standardization | LOW | 0.5 hours | ✅ DONE |

---

## Related Documents

- [`docs/GLIB_MEMORY_AUDIT.md`](GLIB_MEMORY_AUDIT.md) — GLib memory management patterns
- [`nostr-gobject/include/nostr-gobject-1.0/nostr-error.h`](../nostr-gobject/include/nostr-gobject-1.0/nostr-error.h) — Error domain definition
