/*
 * nsp-engine — search orchestration for org.gnome.Shell.SearchProvider2.
 *
 * Every search completes by its deadline (default 800 ms, so the Shell
 * gets an answer well inside ~1 s) with whatever it has: session-relay
 * queries end on EOSE/CLOSED or at the deadline with partial results, the
 * NIP-05 HTTPS look-up keeps running in the background to fill the cache
 * for the next keystroke, and avatars are never fetched on this path.
 *
 * Degradation, in order:
 *   - no relay socket                 → identifier queries still return an
 *                                        "Open Nostr profile/note" result
 *                                        (bare item); text returns nothing;
 *   - relay without NIP-50 search     → bounded local scan + client-side
 *                                        word match;
 *   - relay that never answers (the   → first search waits the deadline,
 *     cache-less session relay built    then a circuit breaker skips the
 *     with nostrdb OFF sends no EOSE)   relay for NSP_CIRCUIT_OPEN_S so
 *                                        later keystrokes return at once.
 *
 * Subsearch (the Shell refining its terms): free-text queries are first
 * narrowed locally from the previous result ids without any I/O; only if
 * nothing survives is a fresh search run. Identifier queries are exact
 * look-ups and always run fresh.
 */
#ifndef NSP_ENGINE_H
#define NSP_ENGINE_H

#include <gio/gio.h>

#include "nsp-avatar.h"
#include "nsp-nip05.h"
#include "nsp-relay.h"

G_BEGIN_DECLS

#define NSP_DEADLINE_MS_DEFAULT 800
#define NSP_NIP05_DEBOUNCE_MS_DEFAULT 250
#define NSP_CIRCUIT_OPEN_S 30
#define NSP_MAX_RESULTS 20
#define NSP_STORE_MAX 2000

typedef struct {
  const char *socket_path;  /* NULL: $XDG_RUNTIME_DIR/nostr/relay.sock */
  guint deadline_ms;        /* 0: NSP_DEADLINE_MS_DEFAULT */
  gboolean resolve_nip05;   /* network NIP-05 look-ups */
  guint nip05_debounce_ms;  /* 0: default; G_MAXUINT: none */
  NspAvatars *avatars;      /* borrowed, nullable */
} NspEngineOptions;

typedef struct {
  guint requests;           /* REQs sent to the session relay */
  NspRelayStatus primary;   /* status of the first REQ */
  gboolean fallback_scan;   /* NIP-50 unsupported → local scan */
  gboolean narrowed;        /* subsearch answered from previous results */
  gboolean deadline_hit;
  gboolean circuit_open;    /* relay skipped as recently unresponsive */
  gint64 elapsed_us;
} NspSearchStats;

typedef struct _NspEngine NspEngine;

NspEngine *nsp_engine_new(const NspEngineOptions *opts);
void nsp_engine_free(NspEngine *e);

/* @previous NULL: GetInitialResultSet; else GetSubsearchResultSet. */
void nsp_engine_search_async(NspEngine *e, const char *const *terms,
                             const char *const *previous, GAsyncReadyCallback callback,
                             gpointer user_data);
/* Result ids (canonical nostr: URIs); never NULL. */
char **nsp_engine_search_finish(GAsyncResult *res, NspSearchStats *stats_out);

/* aa{sv} for the known ids (unknown ids are skipped). Floating. */
GVariant *nsp_engine_result_metas(NspEngine *e, const char *const *ids);

/* Finish every in-flight search now with partial results (XUbuntuCancel). */
void nsp_engine_cancel_all(NspEngine *e);

NspNip05 *nsp_engine_get_nip05(NspEngine *e);

G_END_DECLS

#endif /* NSP_ENGINE_H */
