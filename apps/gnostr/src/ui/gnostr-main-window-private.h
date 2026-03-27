#ifndef GNOSTR_MAIN_WINDOW_PRIVATE_H
#define GNOSTR_MAIN_WINDOW_PRIVATE_H

#include "gnostr-main-window.h"
#include <stddef.h>
#include <stdint.h>
#include <gio/gio.h>
#include <nostr-gtk-1.0/gnostr-composer.h>
#include "nostr-filter.h"

G_BEGIN_DECLS

typedef struct NostrNip46Session NostrNip46Session;
typedef struct _GNostrPool GNostrPool;
typedef struct _GNostrPoolMultiSub GNostrPoolMultiSub;
typedef struct _GnostrDmService GnostrDmService;
typedef struct _GnNostrEventModel GnNostrEventModel;

struct _GnostrMainWindow {
  AdwApplicationWindow parent_instance;

  /* New Fractal-style state stack */
  GtkStack *main_stack;
  GnostrSessionView *session_view;
  GtkWidget *login_view;
  AdwStatusPage *error_page;
  AdwToastOverlay *toast_overlay;

  /* Responsive mode */
  gboolean compact;

  /* Session state */
  GHashTable *seen_texts; /* owned; keys are g_strdup(text), values unused */

  /* GListModel-based timeline (primary data source) */
  GnNostrEventModel *event_model; /* owned; reactive model over nostrdb */
  guint model_refresh_pending;    /* debounced refresh source id, 0 if none */

  /* REMOVED: avatar_tex_cache was dead code - never populated.
   * Avatar textures are now managed by the centralized gnostr-avatar-cache module.
   * Use gnostr_avatar_cache_size() for texture cache stats. */

  /* Profile subscription */
  gulong profile_sub_id;        /* signal handler ID for profile events */
  GCancellable *profile_sub_cancellable; /* cancellable for profile sub */

  /* Background profile prefetch (paginate kind-1 authors) */
  gulong bg_prefetch_handler;   /* signal handler ID */
  GCancellable *bg_prefetch_cancellable; /* cancellable for paginator */
  guint bg_prefetch_interval_ms; /* default 250ms between pages */

  /* Demand-driven profile fetch (debounced batch) */
  GPtrArray   *profile_fetch_queue;   /* owned; char* pubkey hex to fetch */
  guint        profile_fetch_source_id; /* GLib source id for debounce */
  guint        profile_fetch_debounce_ms; /* default 150ms */
  GCancellable *profile_fetch_cancellable; /* async cancellable */
  guint        profile_fetch_active;  /* count of active concurrent fetches */
  guint        profile_fetch_max_concurrent; /* max concurrent fetches (default 3) */

  /* Remote signer (NIP-46) session */
  NostrNip46Session *nip46_session; /* owned */

  /* Tuning knobs (UI-editable) */
  guint batch_max;             /* default 5; max items per UI batch post */
  guint post_interval_ms;      /* default 150; max ms before forcing a batch */
  guint eose_quiet_ms;         /* default 150; quiet ms after EOSE to stop */
  guint per_relay_hard_ms;     /* default 5000; hard cap per relay */
  guint default_limit;         /* default 30; timeline default limit */
  gboolean use_since;          /* default FALSE; use since window */
  guint since_seconds;         /* default 3600; when use_since */

  /* Backfill interval */
  guint backfill_interval_sec; /* default 0; disabled when 0 */
  guint backfill_source_id;    /* GLib source id, 0 if none */

  /* GNostrPool live stream */
  GNostrPool      *pool;       /* owned */
  GNostrPoolMultiSub *live_multi_sub; /* owned; multi-relay live subscription */
  GCancellable    *pool_cancellable; /* owned */
  NostrFilters    *live_filters; /* owned; current live filter set */
  gulong           pool_events_handler; /* signal handler id */
  gboolean         reconnection_in_progress; /* prevent concurrent reconnection attempts */
  guint            health_check_source_id;   /* GLib source id for relay health check */
  const char     **live_urls;          /* owned array pointer + strings */
  size_t           live_url_count;     /* number of current live relays */
  
  /* Backpressure monitoring */
  guint64          ingest_events_received;   /* total events received from relays */
  guint64          ingest_events_dropped;    /* events dropped due to queue full */
  guint64          ingest_events_processed;  /* events successfully ingested to NDB */
  gint64           last_backpressure_warn_us; /* last backpressure warning timestamp */

  /* Sequential profile batch dispatch state */
  GNostrPool      *profile_pool;          /* owned; GObject pool for profile fetching */
  NostrFilters   *profile_batch_filters; /* owned; kept alive during async query */
  GPtrArray      *profile_batches;       /* owned; elements: GPtrArray* of char* authors */
  guint           profile_batch_pos;     /* next batch index */
  const char    **profile_batch_urls;    /* owned array pointer + strings */
  size_t          profile_batch_url_count;

  /* Debounced local NostrDB profile sweep */
  guint           ndb_sweep_source_id;   /* GLib source id, 0 if none */
  guint           ndb_sweep_debounce_ms; /* default ~150ms */

  /* Sliding window pagination */
  gboolean        loading_older;         /* TRUE while loading older events */
  guint           load_older_batch_size; /* default 30 */

  /* Gift wrap (NIP-59) subscription for DMs */
  uint64_t        sub_gift_wrap;         /* nostrdb subscription ID for kind 1059 */
  char           *user_pubkey_hex;       /* current user's pubkey (64-char hex), NULL if not signed in */
  guint           profile_watch_id;     /* profile provider watch for user's pubkey, 0 if none */
  GPtrArray      *gift_wrap_queue;       /* pending gift wrap events to process */

  /* NIP-17 DM Service for decryption and conversation management */
  GnostrDmService *dm_service;           /* owned; handles gift wrap decryption */

  /* Live relay switching (nostrc-36y.4) */
  gulong           relay_change_handler_id; /* relay config change handler */

  /* Liked events cache (NIP-25 reactions) */
  GHashTable      *liked_events;  /* owned; key=event_id_hex (char*), value=unused (GINT_TO_POINTER(1)) */

  /* nostrc-61s.6: Background operation mode */
  gboolean         background_mode_enabled; /* hide on close instead of quit */

  /* nostrc-mzab: Background NDB ingestion to avoid blocking main thread.
   * Events are queued here and consumed by a dedicated ingestion thread,
   * so ndb_process_event_with() never blocks the GTK main loop. */
  GAsyncQueue     *ingest_queue;           /* owned; char* JSON strings */
  GThread         *ingest_thread;          /* owned; background ingestion worker */
  gboolean         ingest_running;         /* atomic; FALSE signals thread to exit */
};


void gnostr_main_window_show_toast_internal(GnostrMainWindow *self, const char *message);
void gnostr_main_window_handle_composer_post_requested(NostrGtkComposer *composer, const char *text, gpointer user_data);
void gnostr_main_window_on_relays_clicked_internal(GtkButton *btn, gpointer user_data);

G_END_DECLS

#endif /* GNOSTR_MAIN_WINDOW_PRIVATE_H */
