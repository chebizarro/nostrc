#include "gnostr-timeline-geometry.h"

#include <string.h>

#define MIN_ROW_HEIGHT_PX       160.0
#define ROW_CHROME_PX           88.0
#define AVATAR_HEADER_PX        56.0
#define FOOTER_PX               44.0
#define THREAD_CONTEXT_PX       28.0
#define REPOST_CONTEXT_PX       28.0
#define QUOTE_CONTEXT_PX        112.0
#define PROFILE_FALLBACK_PX     0.0
#define CONTENT_WARNING_PX      56.0
#define MODERATION_COLLAPSED_PX 96.0
#define LINE_HEIGHT_PX          20.0
#define TEXT_VERTICAL_PAD_PX    12.0
#define MAX_COLLAPSED_LINES     12u
#define HEIGHT_QUANTUM_PX       8.0

typedef struct {
  double measured_height;
} CachedMeasurement;

struct _GnostrTimelineGeometryResolver {
  GHashTable *measurements; /* char* cache-key -> CachedMeasurement* */
};

static void
cached_measurement_free(CachedMeasurement *measurement)
{
  g_free(measurement);
}

static guint
content_text_bucket(const char *content)
{
  if (!content || !*content)
    return 0;

  glong chars = g_utf8_strlen(content, -1);
  if (chars <= 0)
    chars = (glong)strlen(content);

  return (guint)MIN(9999, ((chars + 39) / 40) * 40);
}

static guint
estimate_text_lines(const char *content,
                    guint width_bucket)
{
  if (!content || !*content)
    return 1;

  guint content_width = width_bucket > 112 ? width_bucket - 112 : 160;
  guint chars_per_line = MAX(20u, content_width / 7u);
  glong chars = g_utf8_strlen(content, -1);
  if (chars <= 0)
    chars = (glong)strlen(content);

  guint hard_breaks = 0;
  for (const char *p = content; *p; p++) {
    if (*p == '\n')
      hard_breaks++;
  }

  guint wrapped = (guint)(((MAX(chars, 1) + chars_per_line - 1) / chars_per_line));
  return CLAMP(wrapped + hard_breaks, 1u, MAX_COLLAPSED_LINES);
}

static double
quantize_height(double value)
{
  if (value <= 0.0)
    return MIN_ROW_HEIGHT_PX;

  guint units = (guint)(value / HEIGHT_QUANTUM_PX);
  if (((double)units * HEIGHT_QUANTUM_PX) < value)
    units++;
  return (double)units * HEIGHT_QUANTUM_PX;
}

GnostrTimelineGeometryResolver *
gnostr_timeline_geometry_resolver_new(void)
{
  GnostrTimelineGeometryResolver *self = g_new0(GnostrTimelineGeometryResolver, 1);
  self->measurements = g_hash_table_new_full(g_str_hash,
                                             g_str_equal,
                                             g_free,
                                             (GDestroyNotify)cached_measurement_free);
  return self;
}

void
gnostr_timeline_geometry_resolver_free(GnostrTimelineGeometryResolver *self)
{
  if (!self)
    return;
  g_clear_pointer(&self->measurements, g_hash_table_unref);
  g_free(self);
}

void
gnostr_timeline_row_footprint_clear(GnostrTimelineRowFootprint *footprint)
{
  if (!footprint)
    return;
  g_clear_pointer(&footprint->layout_signature, g_free);
  footprint->estimated_height = 0.0;
  footprint->measured_height = 0.0;
  footprint->effective_height = 0.0;
  footprint->width_bucket = 0;
  footprint->geometry_measured = FALSE;
}

guint
gnostr_timeline_geometry_width_to_bucket(guint width_px)
{
  if (width_px == 0)
    return GNOSTR_TIMELINE_GEOMETRY_DEFAULT_WIDTH_BUCKET;

  /* Bucket by 80px increments so small resize jitter does not invalidate every
   * geometry entry, while material layout width changes still get distinct keys. */
  guint bucket = ((width_px + 39u) / 80u) * 80u;
  return MAX(bucket, 80u);
}

char *
gnostr_timeline_geometry_dup_layout_signature(const GnostrTimelineGeometryInput *input,
                                              guint width_bucket)
{
  (void)width_bucket;

  if (input && input->geometry_signature && *input->geometry_signature) {
    return g_strdup_printf("%s:%s", GNOSTR_TIMELINE_GEOMETRY_LAYOUT_VERSION,
                           input->geometry_signature);
  }

  gboolean has_thread = input && input->has_reply_context_reservation;
  gboolean has_repost = input && input->has_repost_context_reservation;
  gboolean has_quote = input && input->has_quote_context_reservation;
  gboolean has_footer = !input || input->has_footer_action_reservation;
  guint media_count = input ? input->media_reservation_count : 0;
  guint link_count = input ? input->link_preview_reservation_count : 0;
  guint text_bucket = input ? content_text_bucket(input->content) : 0;

  return g_strdup_printf("%s:text%u:replyctx%d:repostctx%d:quotectx%d:media%u@%.0f:link%u@%.0f:footer%d:cw%d:mod%d:kind%d",
                         GNOSTR_TIMELINE_GEOMETRY_LAYOUT_VERSION,
                         text_bucket,
                         has_thread ? 1 : 0,
                         has_repost ? 1 : 0,
                         has_quote ? 1 : 0,
                         media_count,
                         input ? input->media_reserved_height : 0.0,
                         link_count,
                         input ? input->link_preview_reserved_height : 0.0,
                         has_footer ? 1 : 0,
                         (input && input->has_content_warning) ? 1 : 0,
                         input ? input->moderation_state : 0,
                         input ? input->kind : 1);
}

char *
gnostr_timeline_geometry_dup_cache_key(const char *event_id,
                                       guint width_bucket,
                                       const char *layout_signature)
{
  return g_strdup_printf("%s|%u|%s",
                         event_id ? event_id : "",
                         width_bucket,
                         layout_signature ? layout_signature : GNOSTR_TIMELINE_GEOMETRY_LAYOUT_VERSION);
}

gboolean
gnostr_timeline_geometry_parse_cache_key(const char *cache_key,
                                         char **out_event_id,
                                         guint *out_width_bucket,
                                         char **out_layout_signature)
{
  if (!cache_key || !*cache_key)
    return FALSE;

  g_auto(GStrv) parts = g_strsplit(cache_key, "|", 3);
  if (!parts[0] || !*parts[0] || !parts[1] || !parts[2])
    return FALSE;

  guint64 width = g_ascii_strtoull(parts[1], NULL, 10);
  if (width == 0 || width > G_MAXUINT)
    return FALSE;

  if (out_event_id)
    *out_event_id = g_strdup(parts[0]);
  if (out_width_bucket)
    *out_width_bucket = (guint)width;
  if (out_layout_signature)
    *out_layout_signature = g_strdup(parts[2]);

  return TRUE;
}

void
gnostr_timeline_geometry_resolver_record_measurement(GnostrTimelineGeometryResolver *self,
                                                     const char *event_id,
                                                     guint width_bucket,
                                                     const char *layout_signature,
                                                     double measured_height)
{
  g_return_if_fail(self != NULL);

  if (!event_id || !*event_id || width_bucket == 0 || measured_height <= 0.0)
    return;

  g_autofree char *cache_key =
    gnostr_timeline_geometry_dup_cache_key(event_id, width_bucket, layout_signature);
  gnostr_timeline_geometry_resolver_record_measurement_for_key(self,
                                                               cache_key,
                                                               measured_height);
}

void
gnostr_timeline_geometry_resolver_record_measurement_for_key(GnostrTimelineGeometryResolver *self,
                                                             const char *cache_key,
                                                             double measured_height)
{
  g_return_if_fail(self != NULL);

  if (!cache_key || !*cache_key || measured_height <= 0.0)
    return;

  CachedMeasurement *measurement = g_new0(CachedMeasurement, 1);
  measurement->measured_height = quantize_height(measured_height);
  g_hash_table_replace(self->measurements, g_strdup(cache_key), measurement);
}

gboolean
gnostr_timeline_geometry_resolver_lookup_measurement(GnostrTimelineGeometryResolver *self,
                                                     const char *event_id,
                                                     guint width_bucket,
                                                     const char *layout_signature,
                                                     double *out_measured_height)
{
  g_return_val_if_fail(self != NULL, FALSE);

  g_autofree char *cache_key =
    gnostr_timeline_geometry_dup_cache_key(event_id, width_bucket, layout_signature);
  CachedMeasurement *measurement = g_hash_table_lookup(self->measurements, cache_key);
  if (!measurement)
    return FALSE;

  if (out_measured_height)
    *out_measured_height = measurement->measured_height;
  return TRUE;
}

void
gnostr_timeline_geometry_resolver_resolve(GnostrTimelineGeometryResolver *self,
                                          const GnostrTimelineGeometryInput *input,
                                          guint width_bucket,
                                          GnostrTimelineRowFootprint *out_footprint)
{
  g_return_if_fail(self != NULL);
  g_return_if_fail(out_footprint != NULL);

  memset(out_footprint, 0, sizeof(*out_footprint));
  if (width_bucket == 0)
    width_bucket = GNOSTR_TIMELINE_GEOMETRY_DEFAULT_WIDTH_BUCKET;

  out_footprint->width_bucket = width_bucket;
  out_footprint->layout_signature =
    gnostr_timeline_geometry_dup_layout_signature(input, width_bucket);

  gboolean has_thread = input && input->has_reply_context_reservation;
  gboolean has_repost = input && input->has_repost_context_reservation;
  gboolean has_quote = input && input->has_quote_context_reservation;
  gboolean has_footer = !input || input->has_footer_action_reservation;

  double estimate = input ? input->initial_reserved_height : 0.0;

  if (estimate <= 0.0) {
    estimate = ROW_CHROME_PX + AVATAR_HEADER_PX + PROFILE_FALLBACK_PX;

    guint text_lines = estimate_text_lines(input ? input->content : NULL, width_bucket);
    estimate += TEXT_VERTICAL_PAD_PX + ((double)text_lines * LINE_HEIGHT_PX);

    if (has_thread)
      estimate += THREAD_CONTEXT_PX;
    if (has_repost)
      estimate += REPOST_CONTEXT_PX;
    if (has_quote)
      estimate += QUOTE_CONTEXT_PX;
    if (input && input->media_reservation_count > 0)
      estimate += input->media_reserved_height;
    if (input && input->link_preview_reservation_count > 0)
      estimate += input->link_preview_reserved_height;
    if (input && input->has_content_warning)
      estimate += CONTENT_WARNING_PX;
    if (input && input->moderation_state != 0)
      estimate = MAX(estimate, MODERATION_COLLAPSED_PX);
    if (has_footer)
      estimate += FOOTER_PX;
  }

  out_footprint->estimated_height = MAX(MIN_ROW_HEIGHT_PX, quantize_height(estimate));
  out_footprint->effective_height = out_footprint->estimated_height;

  double measured = 0.0;
  if (input && input->event_id &&
      gnostr_timeline_geometry_resolver_lookup_measurement(self,
                                                           input->event_id,
                                                           width_bucket,
                                                           out_footprint->layout_signature,
                                                           &measured)) {
    out_footprint->measured_height = measured;
    out_footprint->geometry_measured = TRUE;

    if (input->explicit_expanded)
      out_footprint->effective_height = MAX(out_footprint->estimated_height, measured);
  }
}
