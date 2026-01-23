# Secure Memory Management

This document describes the secure memory management APIs and patterns used in
gnostr-signer to protect sensitive data such as private keys, passwords, and
session tokens.

Issue: nostrc-ycd

## Overview

Secure memory management is critical for applications handling cryptographic
keys and secrets. The goals are:

1. **Prevent swapping**: Lock sensitive memory in RAM to prevent it from being
   written to disk swap space where it could be recovered.

2. **Secure zeroing**: Ensure memory is properly zeroed before being freed,
   using techniques that prevent compiler optimizations from removing the
   zeroing operation.

3. **Buffer overflow detection**: Use guard pages or canary values to detect
   memory corruption that could leak secrets.

4. **Constant-time operations**: Prevent timing side-channel attacks when
   comparing secrets.

## APIs

### secure-mem.h (Simplified API)

The primary API for secure memory operations with `gnostr_secure_*` prefix:

```c
#include "secure-mem.h"

// Initialize/shutdown
gboolean gnostr_secure_mem_init(void);
void gnostr_secure_mem_shutdown(void);

// Core allocation
void *gnostr_secure_alloc(size_t size);
void gnostr_secure_free(void *ptr, size_t size);
void gnostr_secure_clear(void *ptr, size_t size);

// String operations
gchar *gnostr_secure_strdup(const char *str);
gchar *gnostr_secure_strndup(const char *str, size_t n);
void gnostr_secure_strfree(gchar *str);

// Memory locking
gboolean gnostr_secure_mlock(void *ptr, size_t size);
void gnostr_secure_munlock(void *ptr, size_t size);

// Constant-time comparison
int gnostr_secure_memcmp(const void *a, const void *b, size_t size);
gboolean gnostr_secure_streq(const char *a, const char *b);

// Guard page allocation (explicit)
void *gnostr_secure_alloc_guarded(size_t size);
void gnostr_secure_free_guarded(void *ptr, size_t size);

// Utility functions
gchar *gnostr_secure_concat(const char *s1, const char *s2);
gchar *gnostr_secure_sprintf(const char *format, ...);
```

### secure-memory.h (Full-featured API)

A more complete API with `gn_secure_*` prefix and `GnSecureString` type:

```c
#include "secure-memory.h"

// Initialization with guard mode
GnSecureResult gn_secure_init(GnGuardMode guard_mode);
void gn_secure_shutdown(void);

// Allocation with realloc support
void *gn_secure_alloc(size_t size);
void *gn_secure_realloc(void *ptr, size_t old_size, size_t new_size);
void gn_secure_free(void *ptr, size_t size);

// GnSecureString wrapper type
GnSecureString *gn_secure_string_new(const char *str);
const char *gn_secure_string_get(const GnSecureString *ss);
void gn_secure_string_free(GnSecureString *ss);
```

## Usage Patterns

### Pattern 1: Allocating and Freeing Secure Memory

```c
// Allocate secure memory for a private key
guint8 *privkey = gnostr_secure_alloc(32);
if (!privkey) {
    // Handle allocation failure
    return;
}

// Use the key...
do_signing_operation(privkey);

// IMPORTANT: Always free with matching size
gnostr_secure_free(privkey, 32);
```

### Pattern 2: Secure String Handling

```c
// Duplicate a password into secure memory
gchar *password = gnostr_secure_strdup(user_input);

// Use the password...
verify_password(password);

// Free securely (automatically clears before free)
gnostr_secure_strfree(password);
```

### Pattern 3: Clearing Stack Buffers

```c
void process_key(const guint8 *key) {
    guint8 local_copy[32];
    memcpy(local_copy, key, 32);

    // Do work with local_copy...
    do_operation(local_copy);

    // CRITICAL: Clear before function returns
    GNOSTR_SECURE_CLEAR_BUFFER(local_copy);
}
```

### Pattern 4: Locking External Memory

```c
// Lock a buffer that wasn't allocated with secure_alloc
guint8 buffer[1024];
if (gnostr_secure_mlock(buffer, sizeof(buffer))) {
    // Use buffer for sensitive data...

    // Clear and unlock before going out of scope
    gnostr_secure_clear(buffer, sizeof(buffer));
    gnostr_secure_munlock(buffer, sizeof(buffer));
}
```

### Pattern 5: Constant-Time Password Comparison

```c
gboolean verify_password(const char *input, const char *stored_hash) {
    char *input_hash = compute_hash(input);

    // WRONG: g_strcmp0 can leak length via timing
    // gboolean match = (g_strcmp0(input_hash, stored_hash) == 0);

    // CORRECT: Use constant-time comparison
    gboolean match = gnostr_secure_streq(input_hash, stored_hash);

    gnostr_secure_clear(input_hash, strlen(input_hash));
    g_free(input_hash);
    return match;
}
```

### Pattern 6: Using Guard Pages for Debugging

```c
// Enable guard page mode at startup (before any allocations)
gnostr_secure_set_guard_mode(GNOSTR_GUARD_PAGES);

// Allocations now have guard pages - any overflow causes SIGSEGV
void *ptr = gnostr_secure_alloc_guarded(256);

// Or use regular alloc with global guard mode
void *ptr2 = gnostr_secure_alloc(256);
```

## Guard Modes

Three guard modes are available:

| Mode | Description | Overhead | Security |
|------|-------------|----------|----------|
| `GNOSTR_GUARD_NONE` | No guards (release mode) | Lowest | Basic |
| `GNOSTR_GUARD_CANARY` | Canary values at boundaries | Low | Good |
| `GNOSTR_GUARD_PAGES` | Inaccessible guard pages | High | Best |

- **CANARY**: Detects overflows at free time via magic number verification
- **PAGES**: Detects overflows immediately via SIGSEGV (page fault)

## Implementation Details

### Memory Locking (mlock)

The implementation uses:
- **Unix/Linux**: `mlock()` / `munlock()` syscalls
- **macOS**: `mlock()` / `munlock()` (same API)
- **Windows**: `VirtualLock()` / `VirtualUnlock()`

Note: `mlock` may require elevated privileges (CAP_IPC_LOCK on Linux) or may
be limited by `ulimit -l`. The implementation handles failures gracefully.

### Secure Zeroing

Priority order:
1. `sodium_memzero()` (libsodium, when available)
2. `memset_s()` (C11 Annex K, macOS)
3. `explicit_bzero()` (BSD, glibc 2.25+)
4. Volatile pointer technique with memory barrier

### Guard Pages

When `GNOSTR_GUARD_PAGES` is enabled, allocations use `mmap` with layout:

```
[PROT_NONE guard][header + user data][PROT_NONE guard]
```

Any access to guard pages causes immediate SIGSEGV.

### libsodium Integration

When libsodium is available (`GNOSTR_HAVE_SODIUM` defined):
- Uses `sodium_malloc()` for allocation (automatic mlock + guard pages)
- Uses `sodium_free()` for deallocation (automatic zeroing)
- Uses `sodium_memzero()` for secure clearing
- Uses `sodium_memcmp()` for constant-time comparison

## Best Practices

1. **Always match allocation and free functions**:
   - `gnostr_secure_alloc` -> `gnostr_secure_free`
   - `gnostr_secure_strdup` -> `gnostr_secure_strfree`
   - `gnostr_secure_alloc_guarded` -> `gnostr_secure_free_guarded`

2. **Always pass the correct size to free**:
   ```c
   void *ptr = gnostr_secure_alloc(64);
   gnostr_secure_free(ptr, 64);  // Size must match!
   ```

3. **Clear temporaries on the stack**:
   ```c
   guint8 temp[32];
   // ... use temp ...
   GNOSTR_SECURE_CLEAR_BUFFER(temp);  // Before scope exit
   ```

4. **Use GNOSTR_SECURE_STRFREE macro** for automatic NULL assignment:
   ```c
   gchar *str = gnostr_secure_strdup("secret");
   GNOSTR_SECURE_STRFREE(str);  // str is now NULL
   ```

5. **Initialize early**: Call `gnostr_secure_mem_init()` at application startup.

6. **Shutdown cleanly**: Call `gnostr_secure_mem_shutdown()` at exit to ensure
   all remaining allocations are securely zeroed.

7. **Check statistics in debug builds**:
   ```c
   #ifndef NDEBUG
   gnostr_secure_mem_dump_stats();
   g_assert(gnostr_secure_check_guards());
   #endif
   ```

## Files

| File | Description |
|------|-------------|
| `src/secure-mem.h` | Simplified secure memory API |
| `src/secure-mem.c` | Implementation with mlock/guard pages |
| `src/secure-memory.h` | Full-featured API with GnSecureString |
| `src/secure-memory.c` | Full-featured implementation |
| `tests/test-secure-mem.c` | Unit tests for secure memory |

## See Also

- [libsodium Secure Memory](https://doc.libsodium.org/memory_management)
- [OpenSSL OPENSSL_cleanse](https://www.openssl.org/docs/man3.0/man3/OPENSSL_cleanse.html)
- Linux `mlock(2)` man page
