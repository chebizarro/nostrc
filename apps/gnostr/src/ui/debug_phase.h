/* debug_phase.h - Phase isolation for crash debugging
 *
 * Environment variables:
 *   GNOSTR_STRESS_SCROLL=1    - Enable stress scroll test
 *   GNOSTR_DISABLE_UI_UPDATES=1 - Suppress list model signals to widgets
 *   GNOSTR_DISABLE_NETWORK=1  - No websocket/relay connections
 *   GNOSTR_SINGLE_RELAY=<url> - Use only this relay (reduces concurrency)
 *   GNOSTR_SERIALIZE_RELAYS=1 - Connect relays one at a time, wait for success
 *
 * Usage:
 *   # Test network-only (no UI updates)
 *   GNOSTR_DISABLE_UI_UPDATES=1 ./gnostr
 *
 *   # Test UI-only (no network)
 *   GNOSTR_DISABLE_NETWORK=1 ./gnostr
 *
 *   # Single relay, serialized
 *   GNOSTR_SINGLE_RELAY=wss://relay.damus.io GNOSTR_SERIALIZE_RELAYS=1 ./gnostr
 */

#ifndef GNOSTR_DEBUG_PHASE_H
#define GNOSTR_DEBUG_PHASE_H

#include <glib.h>
#include <stdbool.h>
#include <stdio.h>

/* ── Phase Isolation Checks ─────────────────────────────────────────── */

/* Check if UI updates should be suppressed */
static inline bool gnostr_debug_ui_updates_disabled(void) {
    static int cached = -1;
    if (cached < 0) {
	const char *env = g_getenv("GNOSTR_DISABLE_UI_UPDATES");
	cached = (env && *env && g_strcmp0(env, "0") != 0) ? 1 : 0;
	if (cached) {
	    fprintf(stderr, "[DEBUG_PHASE] UI updates DISABLED\n");
	    fflush(stderr);
	}
    }
    return cached == 1;
}

/* Check if network should be disabled */
static inline bool gnostr_debug_network_disabled(void) {
    static int cached = -1;
    if (cached < 0) {
	const char *env = g_getenv("GNOSTR_DISABLE_NETWORK");
	cached = (env && *env && g_strcmp0(env, "0") != 0) ? 1 : 0;
	if (cached) {
	    fprintf(stderr, "[DEBUG_PHASE] Network DISABLED\n");
	    fflush(stderr);
	}
    }
    return cached == 1;
}

/* Get single relay URL if set (NULL otherwise) */
static inline const char *gnostr_debug_single_relay(void) {
    static const char *cached = (const char *)-1;
    if (cached == (const char *)-1) {
	cached = g_getenv("GNOSTR_SINGLE_RELAY");
	if (cached && *cached) {
	    fprintf(stderr, "[DEBUG_PHASE] Single relay mode: %s\n", cached);
	    fflush(stderr);
	} else {
	    cached = NULL;
	}
    }
    return cached;
}

/* Check if relay connections should be serialized */
static inline bool gnostr_debug_serialize_relays(void) {
    static int cached = -1;
    if (cached < 0) {
	const char *env = g_getenv("GNOSTR_SERIALIZE_RELAYS");
	cached = (env && *env && g_strcmp0(env, "0") != 0) ? 1 : 0;
	if (cached) {
	    fprintf(stderr, "[DEBUG_PHASE] Relay connections SERIALIZED\n");
	    fflush(stderr);
	}
    }
    return cached == 1;
}

/* ── Breadcrumb Logging (bypasses GLib filtering) ───────────────────── */

#define BREADCRUMB(fmt, ...)                                \
    do {                                                    \
	fprintf(stderr, "[MARK] " fmt "\n", ##__VA_ARGS__); \
	fflush(stderr);                                     \
    } while (0)

/* Thread-safe breadcrumb with thread ID */
#define BREADCRUMB_THREAD(fmt, ...)                            \
    do {                                                       \
	fprintf(stderr, "[MARK][tid=%lx] " fmt "\n",           \
		(unsigned long)pthread_self(), ##__VA_ARGS__); \
	fflush(stderr);                                        \
    } while (0)

/* ── Thread Ownership Check ─────────────────────────────────────────── */

/* Store main thread ID at startup */
extern pthread_t g_main_thread_id;

static inline void gnostr_debug_set_main_thread(void) {
    g_main_thread_id = pthread_self();
    fprintf(stderr, "[DEBUG_PHASE] Main thread ID: %lx\n", (unsigned long)g_main_thread_id);
    fflush(stderr);
}

/* Assert we're on the main thread - use in model/UI mutation paths */
#define ASSERT_MAIN_THREAD()                                                    \
    do {                                                                        \
	if (g_main_thread_id != 0 && pthread_self() != g_main_thread_id) {      \
	    fprintf(stderr, "[DEBUG_PHASE] FATAL: Non-main thread UI access!\n" \
			    "  Current thread: %lx\n"                           \
			    "  Main thread: %lx\n"                              \
			    "  Location: %s:%d in %s\n",                        \
		    (unsigned long)pthread_self(),                              \
		    (unsigned long)g_main_thread_id,                            \
		    __FILE__, __LINE__, __func__);                              \
	    fflush(stderr);                                                     \
	    abort();                                                            \
	}                                                                       \
    } while (0)

/* ── Quarantine Timer (GTK main loop) ───────────────────────────────── */

/* Forward declare from channel_debug.h */
extern void go_chan_quarantine_verify(void);
extern int g_go_chan_quarantine_mode;

static inline gboolean gnostr_quarantine_verify_timer_cb(gpointer user_data) {
    (void)user_data;
    if (g_go_chan_quarantine_mode) {
	go_chan_quarantine_verify();
    }
    return G_SOURCE_CONTINUE; /* Keep timer running */
}

/* Start the quarantine verification timer (call from main window init) */
static inline guint gnostr_debug_start_quarantine_timer(void) {
    if (!g_go_chan_quarantine_mode) {
	return 0;
    }
    fprintf(stderr, "[DEBUG_PHASE] Starting quarantine verification timer (50ms)\n");
    fflush(stderr);
    return g_timeout_add(50, gnostr_quarantine_verify_timer_cb, NULL);
}

#endif /* GNOSTR_DEBUG_PHASE_H */
