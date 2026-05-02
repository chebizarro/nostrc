/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 gnostr contributors
 *
 * gnostr-thread-prefetch.h - Eager thread ancestor prefetching
 *
 * nostrc-4bk: When kind:1 or kind:1111 events arrive from relays, this
 * service parses NIP-10 e-tags to discover root and parent event IDs,
 * checks whether those events already exist in nostrdb, and batches
 * relay queries to fetch any missing ancestors. This warms the local
 * database so that opening a thread panel renders instantly without
 * waiting for on-open relay fetches.
 *
 * Ownership: created by GnostrMainWindow, single-owner, not a GObject.
 * All mutable state lives on the owner's GMainContext.
 */

#ifndef GNOSTR_THREAD_PREFETCH_H
#define GNOSTR_THREAD_PREFETCH_H

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _GnostrThreadPrefetch GnostrThreadPrefetch;

/**
 * GnostrThreadPrefetchIngestFunc:
 * @user_data: user data from construction
 * @event_json_owned: (transfer full): JSON string; callee takes ownership on TRUE return
 *
 * Callback to push a fetched event into the normal ingest pipeline.
 * Must return TRUE if the JSON was accepted (ownership transferred),
 * FALSE if rejected (caller frees).
 */
typedef gboolean (*GnostrThreadPrefetchIngestFunc)(gpointer user_data,
                                                   gchar   *event_json_owned);

/**
 * gnostr_thread_prefetch_new:
 * @ingest_func: callback to push fetched ancestor JSON into the ingest queue
 * @ingest_user_data: user data for @ingest_func
 *
 * Creates a new thread prefetch service. All state mutations and timer
 * callbacks run on the thread-default GMainContext at construction time
 * (typically the GTK main thread).
 *
 * Returns: (transfer full): a new prefetch service. Free with
 *          gnostr_thread_prefetch_free().
 */
GnostrThreadPrefetch *gnostr_thread_prefetch_new(
    GnostrThreadPrefetchIngestFunc  ingest_func,
    gpointer                        ingest_user_data);

/**
 * gnostr_thread_prefetch_free:
 * @self: (nullable): prefetch service to free
 *
 * Cancels any pending relay queries, removes timers, and frees all state.
 * Safe to call with NULL.
 */
void gnostr_thread_prefetch_free(GnostrThreadPrefetch *self);

/**
 * gnostr_thread_prefetch_observe_event:
 * @self: a #GnostrThreadPrefetch
 * @event_json: raw JSON of the incoming event (not consumed)
 * @source_relay_url: (nullable): relay URL the event came from (used as hint)
 *
 * Inspects a live kind:1 or kind:1111 event for NIP-10 thread references.
 * If root_id or reply_id are found, schedules prefetch of those ancestors.
 *
 * This function is safe to call from any thread; it marshals work onto
 * the owner context internally.
 */
void gnostr_thread_prefetch_observe_event(GnostrThreadPrefetch *self,
                                          const char           *event_json,
                                          const char           *source_relay_url);

G_END_DECLS

#endif /* GNOSTR_THREAD_PREFETCH_H */
