#ifndef GNOSTR_TIMELINE_GEOMETRY_H
#define GNOSTR_TIMELINE_GEOMETRY_H

#include <glib.h>

G_BEGIN_DECLS

#define GNOSTR_TIMELINE_GEOMETRY_DEFAULT_WIDTH_BUCKET 480u
#define GNOSTR_TIMELINE_GEOMETRY_LAYOUT_VERSION "timeline-geometry-v2"

typedef struct _GnostrTimelineGeometryResolver GnostrTimelineGeometryResolver;

typedef struct {
  const char *event_id;
  const char *content;
  const char *root_id;
  const char *reply_id;
  const char *quoted_event_id;
  const char *reposted_event_id;
  const char *geometry_signature;
  gint        kind;
  gboolean    has_profile;
  gint        moderation_state;
  gboolean    has_content_warning;
  guint       media_reservation_count;
  double      media_reserved_height;
  guint       link_preview_reservation_count;
  double      link_preview_reserved_height;
  gboolean    has_reply_context_reservation;
  gboolean    has_repost_context_reservation;
  gboolean    has_quote_context_reservation;
  gboolean    has_footer_action_reservation;
  double      initial_reserved_height;
  guint       like_count;
  guint       repost_count;
  guint       reply_count;
  guint       zap_count;
  gboolean    explicit_expanded;
} GnostrTimelineGeometryInput;

typedef struct {
  double   estimated_height;
  double   measured_height;
  double   effective_height;
  double   media_reserved_height;
  double   link_preview_reserved_height;
  guint    width_bucket;
  char    *layout_signature;
  gboolean geometry_measured;
} GnostrTimelineRowFootprint;

GnostrTimelineGeometryResolver *gnostr_timeline_geometry_resolver_new(void);
void gnostr_timeline_geometry_resolver_free(GnostrTimelineGeometryResolver *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GnostrTimelineGeometryResolver,
                              gnostr_timeline_geometry_resolver_free)

void gnostr_timeline_row_footprint_clear(GnostrTimelineRowFootprint *footprint);

guint gnostr_timeline_geometry_width_to_bucket(guint width_px);

char *gnostr_timeline_geometry_dup_layout_signature(const GnostrTimelineGeometryInput *input,
                                                    guint width_bucket);
char *gnostr_timeline_geometry_dup_cache_key(const char *event_id,
                                             guint width_bucket,
                                             const char *layout_signature);
gboolean gnostr_timeline_geometry_parse_cache_key(const char *cache_key,
                                                  char **out_event_id,
                                                  guint *out_width_bucket,
                                                  char **out_layout_signature);

void gnostr_timeline_geometry_resolver_record_measurement(GnostrTimelineGeometryResolver *self,
                                                          const char *event_id,
                                                          guint width_bucket,
                                                          const char *layout_signature,
                                                          double measured_height);
void gnostr_timeline_geometry_resolver_record_measurement_for_key(GnostrTimelineGeometryResolver *self,
                                                                  const char *cache_key,
                                                                  double measured_height);

gboolean gnostr_timeline_geometry_resolver_lookup_measurement(GnostrTimelineGeometryResolver *self,
                                                              const char *event_id,
                                                              guint width_bucket,
                                                              const char *layout_signature,
                                                              double *out_measured_height);

void gnostr_timeline_geometry_resolver_resolve(GnostrTimelineGeometryResolver *self,
                                               const GnostrTimelineGeometryInput *input,
                                               guint width_bucket,
                                               GnostrTimelineRowFootprint *out_footprint);

G_END_DECLS

#endif /* GNOSTR_TIMELINE_GEOMETRY_H */
