/* secure-mem.h - Secure memory utilities for gnostr-signer
 *
 * Provides secure memory allocation, deallocation, and string handling
 * for sensitive data like private keys (nsec), passwords, passphrases,
 * decrypted content, and session tokens.
 *
 * Features:
 * - Memory locked in RAM via mlock() to prevent swapping to disk
 * - Secure zeroing via explicit_bzero() that won't be optimized away
 * - Guard pages in debug builds for overflow detection
 * - Integration with libsodium if available
 *
 * This is the simplified API for gnostr-signer. For the full-featured
 * GnSecureString type, see secure-memory.h.
 *
 * Security Notes:
 * - Always use gnostr_secure_free() to release memory from gnostr_secure_alloc()
 * - Always use gnostr_secure_strfree() to release strings from gnostr_secure_strdup()
 * - Size parameters must be accurate for secure zeroing to work correctly
 * - mlock() may fail without elevated privileges (non-fatal warning)
 */
#ifndef APPS_GNOSTR_SIGNER_SECURE_MEM_H
#define APPS_GNOSTR_SIGNER_SECURE_MEM_H

#include <glib.h>
#include <stddef.h>
#include <stdbool.h>

G_BEGIN_DECLS

/* ============================================================
 * Initialization
 * ============================================================ */

/**
 * gnostr_secure_mem_init:
 *
 * Initialize the secure memory subsystem.
 * Called automatically on first allocation if not called explicitly.
 * Safe to call multiple times.
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean gnostr_secure_mem_init(void);

/**
 * gnostr_secure_mem_shutdown:
 *
 * Shutdown the secure memory subsystem.
 * Securely zeros and frees all remaining allocations.
 * Call this during application exit for clean shutdown.
 */
void gnostr_secure_mem_shutdown(void);

/* ============================================================
 * Core Memory Operations
 * ============================================================ */

/**
 * gnostr_secure_alloc:
 * @size: Number of bytes to allocate
 *
 * Allocate secure memory for sensitive data.
 *
 * The allocated memory is:
 * - Zero-initialized
 * - Locked in RAM (mlock) to prevent swapping to disk
 * - Protected by guard canaries in debug builds
 *
 * IMPORTANT: Always free with gnostr_secure_free(), never with free() or g_free()
 *
 * Returns: Pointer to allocated memory, or NULL on failure
 */
void *gnostr_secure_alloc(size_t size);

/**
 * gnostr_secure_free:
 * @ptr: Pointer from gnostr_secure_alloc (NULL is safe)
 * @size: Size of allocation (must match original allocation)
 *
 * Free secure memory, securely zeroing it first.
 *
 * Uses explicit_bzero() (or equivalent) to ensure the memory is
 * actually zeroed before being freed, preventing compiler optimization
 * from removing the zeroing operation.
 *
 * Safe to call with NULL pointer.
 */
void gnostr_secure_free(void *ptr, size_t size);

/**
 * gnostr_secure_clear:
 * @ptr: Memory to zero (NULL is safe)
 * @size: Number of bytes to zero
 *
 * Securely zero memory without freeing it.
 *
 * This is an explicit_bzero() wrapper that ensures the compiler
 * doesn't optimize away the zeroing operation.
 *
 * Use this for:
 * - Stack-allocated buffers containing sensitive data
 * - Clearing data before reuse
 * - Zeroing temporaries
 *
 * Safe to call with NULL pointer or zero size.
 */
void gnostr_secure_clear(void *ptr, size_t size);

/* ============================================================
 * Secure String Handling
 * ============================================================ */

/**
 * gnostr_secure_strdup:
 * @str: Source string (can be NULL)
 *
 * Duplicate a string into secure memory.
 *
 * The returned string is stored in locked memory that won't be
 * swapped to disk.
 *
 * IMPORTANT: Free with gnostr_secure_strfree(), never with g_free() or free()
 *
 * Returns: New string in secure memory, or NULL if @str is NULL or on failure
 */
gchar *gnostr_secure_strdup(const char *str);

/**
 * gnostr_secure_strndup:
 * @str: Source string (can be NULL)
 * @n: Maximum number of characters to copy
 *
 * Duplicate up to @n characters of a string into secure memory.
 *
 * Returns: New null-terminated string in secure memory, or NULL
 */
gchar *gnostr_secure_strndup(const char *str, size_t n);

/**
 * gnostr_secure_strfree:
 * @str: String from gnostr_secure_strdup (NULL is safe)
 *
 * Free a secure string, securely clearing it first.
 *
 * The string content is zeroed using explicit_bzero() before
 * the memory is freed.
 *
 * Safe to call with NULL.
 */
void gnostr_secure_strfree(gchar *str);

/* ============================================================
 * Memory Locking
 * ============================================================ */

/**
 * gnostr_secure_mlock:
 * @ptr: Memory to lock
 * @size: Size of memory region
 *
 * Lock memory region to prevent it from being swapped to disk.
 *
 * This is useful for locking memory that wasn't allocated with
 * gnostr_secure_alloc() (e.g., stack buffers, existing heap allocations).
 *
 * Note: May require elevated privileges. Failure is typically non-fatal
 * and logged as a warning.
 *
 * Returns: TRUE if mlock succeeded, FALSE otherwise
 */
gboolean gnostr_secure_mlock(void *ptr, size_t size);

/**
 * gnostr_secure_munlock:
 * @ptr: Memory to unlock
 * @size: Size of memory region
 *
 * Unlock a previously locked memory region.
 *
 * Should be called before freeing memory that was locked with
 * gnostr_secure_mlock() (not needed for gnostr_secure_alloc() memory).
 */
void gnostr_secure_munlock(void *ptr, size_t size);

/**
 * gnostr_secure_mlock_available:
 *
 * Check if mlock() is available and working on this system.
 *
 * Returns: TRUE if mlock is functional, FALSE otherwise
 */
gboolean gnostr_secure_mlock_available(void);

/* ============================================================
 * Constant-Time Operations
 * ============================================================ */

/**
 * gnostr_secure_memcmp:
 * @a: First memory region
 * @b: Second memory region
 * @size: Number of bytes to compare
 *
 * Constant-time memory comparison.
 *
 * Compares two memory regions in constant time to prevent timing
 * side-channel attacks. Unlike memcmp(), this function does NOT
 * return early on first difference.
 *
 * Returns: 0 if memory regions are equal, non-zero if different
 *          (does NOT indicate which is "greater")
 */
int gnostr_secure_memcmp(const void *a, const void *b, size_t size);

/**
 * gnostr_secure_streq:
 * @a: First string (can be NULL)
 * @b: Second string (can be NULL)
 *
 * Constant-time string comparison.
 *
 * Compares two null-terminated strings in constant time.
 * Returns FALSE immediately if lengths differ (length is not secret).
 *
 * Returns: TRUE if strings are equal, FALSE otherwise
 */
gboolean gnostr_secure_streq(const char *a, const char *b);

/* ============================================================
 * Convenience Macros
 * ============================================================ */

/**
 * GNOSTR_SECURE_CLEAR_BUFFER:
 * @buf: Buffer (must be array, not pointer)
 *
 * Securely zero a stack-allocated buffer.
 * Only works with arrays where sizeof() gives the actual size.
 */
#define GNOSTR_SECURE_CLEAR_BUFFER(buf) \
    gnostr_secure_clear((buf), sizeof(buf))

/**
 * GNOSTR_SECURE_FREE_STRING:
 * @str: String pointer (will be set to NULL)
 *
 * Securely clear and free a string, then set pointer to NULL.
 * Works with any allocated string (not just gnostr_secure_strdup).
 */
#define GNOSTR_SECURE_FREE_STRING(str) \
    G_STMT_START { \
        if ((str) != NULL) { \
            gnostr_secure_clear((str), strlen(str)); \
            g_free(str); \
            (str) = NULL; \
        } \
    } G_STMT_END

/**
 * GNOSTR_SECURE_STRFREE:
 * @str: Secure string pointer (will be set to NULL)
 *
 * Free a secure string and set pointer to NULL.
 * Use this for strings from gnostr_secure_strdup().
 */
#define GNOSTR_SECURE_STRFREE(str) \
    G_STMT_START { \
        gnostr_secure_strfree(str); \
        (str) = NULL; \
    } G_STMT_END

/* ============================================================
 * Guard Page Support
 * ============================================================ */

/**
 * GnostrGuardPageMode:
 * @GNOSTR_GUARD_NONE: No guard pages (minimum overhead)
 * @GNOSTR_GUARD_CANARY: Use canary values for overflow detection (default in debug)
 * @GNOSTR_GUARD_PAGES: Use mprotected guard pages (most secure, higher overhead)
 *
 * Guard page modes for detecting buffer overflows/underflows.
 */
typedef enum {
    GNOSTR_GUARD_NONE = 0,
    GNOSTR_GUARD_CANARY = 1,
    GNOSTR_GUARD_PAGES = 2
} GnostrGuardPageMode;

/**
 * gnostr_secure_set_guard_mode:
 * @mode: The guard page mode to use
 *
 * Set the guard page mode for secure allocations.
 * Must be called before any allocations are made.
 *
 * In GNOSTR_GUARD_PAGES mode:
 * - Each allocation is surrounded by inaccessible guard pages
 * - Any buffer overflow/underflow causes immediate SIGSEGV
 * - Higher memory overhead due to page alignment requirements
 */
void gnostr_secure_set_guard_mode(GnostrGuardPageMode mode);

/**
 * gnostr_secure_get_guard_mode:
 *
 * Get the current guard page mode.
 *
 * Returns: Current guard page mode
 */
GnostrGuardPageMode gnostr_secure_get_guard_mode(void);

/**
 * gnostr_secure_alloc_guarded:
 * @size: Number of bytes to allocate
 *
 * Allocate secure memory with explicit guard pages.
 * This function always uses guard pages regardless of the global mode.
 *
 * The allocation layout (with guard pages):
 *   [GUARD PAGE][HEADER][USER DATA (aligned)][PADDING][GUARD PAGE]
 *
 * Any access to guard pages causes immediate SIGSEGV, providing
 * instant detection of buffer overflows and underflows.
 *
 * Returns: Pointer to allocated memory, or NULL on failure
 */
void *gnostr_secure_alloc_guarded(size_t size);

/**
 * gnostr_secure_free_guarded:
 * @ptr: Pointer from gnostr_secure_alloc_guarded
 * @size: Size of allocation
 *
 * Free guarded secure memory.
 */
void gnostr_secure_free_guarded(void *ptr, size_t size);

/* ============================================================
 * Statistics and Debugging
 * ============================================================ */

/**
 * GnostrSecureMemStats:
 * @total_allocated: Total bytes currently allocated
 * @total_locked: Total bytes successfully locked in memory
 * @allocation_count: Number of active allocations
 * @peak_allocated: Peak memory usage
 * @mlock_available: Whether mlock is working
 * @sodium_available: Whether libsodium is being used
 * @guard_mode: Current guard page mode
 * @guard_violations: Number of guard violations detected (canary mode only)
 *
 * Statistics about secure memory usage.
 */
typedef struct {
    size_t total_allocated;
    size_t total_locked;
    size_t allocation_count;
    size_t peak_allocated;
    gboolean mlock_available;
    gboolean sodium_available;
    GnostrGuardPageMode guard_mode;
    size_t guard_violations;
} GnostrSecureMemStats;

/**
 * gnostr_secure_mem_get_stats:
 *
 * Get statistics about secure memory usage.
 *
 * Returns: Current statistics
 */
GnostrSecureMemStats gnostr_secure_mem_get_stats(void);

/**
 * gnostr_secure_mem_dump_stats:
 *
 * Print secure memory statistics to debug output.
 * Useful for debugging memory leaks of sensitive data.
 */
void gnostr_secure_mem_dump_stats(void);

/**
 * gnostr_secure_check_guards:
 *
 * Verify all guard canaries are intact.
 * Only useful in GNOSTR_GUARD_CANARY mode.
 *
 * Returns: TRUE if all guards are valid, FALSE if corruption detected
 */
gboolean gnostr_secure_check_guards(void);

/* ============================================================
 * Secure Buffer Operations
 * ============================================================ */

/**
 * gnostr_secure_copy:
 * @dest: Destination buffer (must be in secure memory or locked)
 * @src: Source buffer
 * @size: Number of bytes to copy
 *
 * Copy data into secure memory.
 * This is a secure version of memcpy that ensures the source
 * data is properly cleared from non-secure locations afterward.
 */
void gnostr_secure_copy(void *dest, const void *src, size_t size);

/**
 * gnostr_secure_concat:
 * @s1: First string (can be NULL)
 * @s2: Second string (can be NULL)
 *
 * Concatenate two strings in secure memory.
 *
 * Returns: New string in secure memory, or NULL
 */
gchar *gnostr_secure_concat(const char *s1, const char *s2);

/**
 * gnostr_secure_sprintf:
 * @format: printf-style format string
 * @...: Format arguments
 *
 * Format a string in secure memory.
 *
 * Returns: New string in secure memory, or NULL on failure
 */
gchar *gnostr_secure_sprintf(const char *format, ...) G_GNUC_PRINTF(1, 2);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_SECURE_MEM_H */
