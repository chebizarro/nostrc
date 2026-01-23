/* secure-memory.c - Secure memory management implementation
 *
 * This implementation provides secure memory handling with:
 * - libsodium integration when available
 * - Fallback to system mlock/explicit_bzero
 * - Memory guards for debug builds
 * - Reference-counted GnSecureString wrapper
 */

#include "secure-memory.h"
#include <string.h>
#include <stdlib.h>

#ifdef __APPLE__
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/vm_statistics.h>
#else
#include <sys/mman.h>
#endif

/* Check for libsodium */
#ifdef GNOSTR_HAVE_SODIUM
#include <sodium.h>
#define HAVE_SODIUM 1
#else
#define HAVE_SODIUM 0
#endif

/* Check for explicit_bzero */
#ifdef __APPLE__
#include <string.h>
/* macOS has memset_s in string.h */
#define HAVE_MEMSET_S 1
#elif defined(__linux__)
/* Linux glibc 2.25+ has explicit_bzero */
#include <features.h>
#if __GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 25)
#define HAVE_EXPLICIT_BZERO 1
#endif
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#define HAVE_EXPLICIT_BZERO 1
#endif

/* Guard canary magic values */
#define CANARY_HEAD_MAGIC 0xDEADBEEFCAFEBABEULL
#define CANARY_TAIL_MAGIC 0xFEEDFACE12345678ULL
#define CANARY_SIZE 16

/* Allocation header for tracking */
typedef struct {
  size_t size;            /* Requested size */
  size_t actual_size;     /* Actual allocation (with guards) */
  bool locked;            /* Successfully mlocked */
#ifndef NDEBUG
  uint64_t head_canary;   /* Overflow detection */
#endif
} SecureAllocHeader;

/* Module state */
static struct {
  bool initialized;
  GnGuardMode guard_mode;
  GMutex lock;
  GnSecureStats stats;
  GHashTable *allocations;  /* Track allocations for shutdown */
} secure_state = {0};

/* Forward declarations */
static void secure_zero_fallback(void *ptr, size_t size);
static bool try_mlock(void *ptr, size_t size);
static void try_munlock(void *ptr, size_t size);

/* ============================================================
 * Core Implementation
 * ============================================================ */

GnSecureResult gn_secure_init(GnGuardMode guard_mode) {
  if (secure_state.initialized) {
    return GN_SECURE_OK;
  }

  g_mutex_init(&secure_state.lock);
  secure_state.guard_mode = guard_mode;
  secure_state.allocations = g_hash_table_new(g_direct_hash, g_direct_equal);

  /* Check libsodium availability */
#if HAVE_SODIUM
  if (sodium_init() >= 0) {
    secure_state.stats.sodium_available = true;
    g_debug("secure-memory: using libsodium");
  } else {
    g_warning("secure-memory: sodium_init failed, using fallback");
  }
#else
  g_debug("secure-memory: libsodium not available, using fallback");
#endif

  /* Test mlock capability */
  void *test = malloc(4096);
  if (test) {
    secure_state.stats.mlock_available = try_mlock(test, 4096);
    if (secure_state.stats.mlock_available) {
      try_munlock(test, 4096);
      g_debug("secure-memory: mlock available");
    } else {
      g_debug("secure-memory: mlock not available (may need elevated privileges)");
    }
    free(test);
  }

  secure_state.initialized = true;
  return GN_SECURE_OK;
}

void gn_secure_shutdown(void) {
  if (!secure_state.initialized) {
    return;
  }

  g_mutex_lock(&secure_state.lock);

  /* Zero and free all remaining allocations */
  if (secure_state.allocations) {
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, secure_state.allocations);

    while (g_hash_table_iter_next(&iter, &key, &value)) {
      SecureAllocHeader *header = (SecureAllocHeader*)key;
      void *user_ptr = (char*)header + sizeof(SecureAllocHeader);

#ifndef NDEBUG
      if (secure_state.guard_mode == GN_GUARD_CANARY) {
        user_ptr = (char*)user_ptr + CANARY_SIZE;
      }
#endif

      /* Zero the entire allocation */
      gn_secure_zero(header, header->actual_size);

      if (header->locked) {
        try_munlock(header, header->actual_size);
      }
      free(header);
    }

    g_hash_table_destroy(secure_state.allocations);
    secure_state.allocations = NULL;
  }

  g_mutex_unlock(&secure_state.lock);
  g_mutex_clear(&secure_state.lock);

  memset(&secure_state, 0, sizeof(secure_state));
}

GnSecureStats gn_secure_get_stats(void) {
  GnSecureStats stats;
  g_mutex_lock(&secure_state.lock);
  stats = secure_state.stats;
  g_mutex_unlock(&secure_state.lock);
  return stats;
}

void *gn_secure_alloc(size_t size) {
  if (size == 0) {
    return NULL;
  }

  /* Auto-initialize if needed */
  if (!secure_state.initialized) {
    gn_secure_init(GN_GUARD_NONE);
  }

  /* Calculate actual size needed */
  size_t header_size = sizeof(SecureAllocHeader);
  size_t guard_size = 0;

#ifndef NDEBUG
  if (secure_state.guard_mode == GN_GUARD_CANARY) {
    guard_size = CANARY_SIZE * 2;  /* Head and tail */
  }
#endif

  size_t actual_size = header_size + guard_size + size;

  /* Allocate using sodium if available, otherwise malloc */
  void *raw = NULL;

#if HAVE_SODIUM
  if (secure_state.stats.sodium_available) {
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
  SecureAllocHeader *header = (SecureAllocHeader*)raw;
  header->size = size;
  header->actual_size = actual_size;
  header->locked = false;

#ifndef NDEBUG
  if (secure_state.guard_mode == GN_GUARD_CANARY) {
    header->head_canary = CANARY_HEAD_MAGIC;

    /* Set head guard after header */
    uint64_t *head_guard = (uint64_t*)((char*)raw + header_size);
    head_guard[0] = CANARY_HEAD_MAGIC;
    head_guard[1] = CANARY_HEAD_MAGIC;

    /* Set tail guard after user data */
    uint64_t *tail_guard = (uint64_t*)((char*)raw + header_size + CANARY_SIZE + size);
    tail_guard[0] = CANARY_TAIL_MAGIC;
    tail_guard[1] = CANARY_TAIL_MAGIC;
  }
#endif

  /* Try to lock memory */
#if HAVE_SODIUM
  if (secure_state.stats.sodium_available) {
    /* sodium_malloc already mlocks */
    header->locked = true;
  } else
#endif
  {
    header->locked = try_mlock(raw, actual_size);
  }

  /* Calculate user pointer */
  void *user_ptr = (char*)raw + header_size;

#ifndef NDEBUG
  if (secure_state.guard_mode == GN_GUARD_CANARY) {
    user_ptr = (char*)user_ptr + CANARY_SIZE;
  }
#endif

  /* Track allocation */
  g_mutex_lock(&secure_state.lock);
  g_hash_table_insert(secure_state.allocations, header, GINT_TO_POINTER(1));
  secure_state.stats.total_allocated += size;
  secure_state.stats.allocation_count++;
  if (header->locked) {
    secure_state.stats.total_locked += size;
  }
  if (secure_state.stats.total_allocated > secure_state.stats.peak_allocated) {
    secure_state.stats.peak_allocated = secure_state.stats.total_allocated;
  }
  g_mutex_unlock(&secure_state.lock);

  return user_ptr;
}

void *gn_secure_realloc(void *ptr, size_t old_size, size_t new_size) {
  if (!ptr) {
    return gn_secure_alloc(new_size);
  }

  if (new_size == 0) {
    gn_secure_free(ptr, old_size);
    return NULL;
  }

  /* Allocate new buffer */
  void *new_ptr = gn_secure_alloc(new_size);
  if (!new_ptr) {
    return NULL;
  }

  /* Copy data */
  size_t copy_size = (old_size < new_size) ? old_size : new_size;
  memcpy(new_ptr, ptr, copy_size);

  /* Free old buffer */
  gn_secure_free(ptr, old_size);

  return new_ptr;
}

void gn_secure_free(void *ptr, size_t size) {
  if (!ptr) {
    return;
  }

  /* Calculate header location */
  size_t header_size = sizeof(SecureAllocHeader);
  size_t guard_offset = 0;

#ifndef NDEBUG
  if (secure_state.guard_mode == GN_GUARD_CANARY) {
    guard_offset = CANARY_SIZE;
  }
#endif

  SecureAllocHeader *header = (SecureAllocHeader*)((char*)ptr - header_size - guard_offset);

  /* Verify size matches */
  if (header->size != size) {
    g_critical("secure-memory: size mismatch in gn_secure_free (expected %zu, got %zu)",
               header->size, size);
    /* Continue anyway to avoid leaks */
  }

#ifndef NDEBUG
  /* Check canaries */
  if (secure_state.guard_mode == GN_GUARD_CANARY) {
    uint64_t *head_guard = (uint64_t*)((char*)header + header_size);
    uint64_t *tail_guard = (uint64_t*)((char*)ptr + size);

    if (head_guard[0] != CANARY_HEAD_MAGIC || head_guard[1] != CANARY_HEAD_MAGIC) {
      g_critical("secure-memory: HEAD CANARY CORRUPTED - buffer underflow detected!");
    }
    if (tail_guard[0] != CANARY_TAIL_MAGIC || tail_guard[1] != CANARY_TAIL_MAGIC) {
      g_critical("secure-memory: TAIL CANARY CORRUPTED - buffer overflow detected!");
    }
  }
#endif

  /* Zero the entire allocation before freeing */
  gn_secure_zero(header, header->actual_size);

  /* Unlock memory */
  if (header->locked) {
#if HAVE_SODIUM
    if (!secure_state.stats.sodium_available)
#endif
    {
      try_munlock(header, header->actual_size);
    }
  }

  /* Update stats and remove from tracking */
  g_mutex_lock(&secure_state.lock);
  g_hash_table_remove(secure_state.allocations, header);
  secure_state.stats.total_allocated -= size;
  secure_state.stats.allocation_count--;
  if (header->locked) {
    secure_state.stats.total_locked -= size;
  }
  g_mutex_unlock(&secure_state.lock);

  /* Free the memory */
#if HAVE_SODIUM
  if (secure_state.stats.sodium_available) {
    sodium_free(header);
  } else
#endif
  {
    free(header);
  }
}

void gn_secure_zero(void *ptr, size_t size) {
  if (!ptr || size == 0) {
    return;
  }

#if HAVE_SODIUM
  if (secure_state.stats.sodium_available) {
    sodium_memzero(ptr, size);
    return;
  }
#endif

  secure_zero_fallback(ptr, size);
}

int gn_secure_memcmp(const void *a, const void *b, size_t size) {
  if (!a || !b) {
    return (a != b) ? 1 : 0;
  }

#if HAVE_SODIUM
  if (secure_state.stats.sodium_available) {
    return sodium_memcmp(a, b, size);
  }
#endif

  /* Constant-time comparison fallback */
  volatile const unsigned char *va = (volatile const unsigned char*)a;
  volatile const unsigned char *vb = (volatile const unsigned char*)b;
  volatile unsigned char result = 0;

  for (size_t i = 0; i < size; i++) {
    result |= va[i] ^ vb[i];
  }

  return result != 0;
}

char *gn_secure_strdup(const char *str) {
  if (!str) {
    return NULL;
  }

  size_t len = strlen(str);
  char *dup = (char*)gn_secure_alloc(len + 1);

  if (dup) {
    memcpy(dup, str, len + 1);
  }

  return dup;
}

void gn_secure_strfree(char *str) {
  if (!str) {
    return;
  }

  size_t len = strlen(str);
  gn_secure_free(str, len + 1);
}

size_t gn_secure_strlen(const char *str) {
  return str ? strlen(str) : 0;
}

/* ============================================================
 * GnSecureString Implementation
 * ============================================================ */

struct _GnSecureString {
  volatile int ref_count;
  char *data;
  size_t len;
  size_t capacity;
};

GnSecureString *gn_secure_string_new(const char *str) {
  if (!str) {
    return gn_secure_string_new_empty(0);
  }

  return gn_secure_string_new_len(str, strlen(str));
}

GnSecureString *gn_secure_string_new_len(const char *data, size_t len) {
  GnSecureString *ss = g_new0(GnSecureString, 1);
  if (!ss) {
    return NULL;
  }

  ss->ref_count = 1;
  ss->len = len;
  ss->capacity = len + 1;
  ss->data = (char*)gn_secure_alloc(ss->capacity);

  if (!ss->data) {
    g_free(ss);
    return NULL;
  }

  if (data && len > 0) {
    memcpy(ss->data, data, len);
  }
  ss->data[len] = '\0';

  return ss;
}

GnSecureString *gn_secure_string_new_empty(size_t capacity) {
  GnSecureString *ss = g_new0(GnSecureString, 1);
  if (!ss) {
    return NULL;
  }

  ss->ref_count = 1;
  ss->len = 0;
  ss->capacity = capacity > 0 ? capacity + 1 : 16;
  ss->data = (char*)gn_secure_alloc(ss->capacity);

  if (!ss->data) {
    g_free(ss);
    return NULL;
  }

  ss->data[0] = '\0';
  return ss;
}

const char *gn_secure_string_get(const GnSecureString *ss) {
  return ss ? ss->data : NULL;
}

size_t gn_secure_string_len(const GnSecureString *ss) {
  return ss ? ss->len : 0;
}

bool gn_secure_string_is_empty(const GnSecureString *ss) {
  return !ss || ss->len == 0;
}

GnSecureResult gn_secure_string_append(GnSecureString *ss, const char *str) {
  if (!ss || !str) {
    return GN_SECURE_ERR_INVALID;
  }

  size_t add_len = strlen(str);
  if (add_len == 0) {
    return GN_SECURE_OK;
  }

  size_t new_len = ss->len + add_len;

  /* Check if we need to grow */
  if (new_len + 1 > ss->capacity) {
    size_t new_capacity = (new_len + 1) * 2;
    char *new_data = (char*)gn_secure_alloc(new_capacity);
    if (!new_data) {
      return GN_SECURE_ERR_ALLOC;
    }

    memcpy(new_data, ss->data, ss->len);
    gn_secure_free(ss->data, ss->capacity);
    ss->data = new_data;
    ss->capacity = new_capacity;
  }

  memcpy(ss->data + ss->len, str, add_len);
  ss->len = new_len;
  ss->data[ss->len] = '\0';

  return GN_SECURE_OK;
}

GnSecureResult gn_secure_string_append_c(GnSecureString *ss, char c) {
  char buf[2] = {c, '\0'};
  return gn_secure_string_append(ss, buf);
}

void gn_secure_string_clear(GnSecureString *ss) {
  if (!ss || !ss->data) {
    return;
  }

  gn_secure_zero(ss->data, ss->len);
  ss->len = 0;
  ss->data[0] = '\0';
}

GnSecureString *gn_secure_string_ref(GnSecureString *ss) {
  if (ss) {
    g_atomic_int_inc(&ss->ref_count);
  }
  return ss;
}

void gn_secure_string_unref(GnSecureString *ss) {
  if (!ss) {
    return;
  }

  if (g_atomic_int_dec_and_test(&ss->ref_count)) {
    if (ss->data) {
      gn_secure_free(ss->data, ss->capacity);
    }
    g_free(ss);
  }
}

bool gn_secure_string_equal(const GnSecureString *a, const GnSecureString *b) {
  if (a == b) {
    return true;
  }

  if (!a || !b) {
    return false;
  }

  if (a->len != b->len) {
    return false;
  }

  return gn_secure_memcmp(a->data, b->data, a->len) == 0;
}

char *gn_secure_string_steal(GnSecureString *ss, size_t *out_len) {
  if (!ss) {
    if (out_len) *out_len = 0;
    return NULL;
  }

  char *data = ss->data;
  size_t len = ss->len;

  ss->data = NULL;
  ss->len = 0;
  ss->capacity = 0;

  if (out_len) {
    *out_len = len;
  }

  return data;
}

/* ============================================================
 * GLib Boxed Type Registration
 * ============================================================ */

static GnSecureString *gn_secure_string_copy(GnSecureString *ss) {
  return gn_secure_string_ref(ss);
}

G_DEFINE_BOXED_TYPE(GnSecureString, gn_secure_string,
                    gn_secure_string_copy, gn_secure_string_unref)

/* ============================================================
 * Internal Helper Functions
 * ============================================================ */

static void secure_zero_fallback(void *ptr, size_t size) {
  if (!ptr || size == 0) {
    return;
  }

#ifdef HAVE_MEMSET_S
  /* macOS/C11 secure memset */
  memset_s(ptr, size, 0, size);
#elif defined(HAVE_EXPLICIT_BZERO)
  /* BSD/glibc explicit_bzero */
  explicit_bzero(ptr, size);
#else
  /* Volatile pointer technique to prevent optimization */
  volatile unsigned char *vptr = (volatile unsigned char*)ptr;
  while (size--) {
    *vptr++ = 0;
  }

  /* Memory barrier to ensure the write completes */
  __asm__ __volatile__("" ::: "memory");
#endif
}

static bool try_mlock(void *ptr, size_t size) {
  if (!ptr || size == 0) {
    return false;
  }

#ifdef _WIN32
  return VirtualLock(ptr, size) != 0;
#else
  return mlock(ptr, size) == 0;
#endif
}

static void try_munlock(void *ptr, size_t size) {
  if (!ptr || size == 0) {
    return;
  }

#ifdef _WIN32
  VirtualUnlock(ptr, size);
#else
  munlock(ptr, size);
#endif
}
