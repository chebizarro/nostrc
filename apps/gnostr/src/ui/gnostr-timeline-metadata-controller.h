/* gnostr-timeline-metadata-controller.h — App-owned metadata batching
 * controller for GNostr timeline rows.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * nostrc-hiei: Moves GNostr-specific metadata batching state out of
 * NostrGtkTimelineView's private struct and into an app-owned GObject
 * so the shared nostr-gtk widget does not carry product-specific
 * scratch fields.
 *
 * Responsibilities:
 * - Queue GNostr event items whose reaction / zap / repost / reply
 *   counts need to be loaded from nostrdb.
 * - Debounce queued items via a single idle source so the worker thread
 *   runs one batch per main-loop iteration instead of N individual
 *   queries (avoids an N+1 pattern during scroll-induced row binds).
 * - Dispatch the batch on a worker thread and apply results back to the
 *   source items on the main thread.
 *
 * Ownership model:
 *   The controller is typically attached to a timeline view via qdata
 *   (see gnostr_timeline_metadata_controller_ensure()). When the view
 *   is disposed, the qdata destroy notify calls
 *   gnostr_timeline_metadata_controller_shutdown() and then unrefs the
 *   controller. Shutdown cancels any pending idle source and drops
 *   queued items. Tasks that were already dispatched to the worker
 *   thread are allowed to complete naturally; they hold their own
 *   reference to the items they operate on, and applying results to a
 *   still-live item is idempotent and harmless even if the row is no
 *   longer bound.
 */

#ifndef GNOSTR_TIMELINE_METADATA_CONTROLLER_H
#define GNOSTR_TIMELINE_METADATA_CONTROLLER_H

#include <glib-object.h>

#include "../model/gn-nostr-event-item.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_TIMELINE_METADATA_CONTROLLER \
    (gnostr_timeline_metadata_controller_get_type())
G_DECLARE_FINAL_TYPE(GnostrTimelineMetadataController,
                     gnostr_timeline_metadata_controller,
                     GNOSTR, TIMELINE_METADATA_CONTROLLER,
                     GObject)

/**
 * gnostr_timeline_metadata_controller_new:
 *
 * Create a new controller. Most callers should use
 * gnostr_timeline_metadata_controller_ensure() instead, which ties the
 * controller's lifetime to an owner object via qdata.
 *
 * Returns: (transfer full): a new controller.
 */
GnostrTimelineMetadataController *
gnostr_timeline_metadata_controller_new(void);

/**
 * gnostr_timeline_metadata_controller_ensure:
 * @owner: GObject whose lifetime the controller should follow
 *   (normally the #NostrGtkTimelineView)
 *
 * Return the controller attached to @owner via qdata, creating and
 * attaching one on first access. The controller is automatically shut
 * down and unref'd when @owner is disposed.
 *
 * Returns: (transfer none): borrowed controller owned by @owner.
 */
GnostrTimelineMetadataController *
gnostr_timeline_metadata_controller_ensure(GObject *owner);

/**
 * gnostr_timeline_metadata_controller_schedule:
 * @self: controller
 * @item: (transfer none): a #GnNostrEventItem whose metadata
 *   (reactions, zaps, reposts, replies) should be refreshed from
 *   nostrdb
 *
 * Queue @item for batched metadata refresh. The controller takes a
 * reference on @item while it is pending. A single idle source fires
 * after the current main loop iteration to hand the queue to a worker
 * thread. Calls with an item already pending in the current batch
 * window are coalesced — only one NDB round-trip is issued per
 * distinct item instance per batch.
 */
void gnostr_timeline_metadata_controller_schedule(
    GnostrTimelineMetadataController *self,
    GnNostrEventItem *item);

/**
 * gnostr_timeline_metadata_controller_shutdown:
 * @self: controller
 *
 * Cancel any pending idle source and drop all queued items. In-flight
 * worker-thread tasks are not cancelled — their results simply apply to
 * the source items which outlive this controller.
 *
 * Normally invoked automatically by the qdata destroy notify when the
 * owner widget is disposed. May be called explicitly for tests or
 * when detaching a controller from its owner.
 */
void gnostr_timeline_metadata_controller_shutdown(
    GnostrTimelineMetadataController *self);

G_END_DECLS

#endif /* GNOSTR_TIMELINE_METADATA_CONTROLLER_H */
