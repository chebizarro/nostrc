/* gnostr-timeline-feed-controller.h — Timeline compositor/controller core
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The feed controller is the model-time compositor described in
 * docs/designs/gnostr-timeline-compositor.md.  It owns the stable reading
 * surface policy: source batches are merged into a hidden working set, live
 * head updates can be held pending while the reader is scrolled down, and
 * immutable snapshots are published intentionally through
 * GnostrTimelineSnapshotModel.
 *
 * This object deliberately does not bind rows or switch the main timeline page.
 * Future view/factory code should consume gnostr_timeline_feed_controller_get_model(),
 * set row reservations from GnostrTimelineSnapshotRow, use
 * gnostr_timeline_feed_controller_dup_geometry_token_for_row() as the row
 * measured-geometry token, and feed measurements back with
 * gnostr_timeline_feed_controller_record_geometry().
 */

#ifndef GNOSTR_TIMELINE_FEED_CONTROLLER_H
#define GNOSTR_TIMELINE_FEED_CONTROLLER_H

#include <gio/gio.h>
#include <glib-object.h>

#include "../model/gnostr-timeline-batch.h"
#include "../model/gnostr-timeline-snapshot-model.h"
#include "../model/gnostr-timeline-source.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_TIMELINE_FEED_CONTROLLER \
  (gnostr_timeline_feed_controller_get_type())
G_DECLARE_FINAL_TYPE(GnostrTimelineFeedController,
                     gnostr_timeline_feed_controller,
                     GNOSTR, TIMELINE_FEED_CONTROLLER,
                     GObject)

typedef struct {
  char   *event_id;
  guint   index_hint;
  double  offset_px_in_row;
  guint64 snapshot_generation;
} GnostrTimelineAnchor;

GnostrTimelineFeedController *
gnostr_timeline_feed_controller_new(GnostrTimelineSource *source);

GnostrTimelineSnapshotModel *
gnostr_timeline_feed_controller_get_model(GnostrTimelineFeedController *self);

GnostrTimelineSource *
gnostr_timeline_feed_controller_get_source(GnostrTimelineFeedController *self);

guint64 gnostr_timeline_feed_controller_get_query_generation(GnostrTimelineFeedController *self);

void gnostr_timeline_feed_controller_set_source(GnostrTimelineFeedController *self,
                                                GnostrTimelineSource *source);

void gnostr_timeline_feed_controller_set_query(GnostrTimelineFeedController *self,
                                               GNostrTimelineQuery *query);

void gnostr_timeline_feed_controller_refresh(GnostrTimelineFeedController *self);
void gnostr_timeline_feed_controller_load_older(GnostrTimelineFeedController *self,
                                                guint count);
void gnostr_timeline_feed_controller_load_newer(GnostrTimelineFeedController *self,
                                                guint count);

void gnostr_timeline_feed_controller_ingest_batch(GnostrTimelineFeedController *self,
                                                  GnostrTimelineBatch *batch);

void gnostr_timeline_feed_controller_set_viewport(GnostrTimelineFeedController *self,
                                                  double scroll_y,
                                                  double viewport_height,
                                                  guint width_px);
void gnostr_timeline_feed_controller_set_user_at_top(GnostrTimelineFeedController *self,
                                                     gboolean at_top);
gboolean gnostr_timeline_feed_controller_get_user_at_top(GnostrTimelineFeedController *self);

guint gnostr_timeline_feed_controller_get_pending_count(GnostrTimelineFeedController *self);
void  gnostr_timeline_feed_controller_admit_pending_head(GnostrTimelineFeedController *self,
                                                         gboolean scroll_to_top);

void gnostr_timeline_feed_controller_compose_now(GnostrTimelineFeedController *self);

char *gnostr_timeline_feed_controller_dup_geometry_token_for_row(GnostrTimelineSnapshotRow *row);
void  gnostr_timeline_feed_controller_record_geometry(GnostrTimelineFeedController *self,
                                                      const char *geometry_token,
                                                      guint64 snapshot_generation,
                                                      gint width_px,
                                                      gint height_px);

void gnostr_timeline_anchor_clear(GnostrTimelineAnchor *anchor);

G_END_DECLS

#endif /* GNOSTR_TIMELINE_FEED_CONTROLLER_H */
