/* channel_debug.h - Debug infrastructure for GoChannel primitives
 *
 * Enable with: -DGO_CHANNEL_DEBUG=1 or env NOSTR_CHAN_DEBUG=1
 *
 * Features:
 * - Magic number validation (already exists)
 * - State tracking (ALIVE, CLOSING, FREED)
 * - Canary values before/after struct
 * - Allocation ID for tracking
 * - Poison fill on free
 * - Quarantine mode (never-free for diagnostics)
 * - Detailed assertions at every entrypoint
 */

#ifndef GO_CHANNEL_DEBUG_H
#define GO_CHANNEL_DEBUG_H

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Debug Configuration ─────────────────────────────────────────────── */

/* Enable debug mode via compile flag or runtime env */
#ifndef GO_CHANNEL_DEBUG
#define GO_CHANNEL_DEBUG 0
#endif

/* Poison byte for freed memory */
#define GO_CHAN_POISON_BYTE 0xA5

/* Canary values */
#define GO_CHAN_CANARY1 0x1111111111111111ULL
#define GO_CHAN_CANARY2 0x2222222222222222ULL

/* Magic values */
#define GO_CHAN_MAGIC_ALIVE 0xC4A77E10	 /* "CHANNEL0" - existing magic */
#define GO_CHAN_MAGIC_CLOSING 0xC4A77E11 /* Channel is being destroyed */
#define GO_CHAN_MAGIC_FREED 0xDEADBEEF	 /* Channel has been freed */

/* ── State Enum ──────────────────────────────────────────────────────── */

typedef enum {
    GO_CHAN_STATE_ALIVE = 1,
    GO_CHAN_STATE_CLOSING = 2,
    GO_CHAN_STATE_FREED = 3
} GoChanState;

/* ── Debug Header (prepended to channel in debug mode) ───────────────── */

typedef struct GoChanDebugHeader {
    uint64_t canary_pre;    /* Must be GO_CHAN_CANARY1 */
    uint64_t alloc_id;	    /* Monotonic allocation counter */
    _Atomic uint32_t state; /* GoChanState */
    uint32_t owner_tid;	    /* Thread that created this channel */
    uint64_t alloc_time_ns; /* Allocation timestamp */
    uint64_t free_time_ns;  /* Free timestamp (0 if alive) */
} GoChanDebugHeader;

/* ── Debug Footer (appended to channel in debug mode) ────────────────── */

typedef struct GoChanDebugFooter {
    uint64_t canary_post; /* Must be GO_CHAN_CANARY2 */
} GoChanDebugFooter;

/* ── Global Debug State ──────────────────────────────────────────────── */

/* Runtime debug flag (set from env NOSTR_CHAN_DEBUG) */
extern int g_go_chan_debug_enabled;

/* Quarantine mode: never free channels, just poison them */
extern int g_go_chan_quarantine_mode;

/* Never-free mode: channels are NEVER freed, not even poisoned (purest UAF test) */
extern int g_go_chan_never_free_mode;

/* Operation counter for periodic verification */
extern _Atomic uint64_t g_go_chan_op_counter;

/* Allocation counter */
extern _Atomic uint64_t g_go_chan_alloc_counter;

/* Leaked channel counter (for quarantine mode) */
extern _Atomic uint64_t g_go_chan_leaked_count;

/* Quarantine list for verification */
#define GO_CHAN_QUARANTINE_MAX 1024
extern void *g_go_chan_quarantine_list[GO_CHAN_QUARANTINE_MAX];
extern size_t g_go_chan_quarantine_sizes[GO_CHAN_QUARANTINE_MAX];
extern _Atomic size_t g_go_chan_quarantine_count;

/* ── Debug Initialization ────────────────────────────────────────────── */

static inline void go_chan_debug_init(void) {
    static int inited = 0;
    if (inited)
	return;
    inited = 1;

    const char *dbg = getenv("NOSTR_CHAN_DEBUG");
    if (dbg && *dbg && *dbg != '0') {
	g_go_chan_debug_enabled = 1;
	fprintf(stderr, "[GO_CHAN_DEBUG] Debug mode ENABLED\n");
    }

    const char *quar = getenv("NOSTR_CHAN_QUARANTINE");
    if (quar && *quar && *quar != '0') {
	g_go_chan_quarantine_mode = 1;
	fprintf(stderr, "[GO_CHAN_DEBUG] Quarantine mode ENABLED (channels poisoned but not freed)\n");
    }

    const char *nofree = getenv("NOSTR_CHAN_NEVER_FREE");
    if (nofree && *nofree && *nofree != '0') {
	g_go_chan_never_free_mode = 1;
	fprintf(stderr, "[GO_CHAN_DEBUG] Never-free mode ENABLED (channels NEVER freed - purest UAF test)\n");
    }
}

/* ── Forward Declarations ────────────────────────────────────────────── */

/* Forward declare for use in go_chan_debug_check */
static inline void go_chan_quarantine_verify(void);

/* ── Debug Assertions ────────────────────────────────────────────────── */

#include <pthread.h>

/* Check channel validity - call at every public entrypoint */
#define GO_CHAN_CHECK(chan)                                            \
    do {                                                               \
	if (g_go_chan_debug_enabled) {                                 \
	    go_chan_debug_check((chan), __FILE__, __LINE__, __func__); \
	}                                                              \
    } while (0)

/* Detailed check function */

static inline void go_chan_debug_check(void *chan_ptr, const char *file, int line, const char *func) {
    if (!chan_ptr) {
	fprintf(stderr, "[GO_CHAN_DEBUG] FATAL: NULL channel at %s:%d in %s [tid=%lx]\n",
		file, line, func, (unsigned long)pthread_self());
	abort();
    }

    /* Access the channel's magic field (first field in GoChannel struct) */
    uint32_t magic = *(uint32_t *)chan_ptr;

    if (magic == GO_CHAN_MAGIC_FREED) {
	fprintf(stderr, "[GO_CHAN_DEBUG] FATAL: Use-after-free! Channel %p has FREED magic at %s:%d in %s [tid=%lx]\n",
		chan_ptr, file, line, func, (unsigned long)pthread_self());
	abort();
    }

    if (magic == GO_CHAN_MAGIC_CLOSING) {
	fprintf(stderr, "[GO_CHAN_DEBUG] WARNING: Channel %p is CLOSING at %s:%d in %s [tid=%lx]\n",
		chan_ptr, file, line, func, (unsigned long)pthread_self());
	/* Don't abort - closing channels may still be accessed briefly */
    }

    if (magic != GO_CHAN_MAGIC_ALIVE && magic != GO_CHAN_MAGIC_CLOSING) {
	fprintf(stderr, "[GO_CHAN_DEBUG] FATAL: Invalid magic 0x%08X (expected 0x%08X) at %s:%d in %s [tid=%lx]\n",
		magic, GO_CHAN_MAGIC_ALIVE, file, line, func, (unsigned long)pthread_self());
	abort();
    }

    /* Periodic quarantine verification every 256 ops */
    uint64_t ops = atomic_fetch_add_explicit(&g_go_chan_op_counter, 1, memory_order_relaxed);
    if (g_go_chan_quarantine_mode && (ops & 0xFF) == 0) {
	go_chan_quarantine_verify();
    }
}

/* ── Poison Fill ─────────────────────────────────────────────────────── */

static inline void go_chan_poison_fill(void *ptr, size_t size) {
    if (g_go_chan_debug_enabled) {
	memset(ptr, GO_CHAN_POISON_BYTE, size);
    }
}

/* ── Allocation Tracking ─────────────────────────────────────────────── */

static inline uint64_t go_chan_next_alloc_id(void) {
    return atomic_fetch_add_explicit(&g_go_chan_alloc_counter, 1, memory_order_relaxed);
}

/* ── Quarantine Check ────────────────────────────────────────────────── */

/* Returns 1 if channel should NOT be freed (quarantine mode) */
static inline int go_chan_should_quarantine(void) {
    return g_go_chan_quarantine_mode;
}

static inline void go_chan_record_leak(void) {
    if (g_go_chan_quarantine_mode) {
	uint64_t count = atomic_fetch_add_explicit(&g_go_chan_leaked_count, 1, memory_order_relaxed);
	if ((count + 1) % 100 == 0) {
	    fprintf(stderr, "[GO_CHAN_DEBUG] Quarantine: %llu channels leaked (intentionally)\n",
		    (unsigned long long)(count + 1));
	}
    }
}

/* ── Quarantine Verification ─────────────────────────────────────────── */

/* Add a poisoned channel to quarantine list for later verification */
static inline void go_chan_quarantine_add(void *ptr, size_t size) {
    if (!g_go_chan_quarantine_mode)
	return;

    size_t idx = atomic_fetch_add_explicit(&g_go_chan_quarantine_count, 1, memory_order_relaxed);
    if (idx < GO_CHAN_QUARANTINE_MAX) {
	g_go_chan_quarantine_list[idx] = ptr;
	g_go_chan_quarantine_sizes[idx] = size;
    }
}

/* Verify all quarantined memory is still poisoned (detect UAF writes) */
static inline void go_chan_quarantine_verify(void) {
    if (!g_go_chan_quarantine_mode)
	return;

    size_t count = atomic_load_explicit(&g_go_chan_quarantine_count, memory_order_acquire);
    if (count > GO_CHAN_QUARANTINE_MAX)
	count = GO_CHAN_QUARANTINE_MAX;

    for (size_t i = 0; i < count; i++) {
	void *ptr = g_go_chan_quarantine_list[i];
	size_t size = g_go_chan_quarantine_sizes[i];
	if (!ptr || size == 0)
	    continue;

	/* Skip the magic field (first 4 bytes) - it's set to FREED, not poisoned */
	size_t skip = sizeof(uint32_t);
	if (size <= skip)
	    continue;

	/* Check bytes after magic for poison pattern */
	size_t check_size = (size - skip) < 64 ? (size - skip) : 64;
	unsigned char *bytes = (unsigned char *)ptr + skip;

	for (size_t j = 0; j < check_size; j++) {
	    if (bytes[j] != GO_CHAN_POISON_BYTE) {
		fprintf(stderr,
			"[GO_CHAN_DEBUG] FATAL: UAF WRITE DETECTED!\n"
			"  Quarantined channel %p (size=%zu) was modified!\n"
			"  Byte %zu (offset %zu): expected 0x%02X, found 0x%02X\n"
			"  This proves a use-after-free WRITE occurred!\n"
			"  Thread: %lx\n",
			ptr, size, j, j + skip, GO_CHAN_POISON_BYTE, bytes[j],
			(unsigned long)pthread_self());
		fflush(stderr);
		abort();
	    }
	}
    }

    static uint64_t verify_count = 0;
    verify_count++;
    /* Log every 100 verifications to confirm it's running */
    if (verify_count % 100 == 0) {
	fprintf(stderr, "[GO_CHAN_DEBUG] Quarantine verify #%llu: %zu channels OK\n",
		(unsigned long long)verify_count, count);
	fflush(stderr);
    }
}

#endif /* GO_CHANNEL_DEBUG_H */
