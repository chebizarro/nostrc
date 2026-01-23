/* secure-memory.h - Secure memory management for sensitive data
 *
 * This module provides secure memory allocation and handling functions
 * for sensitive data like private keys, passwords, and passphrases.
 *
 * Features:
 * - Memory that won't be swapped to disk (mlock)
 * - Secure zeroing that won't be optimized away
 * - Constant-time comparison to prevent timing attacks
 * - Memory guards to detect buffer overflows (debug builds)
 * - GnSecureString wrapper for automatic cleanup
 *
 * Uses libsodium if available, with fallback to system APIs.
 */
#pragma once

#include <glib.h>
#include <glib-object.h>
#include <stddef.h>
#include <stdbool.h>

G_BEGIN_DECLS

/* Result codes for secure memory operations */
typedef enum {
  GN_SECURE_OK = 0,
  GN_SECURE_ERR_ALLOC,      /* Memory allocation failed */
  GN_SECURE_ERR_MLOCK,      /* Failed to lock memory (non-fatal) */
  GN_SECURE_ERR_INVALID,    /* Invalid parameter */
  GN_SECURE_ERR_OVERFLOW    /* Buffer overflow detected (debug) */
} GnSecureResult;

/* Memory guard configuration (for debug builds) */
typedef enum {
  GN_GUARD_NONE = 0,        /* No guards (production) */
  GN_GUARD_CANARY = 1,      /* Canary values at buffer boundaries */
  GN_GUARD_PAGE = 2         /* Guard pages (expensive but thorough) */
} GnGuardMode;

/* Secure memory statistics */
typedef struct {
  size_t total_allocated;   /* Total bytes allocated */
  size_t total_locked;      /* Total bytes locked in memory */
  size_t allocation_count;  /* Number of active allocations */
  size_t peak_allocated;    /* Peak memory usage */
  bool sodium_available;    /* libsodium is being used */
  bool mlock_available;     /* mlock is working */
} GnSecureStats;

/* Initialize secure memory subsystem
 * Call this early in application startup.
 * @guard_mode: Guard mode for debug builds (ignored in release)
 * Returns: GN_SECURE_OK on success
 */
GnSecureResult gn_secure_init(GnGuardMode guard_mode);

/* Shutdown secure memory subsystem
 * Zeros and frees all remaining allocations.
 * Call this before application exit.
 */
void gn_secure_shutdown(void);

/* Get secure memory statistics */
GnSecureStats gn_secure_get_stats(void);

/* Allocate secure memory
 * Memory is:
 * - Locked in RAM (won't be swapped)
 * - Zero-initialized
 * - Protected by guards in debug builds
 *
 * @size: Number of bytes to allocate
 * Returns: Pointer to allocated memory, or NULL on failure
 *
 * IMPORTANT: Always free with gn_secure_free(), never with free()
 */
void *gn_secure_alloc(size_t size);

/* Reallocate secure memory
 * @ptr: Pointer from gn_secure_alloc (or NULL for new allocation)
 * @old_size: Current size (must be accurate for secure zeroing)
 * @new_size: Desired new size
 * Returns: New pointer, or NULL on failure (original unchanged)
 */
void *gn_secure_realloc(void *ptr, size_t old_size, size_t new_size);

/* Free secure memory
 * Securely zeros memory before freeing.
 *
 * @ptr: Pointer from gn_secure_alloc (NULL is safe)
 * @size: Size of allocation (must match original allocation)
 */
void gn_secure_free(void *ptr, size_t size);

/* Securely zero memory
 * Uses a technique that prevents compiler optimization from
 * removing the zeroing operation.
 *
 * Prefers:
 * 1. sodium_memzero() if libsodium available
 * 2. explicit_bzero() if available
 * 3. volatile memory barrier technique
 *
 * @ptr: Memory to zero
 * @size: Number of bytes to zero
 */
void gn_secure_zero(void *ptr, size_t size);

/* Constant-time memory comparison
 * Compares two memory regions in constant time to prevent
 * timing side-channel attacks.
 *
 * @a: First memory region
 * @b: Second memory region
 * @size: Number of bytes to compare
 * Returns: 0 if equal, non-zero if different
 *
 * Note: Unlike memcmp, does NOT indicate which is "greater"
 */
int gn_secure_memcmp(const void *a, const void *b, size_t size);

/* Duplicate string in secure memory
 * @str: Source string (can be NULL)
 * Returns: New string in secure memory, or NULL
 *
 * Free with gn_secure_strfree()
 */
char *gn_secure_strdup(const char *str);

/* Free secure string
 * @str: String from gn_secure_strdup (NULL is safe)
 */
void gn_secure_strfree(char *str);

/* Secure string length
 * Returns length of string, or 0 if NULL
 */
size_t gn_secure_strlen(const char *str);

/* ============================================================
 * GnSecureString - Wrapper type for sensitive strings
 * ============================================================
 *
 * Usage:
 *   GnSecureString *ss = gn_secure_string_new("password123");
 *   const char *ptr = gn_secure_string_get(ss);
 *   // Use ptr...
 *   gn_secure_string_free(ss);  // Automatically zeros memory
 *
 * Features:
 * - Automatically zeros on destruction
 * - Prevents accidental copying
 * - Reference counting for safe sharing
 * - Integrates with GLib memory management
 */

typedef struct _GnSecureString GnSecureString;

/* Create a new secure string from a C string
 * @str: Source string (will be copied; original can be freed)
 * Returns: New GnSecureString, or NULL on failure
 */
GnSecureString *gn_secure_string_new(const char *str);

/* Create a new secure string with a specific length
 * Useful for binary data or strings without null terminator.
 * @data: Source data
 * @len: Length in bytes
 * Returns: New GnSecureString (will be null-terminated)
 */
GnSecureString *gn_secure_string_new_len(const char *data, size_t len);

/* Create an empty secure string with reserved capacity
 * @capacity: Initial capacity (not including null terminator)
 */
GnSecureString *gn_secure_string_new_empty(size_t capacity);

/* Get the C string pointer
 * The returned pointer is valid until the GnSecureString is freed.
 * @ss: Secure string (can be NULL)
 * Returns: C string pointer, or NULL
 */
const char *gn_secure_string_get(const GnSecureString *ss);

/* Get the length of the secure string
 * @ss: Secure string (can be NULL)
 * Returns: Length in bytes, or 0
 */
size_t gn_secure_string_len(const GnSecureString *ss);

/* Check if secure string is empty or NULL */
bool gn_secure_string_is_empty(const GnSecureString *ss);

/* Append to secure string
 * @ss: Secure string to append to
 * @str: String to append
 * Returns: GN_SECURE_OK on success
 */
GnSecureResult gn_secure_string_append(GnSecureString *ss, const char *str);

/* Append a single character */
GnSecureResult gn_secure_string_append_c(GnSecureString *ss, char c);

/* Clear the contents (zeros memory but keeps allocation) */
void gn_secure_string_clear(GnSecureString *ss);

/* Increment reference count
 * Use when you need to share a secure string.
 * @ss: Secure string to reference
 * Returns: The same pointer (for convenience)
 */
GnSecureString *gn_secure_string_ref(GnSecureString *ss);

/* Decrement reference count and free if zero
 * @ss: Secure string (NULL is safe)
 */
void gn_secure_string_unref(GnSecureString *ss);

/* Convenience alias for unref */
#define gn_secure_string_free(ss) gn_secure_string_unref(ss)

/* Compare two secure strings in constant time
 * Returns: true if equal, false otherwise
 */
bool gn_secure_string_equal(const GnSecureString *a, const GnSecureString *b);

/* Steal the underlying buffer
 * Returns the raw pointer and clears the GnSecureString.
 * The caller is responsible for calling gn_secure_free() on the result.
 * @ss: Secure string
 * @out_len: Output parameter for length (can be NULL)
 * Returns: The buffer, or NULL if empty
 */
char *gn_secure_string_steal(GnSecureString *ss, size_t *out_len);

/* ============================================================
 * GLib Integration
 * ============================================================ */

/* GnSecureString is a boxed type for GLib */
#define GN_TYPE_SECURE_STRING (gn_secure_string_get_type())
GType gn_secure_string_get_type(void);

/* Auto-cleanup support for GnSecureString with g_autoptr */
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GnSecureString, gn_secure_string_unref)

/* ============================================================
 * Utility Macros
 * ============================================================ */

/* Declare a local secure string that auto-frees at scope exit */
#define GN_AUTO_SECURE_STRING g_autoptr(GnSecureString)

/* Zero and free a regular (non-secure) string
 * Useful for cleaning up strings before switching to secure versions.
 * @str: String to zero and free (modified to NULL)
 */
#define gn_secure_clear_string(str) \
  G_STMT_START { \
    if ((str) != NULL) { \
      gn_secure_zero((str), strlen(str)); \
      g_free(str); \
      (str) = NULL; \
    } \
  } G_STMT_END

/* Zero a stack-allocated buffer
 * @buf: Buffer (must be array, not pointer)
 */
#define gn_secure_clear_buffer(buf) \
  gn_secure_zero((buf), sizeof(buf))

G_END_DECLS
