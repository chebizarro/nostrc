#ifndef GNOSTR_TIMELINE_SNAPSHOT_H
#define GNOSTR_TIMELINE_SNAPSHOT_H

#include <gio/gio.h>

#include "gnostr-timeline-item-view-model.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_TIMELINE_SNAPSHOT_ROW (gnostr_timeline_snapshot_row_get_type())
G_DECLARE_FINAL_TYPE(GnostrTimelineSnapshotRow, gnostr_timeline_snapshot_row, GNOSTR, TIMELINE_SNAPSHOT_ROW, GObject)

#define GNOSTR_TYPE_TIMELINE_SNAPSHOT (gnostr_timeline_snapshot_get_type())
G_DECLARE_FINAL_TYPE(GnostrTimelineSnapshot, gnostr_timeline_snapshot, GNOSTR, TIMELINE_SNAPSHOT, GObject)

GnostrTimelineSnapshotRow *gnostr_timeline_snapshot_row_new(const char *event_id,
                                                            const char *note_key,
                                                            const char *pubkey,
                                                            gint64 created_at,
                                                            const char *tie_breaker,
                                                            const char *content,
                                                            double estimated_height,
                                                            double measured_height,
                                                            double effective_height,
                                                            guint width_bucket,
                                                            const char *layout_signature,
                                                            gboolean geometry_measured);

GnostrTimelineSnapshotRow *gnostr_timeline_snapshot_row_new_full(const char *event_id,
                                                                 const char *note_key,
                                                                 const char *pubkey,
                                                                 gint64 created_at,
                                                                 const char *tie_breaker,
                                                                 const char *content,
                                                                 const char *display_name,
                                                                 const char *handle,
                                                                 const char *avatar_url,
                                                                 const char *nip05,
                                                                 const char *root_id,
                                                                 const char *reply_id,
                                                                 const char *quoted_event_id,
                                                                 const char *reposted_event_id,
                                                                 const char * const *hashtags,
                                                                 gint kind,
                                                                 gboolean has_profile,
                                                                 guint like_count,
                                                                 gboolean is_liked,
                                                                 guint repost_count,
                                                                 guint reply_count,
                                                                 guint zap_count,
                                                                 gint64 zap_total_msat,
                                                                 double estimated_height,
                                                                 double measured_height,
                                                                 double effective_height,
                                                                 guint width_bucket,
                                                                 const char *layout_signature,
                                                                 gboolean geometry_measured);

GnostrTimelineSnapshotRow *gnostr_timeline_snapshot_row_new_from_view_model(GnostrTimelineItemViewModel *view_model,
                                                                            double estimated_height,
                                                                            double measured_height,
                                                                            double effective_height,
                                                                            guint width_bucket,
                                                                            const char *layout_signature,
                                                                            gboolean geometry_measured);

const char *gnostr_timeline_snapshot_row_get_event_id(GnostrTimelineSnapshotRow *self);
const char *gnostr_timeline_snapshot_row_get_note_key(GnostrTimelineSnapshotRow *self);
const char *gnostr_timeline_snapshot_row_get_pubkey(GnostrTimelineSnapshotRow *self);
gint64      gnostr_timeline_snapshot_row_get_created_at(GnostrTimelineSnapshotRow *self);
const char *gnostr_timeline_snapshot_row_get_tie_breaker(GnostrTimelineSnapshotRow *self);
const char *gnostr_timeline_snapshot_row_get_content(GnostrTimelineSnapshotRow *self);
const char *gnostr_timeline_snapshot_row_get_display_name(GnostrTimelineSnapshotRow *self);
const char *gnostr_timeline_snapshot_row_get_handle(GnostrTimelineSnapshotRow *self);
const char *gnostr_timeline_snapshot_row_get_avatar_url(GnostrTimelineSnapshotRow *self);
const char *gnostr_timeline_snapshot_row_get_nip05(GnostrTimelineSnapshotRow *self);
const char *gnostr_timeline_snapshot_row_get_root_id(GnostrTimelineSnapshotRow *self);
const char *gnostr_timeline_snapshot_row_get_reply_id(GnostrTimelineSnapshotRow *self);
const char *gnostr_timeline_snapshot_row_get_quoted_event_id(GnostrTimelineSnapshotRow *self);
const char *gnostr_timeline_snapshot_row_get_reposted_event_id(GnostrTimelineSnapshotRow *self);
const char * const *gnostr_timeline_snapshot_row_get_hashtags(GnostrTimelineSnapshotRow *self);
gint        gnostr_timeline_snapshot_row_get_kind(GnostrTimelineSnapshotRow *self);
gboolean    gnostr_timeline_snapshot_row_get_has_profile(GnostrTimelineSnapshotRow *self);
guint       gnostr_timeline_snapshot_row_get_like_count(GnostrTimelineSnapshotRow *self);
gboolean    gnostr_timeline_snapshot_row_get_is_liked(GnostrTimelineSnapshotRow *self);
guint       gnostr_timeline_snapshot_row_get_repost_count(GnostrTimelineSnapshotRow *self);
guint       gnostr_timeline_snapshot_row_get_reply_count(GnostrTimelineSnapshotRow *self);
guint       gnostr_timeline_snapshot_row_get_zap_count(GnostrTimelineSnapshotRow *self);
gint64      gnostr_timeline_snapshot_row_get_zap_total_msat(GnostrTimelineSnapshotRow *self);
double      gnostr_timeline_snapshot_row_get_estimated_height(GnostrTimelineSnapshotRow *self);
double      gnostr_timeline_snapshot_row_get_measured_height(GnostrTimelineSnapshotRow *self);
double      gnostr_timeline_snapshot_row_get_effective_height(GnostrTimelineSnapshotRow *self);
guint       gnostr_timeline_snapshot_row_get_width_bucket(GnostrTimelineSnapshotRow *self);
const char *gnostr_timeline_snapshot_row_get_layout_signature(GnostrTimelineSnapshotRow *self);
gboolean    gnostr_timeline_snapshot_row_get_geometry_measured(GnostrTimelineSnapshotRow *self);
GnostrTimelineItemViewModel *gnostr_timeline_snapshot_row_dup_view_model(GnostrTimelineSnapshotRow *self);

GnostrTimelineSnapshot *gnostr_timeline_snapshot_new(guint64 generation,
                                                      guint64 query_generation,
                                                      GnostrTimelineSnapshotRow * const *rows,
                                                      guint n_rows,
                                                      guint pending_head_count);

GnostrTimelineSnapshot *gnostr_timeline_snapshot_new_empty(guint64 generation,
                                                            guint64 query_generation);

guint64 gnostr_timeline_snapshot_get_generation(GnostrTimelineSnapshot *self);
guint64 gnostr_timeline_snapshot_get_query_generation(GnostrTimelineSnapshot *self);
guint   gnostr_timeline_snapshot_get_n_rows(GnostrTimelineSnapshot *self);
guint   gnostr_timeline_snapshot_get_pending_head_count(GnostrTimelineSnapshot *self);
double  gnostr_timeline_snapshot_get_total_height(GnostrTimelineSnapshot *self);

GnostrTimelineSnapshotRow *gnostr_timeline_snapshot_get_row(GnostrTimelineSnapshot *self,
                                                            guint index);
GnostrTimelineSnapshotRow *gnostr_timeline_snapshot_dup_row(GnostrTimelineSnapshot *self,
                                                            guint index);
gboolean gnostr_timeline_snapshot_lookup_event(GnostrTimelineSnapshot *self,
                                                const char *event_id,
                                                guint *out_index);
double   gnostr_timeline_snapshot_get_row_top(GnostrTimelineSnapshot *self,
                                               guint index);
double   gnostr_timeline_snapshot_get_row_bottom(GnostrTimelineSnapshot *self,
                                                  guint index);
gint     gnostr_timeline_snapshot_compare_rows(GnostrTimelineSnapshotRow *a,
                                                GnostrTimelineSnapshotRow *b);

G_END_DECLS

#endif /* GNOSTR_TIMELINE_SNAPSHOT_H */
