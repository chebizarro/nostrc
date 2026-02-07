/**
 * @file gnostr-sync-service.h
 * @brief Background sync service with adaptive scheduling
 *
 * Manages periodic negentropy sync for contacts (kind:3) and mute lists
 * (kind:10000). Features:
 * - Periodic sync with configurable interval
 * - Incremental sync on relay reconnection
 * - Adaptive scheduling: backs off when in sync, speeds up on changes
 * - EventBus integration for sync status notifications
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GNOSTR_SYNC_SERVICE_H
#define GNOSTR_SYNC_SERVICE_H

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_SYNC_SERVICE (gnostr_sync_service_get_type())

G_DECLARE_FINAL_TYPE(GnostrSyncService, gnostr_sync_service,
                     GNOSTR, SYNC_SERVICE, GObject)

/**
 * GnostrSyncState:
 * @GNOSTR_SYNC_IDLE: No sync in progress
 * @GNOSTR_SYNC_RUNNING: Sync operation active
 * @GNOSTR_SYNC_ERROR: Last sync failed
 *
 * Current sync service state.
 */
typedef enum {
  GNOSTR_SYNC_IDLE,
  GNOSTR_SYNC_RUNNING,
  GNOSTR_SYNC_ERROR
} GnostrSyncState;

/* --- Singleton Access --- */

/**
 * gnostr_sync_service_get_default:
 *
 * Gets the singleton sync service instance. Thread-safe.
 *
 * Returns: (transfer none): The default sync service. Do not unref.
 */
GnostrSyncService *gnostr_sync_service_get_default(void);

/**
 * gnostr_sync_service_shutdown:
 *
 * Shuts down the sync service, stopping timers and cancelling
 * pending operations. Call at application shutdown.
 */
void gnostr_sync_service_shutdown(void);

/* --- Control --- */

/**
 * gnostr_sync_service_start:
 * @self: The sync service
 *
 * Starts the periodic sync timer. Triggers an immediate first sync,
 * then schedules periodic syncs at the adaptive interval.
 * No-op if already started.
 */
void gnostr_sync_service_start(GnostrSyncService *self);

/**
 * gnostr_sync_service_stop:
 * @self: The sync service
 *
 * Stops the periodic sync timer and cancels any pending sync.
 */
void gnostr_sync_service_stop(GnostrSyncService *self);

/**
 * gnostr_sync_service_sync_now:
 * @self: The sync service
 *
 * Triggers an immediate sync, resetting the periodic timer.
 * Use for reconnection events or user-requested sync.
 */
void gnostr_sync_service_sync_now(GnostrSyncService *self);

/* --- Status --- */

/**
 * gnostr_sync_service_get_state:
 * @self: The sync service
 *
 * Returns: The current sync state.
 */
GnostrSyncState gnostr_sync_service_get_state(GnostrSyncService *self);

/**
 * gnostr_sync_service_get_last_sync_time:
 * @self: The sync service
 *
 * Returns: Monotonic time (microseconds) of last completed sync, or 0.
 */
gint64 gnostr_sync_service_get_last_sync_time(GnostrSyncService *self);

/**
 * gnostr_sync_service_get_consecutive_in_sync:
 * @self: The sync service
 *
 * Returns: Number of consecutive syncs that found no changes.
 */
guint gnostr_sync_service_get_consecutive_in_sync(GnostrSyncService *self);

/**
 * gnostr_sync_service_is_running:
 * @self: The sync service
 *
 * Returns: %TRUE if the periodic timer is active.
 */
gboolean gnostr_sync_service_is_running(GnostrSyncService *self);

/* --- EventBus Topics --- */

/**
 * EventBus topics emitted by the sync service:
 *
 * Generic sync lifecycle:
 *   "sync::started"                 - Sync began (json = relay URL)
 *   "sync::completed"               - Sync succeeded (json = stats)
 *   "sync::error"                   - Sync failed (json = error message)
 *   "sync::schedule"                - Interval changed (json = interval info)
 *
 * Negentropy-specific (emitted on completion):
 *   "negentropy::sync-complete"     - Full sync result with kind details
 *   "negentropy::kind::3"           - Contact list may have changed
 *   "negentropy::kind::10000"       - Mute list may have changed
 *
 * UI components should subscribe to kind-specific topics to trigger refresh.
 */
#define GNOSTR_SYNC_TOPIC_STARTED    "sync::started"
#define GNOSTR_SYNC_TOPIC_COMPLETED  "sync::completed"
#define GNOSTR_SYNC_TOPIC_ERROR      "sync::error"
#define GNOSTR_SYNC_TOPIC_SCHEDULE   "sync::schedule"

#define GNOSTR_NEG_TOPIC_SYNC_COMPLETE  "negentropy::sync-complete"
#define GNOSTR_NEG_TOPIC_KIND_PREFIX    "negentropy::kind::"

G_END_DECLS

#endif /* GNOSTR_SYNC_SERVICE_H */
