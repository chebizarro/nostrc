/* secure-mem.c - Secure memory utilities implementation
 *
 * This implementation provides secure memory handling with:
 * - libsodium integration when available
 * - Fallback to system mlock/explicit_bzero
 * - Memory guards for debug builds
 * - Thread-safe allocation tracking
 */

#include "secure-mem.h"
#include <string.h>
#include <stdlib.h>

#ifdef __APPLE__
#include <sys/mman.h>
#include <mach/mach.h>
#else
#include <sys/mman.h>
#endif

/* Check for libsodium */
#ifdef GNOSTR_HAVE_SODIUM
#include <sodium.h>
#define USE_SODIUM 1
#else
#define USE_SODIUM 0
#endif

/* Check for explicit_bzero availability */
#ifdef __APPLE__
#include <string.h>
/* macOS has memset_s in string.h */
#define HAVE_MEMSET_S 1
#elif defined(__linux__)
/* Linux glibc 2.25+ has explicit_bzero */
#include <features.h>
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 25))
#define HAVE_EXPLICIT_BZERO 1
#endif
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#define HAVE_EXPLICIT_BZERO 1
#endif

/* Guard canary values for debug builds */
#ifndef NDEBUG
#define CANARY_HEAD_MAGIC 0xDEADBEEFCAFEBABEULL
#define CANARY_TAIL_MAGIC 0xFEEDFACE12345678ULL
#define CANARY_SIZE 16
#define USE_CANARIES 1
#else
#define CANARY_SIZE 0
#define USE_CANARIES 0
#endif

/* Allocation header for tracking */
typedef struct {
    size_t size;            /* Requested size */
    size_t actual_size;     /* Actual allocation size (with guards) */
    gboolean locked;        /* Successfully mlocked */
#if USE_CANARIES
    guint64 head_canary;    /* Overflow detection */
#endif
} SecureAllocHeader;

/* Module state */
static struct {
    gboolean initialized;
    GMutex lock;
    GnostrSecureMemStats stats;
    GHashTable *allocations;  /* Track allocations for shutdown */
} state = { FALSE };

/* Forward declarations */
static void secure_zero_impl(void *ptr, size_t size);
static gboolean try_mlock_internal(void *ptr, size_t size);
static void try_munlock_internal(void *ptr, size_t size);

/* ============================================================
 * Initialization
 * ============================================================ */

gboolean gnostr_secure_mem_init(void) {
    if (state.initialized) {
        return TRUE;
    }

    g_mutex_init(&state.lock);
    state.allocations = g_hash_table_new(g_direct_hash, g_direct_equal);
    memset(&state.stats, 0, sizeof(state.stats));

    /* Initialize libsodium if available */
#if USE_SODIUM
    if (sodium_init() >= 0) {
        state.stats.sodium_available = TRUE;
        g_debug("gnostr-secure-mem: using libsodium");
    } else {
        g_warning("gnostr-secure-mem: sodium_init failed, using fallback");
    }
#else
    g_debug("gnostr-secure-mem: libsodium not available, using fallback");
#endif

    /* Test mlock capability */
    void *test = malloc(4096);
    if (test) {
        state.stats.mlock_available = try_mlock_internal(test, 4096);
        if (state.stats.mlock_available) {
            try_munlock_internal(test, 4096);
            g_debug("gnostr-secure-mem: mlock available");
        } else {
            g_debug("gnostr-secure-mem: mlock not available (may need elevated privileges)");
        }
        free(test);
    }

    state.initialized = TRUE;
    return TRUE;
}

void gnostr_secure_mem_shutdown(void) {
    if (!state.initialized) {
        return;
    }

    g_mutex_lock(&state.lock);

    /* Zero and free all remaining allocations */
    if (state.allocations) {
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init(&iter, state.allocations);

        while (g_hash_table_iter_next(&iter, &key, &value)) {
            SecureAllocHeader *header = (SecureAllocHeader *)key;

            /* Zero the entire allocation */
            secure_zero_impl(header, header->actual_size);

            if (header->locked) {
                try_munlock_internal(header, header->actual_size);
            }

#if USE_SODIUM
            if (state.stats.sodium_available) {
                sodium_free(header);
            } else
#endif
            {
                free(header);
            }
        }

        g_hash_table_destroy(state.allocations);
        state.allocations = NULL;
    }

    g_mutex_unlock(&state.lock);
    g_mutex_clear(&state.lock);

    memset(&state, 0, sizeof(state));
}

/* ============================================================
 * Core Memory Operations
 * ============================================================ */

void *gnostr_secure_alloc(size_t size) {
    if (size == 0) {
        return NULL;
    }

    /* Auto-initialize if needed */
    if (!state.initialized) {
        if (!gnostr_secure_mem_init()) {
            return NULL;
        }
    }

    /* Calculate actual size needed */
    size_t header_size = sizeof(SecureAllocHeader);
    size_t guard_size = 0;

#if USE_CANARIES
    guard_size = CANARY_SIZE * 2;  /* Head and tail */
#endif

    size_t actual_size = header_size + guard_size + size;

    /* Allocate using sodium if available, otherwise malloc */
    void *raw = NULL;

#if USE_SODIUM
    if (state.stats.sodium_available) {
        raw = sodium_malloc(actual_size);
    }
#endif

    if (!raw) {
        raw = malloc(actual_size);
    }

    if (!raw) {
        return NULL;
    }

    /* Zero the entire allocation */
    memset(raw, 0, actual_size);

    /* Set up header */
    SecureAllocHeader *header = (SecureAllocHeader *)raw;
    header->size = size;
    header->actual_size = actual_size;
    header->locked = FALSE;

#if USE_CANARIES
    header->head_canary = CANARY_HEAD_MAGIC;

    /* Set head guard after header */
    guint64 *head_guard = (guint64 *)((char *)raw + header_size);
    head_guard[0] = CANARY_HEAD_MAGIC;
    head_guard[1] = CANARY_HEAD_MAGIC;

    /* Set tail guard after user data */
    guint64 *tail_guard = (guint64 *)((char *)raw + header_size + CANARY_SIZE + size);
    tail_guard[0] = CANARY_TAIL_MAGIC;
    tail_guard[1] = CANARY_TAIL_MAGIC;
#endif

    /* Try to lock memory */
#if USE_SODIUM
    if (state.stats.sodium_available) {
        /* sodium_malloc already mlocks */
        header->locked = TRUE;
    } else
#endif
    {
        header->locked = try_mlock_internal(raw, actual_size);
    }

    /* Calculate user pointer */
    void *user_ptr = (char *)raw + header_size;

#if USE_CANARIES
    user_ptr = (char *)user_ptr + CANARY_SIZE;
#endif

    /* Track allocation */
    g_mutex_lock(&state.lock);
    g_hash_table_insert(state.allocations, header, GINT_TO_POINTER(1));
    state.stats.total_allocated += size;
    state.stats.allocation_count++;
    if (header->locked) {
        state.stats.total_locked += size;
    }
    if (state.stats.total_allocated > state.stats.peak_allocated) {
        state.stats.peak_allocated = state.stats.total_allocated;
    }
    g_mutex_unlock(&state.lock);

    return user_ptr;
}

void gnostr_secure_free(void *ptr, size_t size) {
    if (!ptr) {
        return;
    }

    if (!state.initialized) {
        g_critical("gnostr_secure_free: called before initialization");
        return;
    }

    /* Calculate header location */
    size_t header_size = sizeof(SecureAllocHeader);
    size_t guard_offset = 0;

#if USE_CANARIES
    guard_offset = CANARY_SIZE;
#endif

    SecureAllocHeader *header = (SecureAllocHeader *)((char *)ptr - header_size - guard_offset);

    /* Verify size matches */
    if (header->size != size) {
        g_critical("gnostr_secure_free: size mismatch (expected %zu, got %zu)",
                   header->size, size);
        /* Continue anyway to avoid leaks */
    }

#if USE_CANARIES
    /* Check canaries */
    guint64 *head_guard = (guint64 *)((char *)header + header_size);
    guint64 *tail_guard = (guint64 *)((char *)ptr + size);

    if (head_guard[0] != CANARY_HEAD_MAGIC || head_guard[1] != CANARY_HEAD_MAGIC) {
        g_critical("gnostr_secure_free: HEAD CANARY CORRUPTED - buffer underflow detected!");
    }
    if (tail_guard[0] != CANARY_TAIL_MAGIC || tail_guard[1] != CANARY_TAIL_MAGIC) {
        g_critical("gnostr_secure_free: TAIL CANARY CORRUPTED - buffer overflow detected!");
    }
#endif

    /* Zero the entire allocation before freeing */
    secure_zero_impl(header, header->actual_size);

    /* Unlock memory */
    if (header->locked) {
#if USE_SODIUM
        if (!state.stats.sodium_available)
#endif
        {
            try_munlock_internal(header, header->actual_size);
        }
    }

    /* Update stats and remove from tracking */
    g_mutex_lock(&state.lock);
    g_hash_table_remove(state.allocations, header);
    state.stats.total_allocated -= size;
    state.stats.allocation_count--;
    if (header->locked) {
        state.stats.total_locked -= size;
    }
    g_mutex_unlock(&state.lock);

    /* Free the memory */
#if USE_SODIUM
    if (state.stats.sodium_available) {
        sodium_free(header);
    } else
#endif
    {
        free(header);
    }
}

void gnostr_secure_clear(void *ptr, size_t size) {
    if (!ptr || size == 0) {
        return;
    }
    secure_zero_impl(ptr, size);
}

/* ============================================================
 * Secure String Handling
 * ============================================================ */

gchar *gnostr_secure_strdup(const char *str) {
    if (!str) {
        return NULL;
    }

    size_t len = strlen(str);
    gchar *dup = (gchar *)gnostr_secure_alloc(len + 1);

    if (dup) {
        memcpy(dup, str, len + 1);
    }

    return dup;
}

gchar *gnostr_secure_strndup(const char *str, size_t n) {
    if (!str) {
        return NULL;
    }

    /* Find actual length (up to n) */
    size_t len = 0;
    while (len < n && str[len] != '\0') {
        len++;
    }

    gchar *dup = (gchar *)gnostr_secure_alloc(len + 1);

    if (dup) {
        memcpy(dup, str, len);
        dup[len] = '\0';
    }

    return dup;
}

void gnostr_secure_strfree(gchar *str) {
    if (!str) {
        return;
    }

    size_t len = strlen(str);
    gnostr_secure_free(str, len + 1);
}

/* ============================================================
 * Memory Locking
 * ============================================================ */

gboolean gnostr_secure_mlock(void *ptr, size_t size) {
    if (!ptr || size == 0) {
        return FALSE;
    }
    return try_mlock_internal(ptr, size);
}

void gnostr_secure_munlock(void *ptr, size_t size) {
    if (!ptr || size == 0) {
        return;
    }
    try_munlock_internal(ptr, size);
}

gboolean gnostr_secure_mlock_available(void) {
    if (!state.initialized) {
        gnostr_secure_mem_init();
    }
    return state.stats.mlock_available;
}

/* ============================================================
 * Constant-Time Operations
 * ============================================================ */

int gnostr_secure_memcmp(const void *a, const void *b, size_t size) {
    if (!a || !b) {
        return (a != b) ? 1 : 0;
    }

#if USE_SODIUM
    if (state.initialized && state.stats.sodium_available) {
        return sodium_memcmp(a, b, size);
    }
#endif

    /* Constant-time comparison fallback */
    volatile const unsigned char *va = (volatile const unsigned char *)a;
    volatile const unsigned char *vb = (volatile const unsigned char *)b;
    volatile unsigned char result = 0;

    for (size_t i = 0; i < size; i++) {
        result |= va[i] ^ vb[i];
    }

    return result != 0;
}

gboolean gnostr_secure_streq(const char *a, const char *b) {
    if (a == b) {
        return TRUE;
    }

    if (!a || !b) {
        return FALSE;
    }

    size_t len_a = strlen(a);
    size_t len_b = strlen(b);

    /* Length difference is not secret, can return early */
    if (len_a != len_b) {
        return FALSE;
    }

    return gnostr_secure_memcmp(a, b, len_a) == 0;
}

/* ============================================================
 * Statistics
 * ============================================================ */

GnostrSecureMemStats gnostr_secure_mem_get_stats(void) {
    GnostrSecureMemStats stats;

    if (!state.initialized) {
        memset(&stats, 0, sizeof(stats));
        return stats;
    }

    g_mutex_lock(&state.lock);
    stats = state.stats;
    g_mutex_unlock(&state.lock);

    return stats;
}

/* ============================================================
 * Internal Helper Functions
 * ============================================================ */

static void secure_zero_impl(void *ptr, size_t size) {
    if (!ptr || size == 0) {
        return;
    }

#if USE_SODIUM
    if (state.initialized && state.stats.sodium_available) {
        sodium_memzero(ptr, size);
        return;
    }
#endif

#ifdef HAVE_MEMSET_S
    /* macOS/C11 secure memset */
    memset_s(ptr, size, 0, size);
#elif defined(HAVE_EXPLICIT_BZERO)
    /* BSD/glibc explicit_bzero */
    explicit_bzero(ptr, size);
#else
    /* Volatile pointer technique to prevent optimization */
    volatile unsigned char *vptr = (volatile unsigned char *)ptr;
    while (size--) {
        *vptr++ = 0;
    }

    /* Memory barrier to ensure the write completes */
#ifdef __GNUC__
    __asm__ __volatile__("" ::: "memory");
#endif
#endif
}

static gboolean try_mlock_internal(void *ptr, size_t size) {
    if (!ptr || size == 0) {
        return FALSE;
    }

#ifdef _WIN32
    return VirtualLock(ptr, size) != 0;
#else
    return mlock(ptr, size) == 0;
#endif
}

static void try_munlock_internal(void *ptr, size_t size) {
    if (!ptr || size == 0) {
        return;
    }

#ifdef _WIN32
    VirtualUnlock(ptr, size);
#else
    munlock(ptr, size);
#endif
}
