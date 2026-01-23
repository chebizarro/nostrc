# Memory Management Patterns - gnostr-signer

This document describes memory management patterns, profiling tools, and best practices for the gnostr-signer application.

## Overview

gnostr-signer is a GTK4/Adwaita application that handles sensitive cryptographic operations. Memory management is critical for:

1. **Security** - Sensitive data (private keys, passwords) must be securely wiped from memory
2. **Performance** - Efficient memory usage for smooth UI responsiveness
3. **Reliability** - Preventing memory leaks that could exhaust resources

## Architecture

### Memory Components

The application tracks memory usage across several logical components:

| Component | Description | Module |
|-----------|-------------|--------|
| `GN_MEM_COMPONENT_CORE` | Core application infrastructure | `main_app.c` |
| `GN_MEM_COMPONENT_ACCOUNTS` | Account metadata storage | `accounts_store.c` |
| `GN_MEM_COMPONENT_SECRETS` | Secret key storage | `secret_store.c` |
| `GN_MEM_COMPONENT_SESSIONS` | Client session management | `client_session.c` |
| `GN_MEM_COMPONENT_POLICIES` | Permission policies | `policies/` |
| `GN_MEM_COMPONENT_UI` | UI widgets and components | `ui/` |
| `GN_MEM_COMPONENT_CACHE` | Caches (relay, profile, etc.) | `cache-manager.c` |
| `GN_MEM_COMPONENT_SECURE` | Secure memory allocations | `secure-mem.c` |

## Secure Memory (`secure-mem.c`)

### Purpose

Handles sensitive data (private keys, passwords, seeds) with:
- Memory locking via `mlock()` to prevent swapping
- Secure zeroing on deallocation
- Buffer overflow detection via canaries (debug builds)
- Integration with libsodium when available

### API

```c
// Secure allocation (zeros memory, optionally locks)
void *gnostr_secure_alloc(size_t size);
void gnostr_secure_free(void *ptr, size_t size);
void gnostr_secure_clear(void *ptr, size_t size);

// Secure strings
gchar *gnostr_secure_strdup(const char *str);
void gnostr_secure_strfree(gchar *str);

// Constant-time comparison (prevents timing attacks)
int gnostr_secure_memcmp(const void *a, const void *b, size_t size);
gboolean gnostr_secure_streq(const char *a, const char *b);
```

### Usage Pattern

```c
// Allocate secure memory for a private key
uint8_t *privkey = gnostr_secure_alloc(32);
if (!privkey) {
    // Handle allocation failure
    return;
}

// Use the key...
do_signing(privkey);

// ALWAYS free with the exact size
gnostr_secure_free(privkey, 32);
```

### Best Practices

1. **Always pair alloc/free with matching sizes**
2. **Never use regular `free()` on secure allocations**
3. **Clear sensitive data as soon as it's no longer needed**
4. **Use secure string functions for passwords and seeds**

## Memory Profiling (`memory-profile.c`)

### Purpose

Debug-build memory tracking to identify:
- Memory leaks
- High-memory components
- Cache efficiency
- Peak memory usage

### API

```c
// Initialize (called from main)
void gn_mem_profile_init(void);
void gn_mem_profile_shutdown(void);

// Track allocations
void gn_mem_profile_alloc(GnMemComponent component, gsize size);
void gn_mem_profile_free(GnMemComponent component, gsize size);

// Cache statistics
void gn_mem_profile_cache_hit(void);
void gn_mem_profile_cache_miss(void);

// Get statistics
GnMemStats gn_mem_profile_get_stats(void);
void gn_mem_profile_log_stats(const gchar *context);
```

### Convenience Macros

```c
// These compile to no-ops in release builds
GN_MEM_ALLOC(GN_MEM_COMPONENT_UI, size);
GN_MEM_FREE(GN_MEM_COMPONENT_UI, size);
GN_MEM_LOG("context");
GN_CACHE_HIT();
GN_CACHE_MISS();
```

### Periodic Reporting

In debug builds, statistics are logged every 60 seconds:

```
mem-profile [periodic]: current=1234567 peak=2345678 allocs=1000 frees=900 (elapsed: 120s)
  core: 50000 bytes, 10 allocations
  accounts: 8000 bytes, 5 allocations
  sessions: 12000 bytes, 3 allocations
  ui: 500000 bytes, 50 allocations
  cache: 200000 bytes, 100 entries, 85.0% hit rate (850/1000)
  Secure memory: 1024 bytes (peak 2048), mlock available
```

## Cache Management (`cache-manager.c`)

### Purpose

Generic LRU cache with:
- Configurable size limits (entry count and bytes)
- TTL-based expiration
- Multiple eviction policies
- Thread-safe operations

### API

```c
// Create a cache
GnCache *gn_cache_new(const gchar *name,
                      guint max_entries,
                      gsize max_bytes,
                      guint default_ttl_sec,
                      GnCacheValueFree value_free);

// Store/retrieve
void gn_cache_put(GnCache *cache, const gchar *key, gpointer value);
gpointer gn_cache_get(GnCache *cache, const gchar *key);

// Maintenance
guint gn_cache_expire(GnCache *cache);
guint gn_cache_evict(GnCache *cache, guint count);
void gn_cache_clear(GnCache *cache);
```

### Eviction Policies

- `GN_CACHE_EVICT_LRU` - Least Recently Used (default)
- `GN_CACHE_EVICT_LFU` - Least Frequently Used
- `GN_CACHE_EVICT_FIFO` - First In First Out
- `GN_CACHE_EVICT_TTL` - Time-based only

### Example Usage

```c
// Create a profile cache: 100 entries, 10MB, 5 min TTL
GnCache *profile_cache = gn_cache_new(
    "profiles",
    100,                    // max_entries
    10 * 1024 * 1024,       // max_bytes
    300,                    // ttl_sec
    (GnCacheValueFree)profile_free
);

// Enable memory tracking
gn_cache_set_value_size_func(profile_cache, profile_size_func);

// Use the cache
gn_cache_put(profile_cache, npub, profile);
Profile *p = gn_cache_get(profile_cache, npub);
```

## Lazy Loading (`ui/lazy-loader.c`)

### Purpose

Defer loading of heavy UI components to:
- Improve startup time
- Reduce initial memory footprint
- Support background preloading

### API

```c
// Register a component factory
void gn_lazy_register(const GnLazyConfig *config);

// Get (load on demand)
GtkWidget *gn_lazy_get(GnLazyComponent id);

// Preload in background
void gn_lazy_preload(GnLazyComponent id);

// Unload to free memory
void gn_lazy_unload(GnLazyComponent id);
```

### Registration Pattern

```c
// In module initialization
static GtkWidget *create_settings_page(void) {
    return GTK_WIDGET(page_settings_new());
}

void register_lazy_pages(void) {
    GnLazyConfig config = {
        .id = GN_LAZY_PAGE_SETTINGS,
        .name = "page-settings",
        .factory = create_settings_page,
        .unload_timeout_sec = 300,  // Unload after 5 min idle
        .preload_on_idle = TRUE,    // Preload after startup
        .estimated_size = 50000     // ~50KB
    };
    gn_lazy_register(&config);
}
```

## GObject Memory Patterns

### Reference Counting

```c
// Taking ownership
GtkWidget *widget = create_widget();  // You own it
gtk_container_add(container, widget); // Container now owns it

// Borrowing
GtkWidget *child = gtk_widget_get_first_child(parent);  // Don't unref!

// Explicit ownership transfer
g_object_ref(widget);                 // You now own a reference
// ... later ...
g_object_unref(widget);               // Release your reference
```

### Signal Handler Cleanup

```c
struct _MyWidget {
    gulong signal_handler_id;
};

// Connect
self->signal_handler_id = g_signal_connect(source, "signal",
                                            G_CALLBACK(handler), self);

// Cleanup in dispose
static void my_widget_dispose(GObject *obj) {
    MyWidget *self = MY_WIDGET(obj);

    if (self->signal_handler_id > 0) {
        g_signal_handler_disconnect(source, self->signal_handler_id);
        self->signal_handler_id = 0;
    }

    G_OBJECT_CLASS(my_widget_parent_class)->dispose(obj);
}
```

### Using g_clear_* Functions

```c
// Preferred: clears and nullifies in one call
g_clear_pointer(&self->data, g_free);
g_clear_object(&self->settings);

// Equivalent to:
if (self->data) {
    g_free(self->data);
    self->data = NULL;
}
```

## Hash Table Memory Management

### Ownership Transfer

```c
// Hash table owns keys and values
GHashTable *map = g_hash_table_new_full(
    g_str_hash,           // key hash
    g_str_equal,          // key equal
    g_free,               // key destroy
    g_free                // value destroy
);

// Keys/values are copied - hash table will free them
g_hash_table_insert(map, g_strdup(key), g_strdup(value));
```

### Avoiding Unnecessary Copies

```c
// Lookup without copy
gpointer value = g_hash_table_lookup(table, key);  // Don't free!

// Remove without destroying
gboolean removed = g_hash_table_steal(table, key);  // You now own it
```

## Common Memory Issues and Solutions

### Issue 1: String Duplication

**Problem:** Excessive string copies

```c
// Inefficient
char *name = get_name();
char *display = g_strdup_printf("Hello, %s", name);  // Copies name
use(display);
g_free(display);
```

**Solution:** Use string builders for complex cases

```c
GString *str = g_string_new("Hello, ");
g_string_append(str, name);
use(str->str);
g_string_free(str, TRUE);
```

### Issue 2: Widget Memory

**Problem:** UI widgets consuming memory when not visible

**Solution:** Use lazy loading

```c
// Instead of creating all pages at startup
// Only create when first accessed
GtkWidget *page = gn_lazy_get(GN_LAZY_PAGE_SETTINGS);
```

### Issue 3: Cache Bloat

**Problem:** Caches growing unbounded

**Solution:** Configure limits and TTL

```c
GnCache *cache = gn_cache_new("data",
    1000,           // max 1000 entries
    50 * 1024 * 1024,  // max 50MB
    600,            // 10 minute TTL
    free_func);
```

### Issue 4: Session Accumulation

**Problem:** Client sessions not being cleaned up

**Solution:** Use session manager cleanup

```c
// Periodic cleanup of expired sessions
guint cleaned = gn_client_session_manager_cleanup_expired(mgr);
```

## Debugging Memory Issues

### 1. Enable Debug Profiling

```bash
# Build with debug
meson setup build --buildtype=debug
ninja -C build

# Run with memory logging
G_MESSAGES_DEBUG=mem-profile ./build/apps/gnostr-signer/gnostr-signer
```

### 2. Use Valgrind

```bash
valgrind --leak-check=full \
         --show-leak-kinds=definite \
         --track-origins=yes \
         ./build/apps/gnostr-signer/gnostr-signer
```

### 3. Use AddressSanitizer

```bash
# Configure with ASan
meson setup build -Db_sanitize=address

# Run - will report issues at runtime
./build/apps/gnostr-signer/gnostr-signer
```

### 4. Check Secure Memory Stats

```c
GnostrSecureMemStats stats = gnostr_secure_mem_get_stats();
g_print("Secure memory: %zu allocated, %zu locked, peak %zu\n",
        stats.total_allocated, stats.total_locked, stats.peak_allocated);
```

## Performance Guidelines

1. **Startup**: Initialize only essential components synchronously
2. **Pages**: Lazy-load UI pages on first access
3. **Data**: Load data asynchronously with progress indication
4. **Caches**: Configure appropriate size limits based on use case
5. **Sessions**: Clean up expired sessions periodically
6. **Secrets**: Free secure memory immediately after use

## Related Files

- `src/memory-profile.h` - Memory profiling API
- `src/memory-profile.c` - Profiling implementation
- `src/secure-mem.h` - Secure memory API
- `src/secure-mem.c` - Secure memory implementation
- `src/cache-manager.h` - Cache API
- `src/cache-manager.c` - Cache implementation
- `src/ui/lazy-loader.h` - Lazy loading API
- `src/ui/lazy-loader.c` - Lazy loading implementation
