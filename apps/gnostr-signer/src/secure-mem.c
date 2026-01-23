/* secure-mem.c - Secure memory utilities implementation
 *
 * This implementation provides secure memory handling with:
 * - libsodium integration when available
 * - Fallback to system mlock/explicit_bzero
 * - Memory guards for debug builds (canary values or guard pages)
 * - Thread-safe allocation tracking
 *
 * Security features:
 * - Memory locked in RAM (mlock) to prevent swapping to disk
 * - Secure zeroing (explicit_bzero/sodium_memzero) that won't be optimized away
 * - Guard pages using mprotect for buffer overflow detection
 * - Constant-time comparison to prevent timing attacks
 */

/* Enable memset_s on macOS/C11 */
#define __STDC_WANT_LIB_EXT1__ 1

#include "secure-mem.h"
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#ifdef __APPLE__
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/vm_statistics.h>
#else
#include <sys/mman.h>
#endif

#ifdef _WIN32
#include <windows.h>
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

/* Get system page size */
static size_t get_page_size(void) {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize;
#else
    return (size_t)sysconf(_SC_PAGESIZE);
#endif
}

/* Round up to page boundary */
static size_t round_up_to_page(size_t size) {
    size_t page_size = get_page_size();
    return (size + page_size - 1) & ~(page_size - 1);
}

/* Module state */
static struct {
    gboolean initialized;
    GMutex lock;
    GnostrSecureMemStats stats;
    GHashTable *allocations;       /* Track allocations for shutdown */
    GHashTable *guarded_allocs;    /* Track guard-page allocations separately */
    GnostrGuardPageMode guard_mode;
} state = { FALSE };

/* Forward declarations */
static void secure_zero_impl(void *ptr, size_t size);
static gboolean try_mlock_internal(void *ptr, size_t size);
static void try_munlock_internal(void *ptr, size_t size);
static void *alloc_with_guard_pages(size_t size);
static void free_with_guard_pages(void *ptr, size_t size);

/* Guarded allocation header (for guard page mode) */
typedef struct {
    size_t user_size;       /* Requested size by user */
    size_t total_size;      /* Total allocation including guards */
    void *base_ptr;         /* Original mmap base pointer */
    gboolean locked;        /* Successfully mlocked */
} GuardedAllocHeader;

/* ============================================================
 * Initialization
 * ============================================================ */

gboolean gnostr_secure_mem_init(void) {
    if (state.initialized) {
        return TRUE;
    }

    g_mutex_init(&state.lock);
    state.allocations = g_hash_table_new(g_direct_hash, g_direct_equal);
    state.guarded_allocs = g_hash_table_new(g_direct_hash, g_direct_equal);
    memset(&state.stats, 0, sizeof(state.stats));

    /* Set default guard mode based on build type */
#ifndef NDEBUG
    state.guard_mode = GNOSTR_GUARD_CANARY;
#else
    state.guard_mode = GNOSTR_GUARD_NONE;
#endif
    state.stats.guard_mode = state.guard_mode;

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
    g_debug("gnostr-secure-mem: initialized (guard_mode=%d, mlock=%s, sodium=%s)",
            state.guard_mode,
            state.stats.mlock_available ? "yes" : "no",
            state.stats.sodium_available ? "yes" : "no");
    return TRUE;
}

void gnostr_secure_mem_shutdown(void) {
    if (!state.initialized) {
        return;
    }

    g_mutex_lock(&state.lock);

    /* Zero and free all remaining guarded allocations */
    if (state.guarded_allocs) {
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init(&iter, state.guarded_allocs);

        while (g_hash_table_iter_next(&iter, &key, &value)) {
            GuardedAllocHeader *header = (GuardedAllocHeader *)key;
            /* User pointer is just after header */
            void *user_ptr = (char *)header + sizeof(GuardedAllocHeader);
            secure_zero_impl(user_ptr, header->user_size);
            free_with_guard_pages(user_ptr, header->user_size);
        }

        g_hash_table_destroy(state.guarded_allocs);
        state.guarded_allocs = NULL;
    }

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

/* ============================================================
 * Guard Page Implementation
 * ============================================================ */

void gnostr_secure_set_guard_mode(GnostrGuardPageMode mode) {
    if (state.initialized && state.stats.allocation_count > 0) {
        g_warning("gnostr-secure-mem: Cannot change guard mode after allocations");
        return;
    }

    if (!state.initialized) {
        gnostr_secure_mem_init();
    }

    g_mutex_lock(&state.lock);
    state.guard_mode = mode;
    state.stats.guard_mode = mode;
    g_mutex_unlock(&state.lock);

    g_debug("gnostr-secure-mem: Guard mode set to %d", mode);
}

GnostrGuardPageMode gnostr_secure_get_guard_mode(void) {
    if (!state.initialized) {
        gnostr_secure_mem_init();
    }
    return state.guard_mode;
}

/**
 * Allocate memory with guard pages.
 *
 * Layout:
 *   [GUARD PAGE (PROT_NONE)] [HEADER + USER DATA] [GUARD PAGE (PROT_NONE)]
 *
 * The guard pages are marked as inaccessible with mprotect(PROT_NONE).
 * Any access to them will cause a SIGSEGV, immediately detecting buffer
 * overflows or underflows.
 */
static void *alloc_with_guard_pages(size_t size) {
    if (size == 0) {
        return NULL;
    }

    size_t page_size = get_page_size();
    size_t header_size = sizeof(GuardedAllocHeader);

    /* Calculate total size: guard + header + user data + guard */
    /* User data starts at header + sizeof(header), aligned to 16 bytes */
    size_t data_size = header_size + size;
    size_t data_pages = round_up_to_page(data_size);
    size_t total_size = page_size + data_pages + page_size;  /* guard + data + guard */

#ifdef _WIN32
    /* Windows: Use VirtualAlloc */
    void *base = VirtualAlloc(NULL, total_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!base) {
        g_warning("gnostr-secure-mem: VirtualAlloc failed for guarded allocation");
        return NULL;
    }

    /* Protect guard pages */
    DWORD old_protect;
    VirtualProtect(base, page_size, PAGE_NOACCESS, &old_protect);
    VirtualProtect((char *)base + page_size + data_pages, page_size, PAGE_NOACCESS, &old_protect);

#else
    /* Unix: Use mmap + mprotect */
    void *base = mmap(NULL, total_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        g_warning("gnostr-secure-mem: mmap failed for guarded allocation: %s", strerror(errno));
        return NULL;
    }

    /* Protect guard pages - make them inaccessible */
    if (mprotect(base, page_size, PROT_NONE) != 0) {
        g_warning("gnostr-secure-mem: mprotect failed for front guard: %s", strerror(errno));
    }
    if (mprotect((char *)base + page_size + data_pages, page_size, PROT_NONE) != 0) {
        g_warning("gnostr-secure-mem: mprotect failed for back guard: %s", strerror(errno));
    }
#endif

    /* Setup header in the data region (after front guard) */
    GuardedAllocHeader *header = (GuardedAllocHeader *)((char *)base + page_size);
    header->user_size = size;
    header->total_size = total_size;
    header->base_ptr = base;
    header->locked = FALSE;

    /* User data starts after header */
    void *user_ptr = (char *)header + header_size;

    /* Zero the user data area */
    memset(user_ptr, 0, size);

    /* Try to lock the data pages in memory */
    void *data_region = (char *)base + page_size;
    header->locked = try_mlock_internal(data_region, data_pages);

    return user_ptr;
}

static void free_with_guard_pages(void *ptr, size_t size) {
    if (!ptr) {
        return;
    }

    size_t header_size = sizeof(GuardedAllocHeader);
    GuardedAllocHeader *header = (GuardedAllocHeader *)((char *)ptr - header_size);

    /* Verify size matches */
    if (header->user_size != size) {
        g_critical("gnostr-secure-mem: guarded free size mismatch (expected %zu, got %zu)",
                   header->user_size, size);
    }

    /* Zero the user data */
    secure_zero_impl(ptr, header->user_size);

    /* Unlock if locked */
    if (header->locked) {
        size_t page_size = get_page_size();
        size_t data_pages = round_up_to_page(header_size + header->user_size);
        try_munlock_internal((char *)header->base_ptr + page_size, data_pages);
    }

    void *base = header->base_ptr;
    size_t total_size = header->total_size;

#ifdef _WIN32
    VirtualFree(base, 0, MEM_RELEASE);
#else
    munmap(base, total_size);
#endif
}

void *gnostr_secure_alloc_guarded(size_t size) {
    if (size == 0) {
        return NULL;
    }

    if (!state.initialized) {
        if (!gnostr_secure_mem_init()) {
            return NULL;
        }
    }

    void *ptr = alloc_with_guard_pages(size);
    if (!ptr) {
        return NULL;
    }

    /* Track the allocation */
    size_t header_size = sizeof(GuardedAllocHeader);
    GuardedAllocHeader *header = (GuardedAllocHeader *)((char *)ptr - header_size);

    g_mutex_lock(&state.lock);
    g_hash_table_insert(state.guarded_allocs, header, GINT_TO_POINTER(1));
    state.stats.total_allocated += size;
    state.stats.allocation_count++;
    if (header->locked) {
        state.stats.total_locked += size;
    }
    if (state.stats.total_allocated > state.stats.peak_allocated) {
        state.stats.peak_allocated = state.stats.total_allocated;
    }
    g_mutex_unlock(&state.lock);

    return ptr;
}

void gnostr_secure_free_guarded(void *ptr, size_t size) {
    if (!ptr) {
        return;
    }

    if (!state.initialized) {
        g_critical("gnostr-secure-mem: free_guarded called before initialization");
        return;
    }

    size_t header_size = sizeof(GuardedAllocHeader);
    GuardedAllocHeader *header = (GuardedAllocHeader *)((char *)ptr - header_size);

    g_mutex_lock(&state.lock);
    g_hash_table_remove(state.guarded_allocs, header);
    state.stats.total_allocated -= size;
    state.stats.allocation_count--;
    if (header->locked) {
        state.stats.total_locked -= size;
    }
    g_mutex_unlock(&state.lock);

    free_with_guard_pages(ptr, size);
}

/* ============================================================
 * Statistics and Debugging
 * ============================================================ */

void gnostr_secure_mem_dump_stats(void) {
    GnostrSecureMemStats stats = gnostr_secure_mem_get_stats();

    g_debug("=== Secure Memory Statistics ===");
    g_debug("  Total allocated: %zu bytes", stats.total_allocated);
    g_debug("  Total locked:    %zu bytes", stats.total_locked);
    g_debug("  Allocations:     %zu", stats.allocation_count);
    g_debug("  Peak allocated:  %zu bytes", stats.peak_allocated);
    g_debug("  Guard mode:      %d", stats.guard_mode);
    g_debug("  Guard violations:%zu", stats.guard_violations);
    g_debug("  mlock available: %s", stats.mlock_available ? "yes" : "no");
    g_debug("  sodium available:%s", stats.sodium_available ? "yes" : "no");
    g_debug("================================");
}

gboolean gnostr_secure_check_guards(void) {
    if (!state.initialized || state.guard_mode != GNOSTR_GUARD_CANARY) {
        return TRUE;
    }

    gboolean all_valid = TRUE;

    g_mutex_lock(&state.lock);

    if (state.allocations) {
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init(&iter, state.allocations);

        while (g_hash_table_iter_next(&iter, &key, &value)) {
            SecureAllocHeader *header = (SecureAllocHeader *)key;

#if USE_CANARIES
            /* Check header canary */
            if (header->head_canary != CANARY_HEAD_MAGIC) {
                g_critical("gnostr-secure-mem: Header canary corrupted at %p", (void *)header);
                all_valid = FALSE;
                state.stats.guard_violations++;
            }

            /* Check guard canaries */
            size_t header_size = sizeof(SecureAllocHeader);
            guint64 *head_guard = (guint64 *)((char *)header + header_size);
            guint64 *tail_guard = (guint64 *)((char *)header + header_size + CANARY_SIZE + header->size);

            if (head_guard[0] != CANARY_HEAD_MAGIC || head_guard[1] != CANARY_HEAD_MAGIC) {
                g_critical("gnostr-secure-mem: Head guard corrupted at %p", (void *)head_guard);
                all_valid = FALSE;
                state.stats.guard_violations++;
            }

            if (tail_guard[0] != CANARY_TAIL_MAGIC || tail_guard[1] != CANARY_TAIL_MAGIC) {
                g_critical("gnostr-secure-mem: Tail guard corrupted at %p", (void *)tail_guard);
                all_valid = FALSE;
                state.stats.guard_violations++;
            }
#endif
        }
    }

    g_mutex_unlock(&state.lock);

    return all_valid;
}

/* ============================================================
 * Secure Buffer Operations
 * ============================================================ */

void gnostr_secure_copy(void *dest, const void *src, size_t size) {
    if (!dest || !src || size == 0) {
        return;
    }

    /* Use memmove for safety (handles overlapping) */
    memmove(dest, src, size);
}

gchar *gnostr_secure_concat(const char *s1, const char *s2) {
    size_t len1 = s1 ? strlen(s1) : 0;
    size_t len2 = s2 ? strlen(s2) : 0;
    size_t total = len1 + len2;

    if (total == 0) {
        return NULL;
    }

    gchar *result = (gchar *)gnostr_secure_alloc(total + 1);
    if (!result) {
        return NULL;
    }

    if (len1 > 0) {
        memcpy(result, s1, len1);
    }
    if (len2 > 0) {
        memcpy(result + len1, s2, len2);
    }
    result[total] = '\0';

    return result;
}

gchar *gnostr_secure_sprintf(const char *format, ...) {
    if (!format) {
        return NULL;
    }

    va_list args;

    /* First pass: determine required size */
    va_start(args, format);
    int len = vsnprintf(NULL, 0, format, args);
    va_end(args);

    if (len < 0) {
        return NULL;
    }

    /* Allocate secure memory */
    gchar *result = (gchar *)gnostr_secure_alloc((size_t)len + 1);
    if (!result) {
        return NULL;
    }

    /* Second pass: format the string */
    va_start(args, format);
    vsnprintf(result, (size_t)len + 1, format, args);
    va_end(args);

    return result;
}
