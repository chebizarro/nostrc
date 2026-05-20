#include <glib.h>
#include <string.h>

#include "../src/model/gnostr-timeline-geometry.h"

static const GnostrTimelineGeometryInput base_input = {
  .event_id = "event-a",
  .content = "hello nostr",
  .kind = 1,
  .has_profile = TRUE,
  .has_footer_action_reservation = TRUE,
};

static void
test_cache_key_includes_event_width_and_signature(void)
{
  g_autofree char *sig =
    gnostr_timeline_geometry_dup_layout_signature(&base_input, 480);
  g_autofree char *key =
    gnostr_timeline_geometry_dup_cache_key(base_input.event_id, 480, sig);

  g_autofree char *event_id = NULL;
  g_autofree char *parsed_sig = NULL;
  guint width_bucket = 0;
  g_assert_true(gnostr_timeline_geometry_parse_cache_key(key,
                                                         &event_id,
                                                         &width_bucket,
                                                         &parsed_sig));
  g_assert_cmpstr(event_id, ==, base_input.event_id);
  g_assert_cmpuint(width_bucket, ==, 480);
  g_assert_cmpstr(parsed_sig, ==, sig);

  g_autofree char *other_width =
    gnostr_timeline_geometry_dup_cache_key(base_input.event_id, 560, sig);
  g_assert_cmpstr(key, !=, other_width);

  GnostrTimelineGeometryInput quote_input = base_input;
  quote_input.has_quote_context_reservation = TRUE;
  g_autofree char *quote_sig =
    gnostr_timeline_geometry_dup_layout_signature(&quote_input, 480);
  g_autofree char *quote_key =
    gnostr_timeline_geometry_dup_cache_key(base_input.event_id, 480, quote_sig);
  g_assert_cmpstr(key, !=, quote_key);
}

static void
test_width_bucket_changes_do_not_reuse_measurement(void)
{
  g_autoptr(GnostrTimelineGeometryResolver) resolver =
    gnostr_timeline_geometry_resolver_new();

  GnostrTimelineGeometryInput input = base_input;
  input.content = "This is long content that wraps differently when the width bucket changes. "
                  "It should produce independent cache entries per material width bucket.";

  GnostrTimelineRowFootprint narrow = { 0 };
  gnostr_timeline_geometry_resolver_resolve(resolver, &input, 480, &narrow);
  gnostr_timeline_geometry_resolver_record_measurement(resolver,
                                                       input.event_id,
                                                       narrow.width_bucket,
                                                       narrow.layout_signature,
                                                       200.0);

  GnostrTimelineRowFootprint narrow_cached = { 0 };
  gnostr_timeline_geometry_resolver_resolve(resolver, &input, 480, &narrow_cached);
  g_assert_true(narrow_cached.geometry_measured);
  g_assert_cmpfloat_with_epsilon(narrow_cached.measured_height, 200.0, 8.0);

  GnostrTimelineRowFootprint wide = { 0 };
  gnostr_timeline_geometry_resolver_resolve(resolver, &input, 640, &wide);
  g_assert_false(wide.geometry_measured);
  g_assert_cmpuint(wide.width_bucket, !=, narrow.width_bucket);

  gnostr_timeline_row_footprint_clear(&narrow);
  gnostr_timeline_row_footprint_clear(&narrow_cached);
  gnostr_timeline_row_footprint_clear(&wide);
}

static void
test_measured_height_is_reused_when_it_fits_reserved_footprint(void)
{
  g_autoptr(GnostrTimelineGeometryResolver) resolver =
    gnostr_timeline_geometry_resolver_new();

  GnostrTimelineGeometryInput input = base_input;
  input.initial_reserved_height = 360.0;

  GnostrTimelineRowFootprint initial = { 0 };
  gnostr_timeline_geometry_resolver_resolve(resolver, &input, 480, &initial);
  g_assert_false(initial.geometry_measured);
  g_assert_cmpfloat_with_epsilon(initial.effective_height, 360.0, 0.001);

  gnostr_timeline_geometry_resolver_record_measurement(resolver,
                                                       input.event_id,
                                                       initial.width_bucket,
                                                       initial.layout_signature,
                                                       192.0);

  GnostrTimelineRowFootprint resolved = { 0 };
  gnostr_timeline_geometry_resolver_resolve(resolver, &input, 480, &resolved);
  g_assert_true(resolved.geometry_measured);
  g_assert_cmpfloat_with_epsilon(resolved.measured_height, 192.0, 8.0);
  g_assert_cmpfloat_with_epsilon(resolved.effective_height, initial.effective_height, 0.001);

  gnostr_timeline_row_footprint_clear(&initial);
  gnostr_timeline_row_footprint_clear(&resolved);
}

static void
test_passive_measurement_cannot_change_effective_height(void)
{
  g_autoptr(GnostrTimelineGeometryResolver) resolver =
    gnostr_timeline_geometry_resolver_new();

  GnostrTimelineGeometryInput input = base_input;
  input.initial_reserved_height = 320.0;
  input.media_reservation_count = 1;
  input.media_reserved_height = 220.0;

  GnostrTimelineRowFootprint initial = { 0 };
  gnostr_timeline_geometry_resolver_resolve(resolver, &input, 480, &initial);
  double reserved = initial.effective_height;

  gnostr_timeline_geometry_resolver_record_measurement(resolver,
                                                       input.event_id,
                                                       initial.width_bucket,
                                                       initial.layout_signature,
                                                       reserved + 240.0);

  GnostrTimelineRowFootprint passive = { 0 };
  gnostr_timeline_geometry_resolver_resolve(resolver, &input, 480, &passive);
  g_assert_true(passive.geometry_measured);
  g_assert_cmpfloat(passive.measured_height, >, reserved);
  g_assert_cmpfloat_with_epsilon(passive.effective_height, reserved, 0.001);

  gnostr_timeline_geometry_resolver_record_measurement(resolver,
                                                       input.event_id,
                                                       initial.width_bucket,
                                                       initial.layout_signature,
                                                       reserved - 160.0);

  GnostrTimelineRowFootprint smaller = { 0 };
  gnostr_timeline_geometry_resolver_resolve(resolver, &input, 480, &smaller);
  g_assert_true(smaller.geometry_measured);
  g_assert_cmpfloat(smaller.measured_height, <, reserved);
  g_assert_cmpfloat_with_epsilon(smaller.effective_height, reserved, 0.001);

  input.explicit_expanded = TRUE;
  GnostrTimelineRowFootprint explicit = { 0 };
  gnostr_timeline_geometry_resolver_record_measurement(resolver,
                                                       input.event_id,
                                                       initial.width_bucket,
                                                       initial.layout_signature,
                                                       reserved + 240.0);
  gnostr_timeline_geometry_resolver_resolve(resolver, &input, 480, &explicit);
  g_assert_cmpfloat(explicit.effective_height, >, reserved);

  gnostr_timeline_row_footprint_clear(&initial);
  gnostr_timeline_row_footprint_clear(&passive);
  gnostr_timeline_row_footprint_clear(&smaller);
  gnostr_timeline_row_footprint_clear(&explicit);
}

static void
test_rich_areas_reserve_additional_height(void)
{
  g_autoptr(GnostrTimelineGeometryResolver) resolver =
    gnostr_timeline_geometry_resolver_new();

  GnostrTimelineRowFootprint plain = { 0 };
  gnostr_timeline_geometry_resolver_resolve(resolver, &base_input, 480, &plain);

  GnostrTimelineGeometryInput rich = base_input;
  rich.has_reply_context_reservation = TRUE;
  rich.has_repost_context_reservation = TRUE;
  rich.has_quote_context_reservation = TRUE;
  rich.media_reservation_count = 2;
  rich.media_reserved_height = 440.0;
  rich.link_preview_reservation_count = 1;
  rich.link_preview_reserved_height = 128.0;
  rich.has_content_warning = TRUE;
  rich.moderation_state = 1;

  GnostrTimelineRowFootprint rich_footprint = { 0 };
  gnostr_timeline_geometry_resolver_resolve(resolver, &rich, 480, &rich_footprint);

  g_assert_cmpfloat(rich_footprint.estimated_height, >, plain.estimated_height);
  g_assert_nonnull(strstr(rich_footprint.layout_signature, "replyctx1"));
  g_assert_nonnull(strstr(rich_footprint.layout_signature, "repostctx1"));
  g_assert_nonnull(strstr(rich_footprint.layout_signature, "quotectx1"));
  g_assert_nonnull(strstr(rich_footprint.layout_signature, "media2@440"));
  g_assert_nonnull(strstr(rich_footprint.layout_signature, "link1@128"));
  g_assert_nonnull(strstr(rich_footprint.layout_signature, "cw1"));
  g_assert_nonnull(strstr(rich_footprint.layout_signature, "mod1"));
  g_assert_nonnull(strstr(rich_footprint.layout_signature, "footer1"));

  gnostr_timeline_row_footprint_clear(&plain);
  gnostr_timeline_row_footprint_clear(&rich_footprint);
}

static void
test_explicit_reservation_fields_control_height_not_url_sniffing(void)
{
  g_autoptr(GnostrTimelineGeometryResolver) resolver =
    gnostr_timeline_geometry_resolver_new();

  GnostrTimelineGeometryInput url_only = base_input;
  url_only.content = "raw URL should not reserve rich boxes https://example.test/pic.jpg";

  GnostrTimelineGeometryInput explicit_media = url_only;
  explicit_media.media_reservation_count = 1;
  explicit_media.media_reserved_height = 220.0;
  explicit_media.link_preview_reservation_count = 1;
  explicit_media.link_preview_reserved_height = 120.0;
  explicit_media.has_reply_context_reservation = TRUE;

  GnostrTimelineRowFootprint url_footprint = { 0 };
  GnostrTimelineRowFootprint explicit_footprint = { 0 };
  gnostr_timeline_geometry_resolver_resolve(resolver, &url_only, 480, &url_footprint);
  gnostr_timeline_geometry_resolver_resolve(resolver, &explicit_media, 480, &explicit_footprint);

  g_assert_cmpfloat(explicit_footprint.estimated_height, >, url_footprint.estimated_height);
  g_assert_nonnull(strstr(explicit_footprint.layout_signature, "media1@220"));
  g_assert_nonnull(strstr(explicit_footprint.layout_signature, "link1@120"));
  g_assert_null(strstr(url_footprint.layout_signature, "media1@"));
  g_assert_null(strstr(url_footprint.layout_signature, "link1@"));

  gnostr_timeline_row_footprint_clear(&url_footprint);
  gnostr_timeline_row_footprint_clear(&explicit_footprint);
}

int
main(int argc,
     char **argv)
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/gnostr/timeline-geometry/cache-key",
                  test_cache_key_includes_event_width_and_signature);
  g_test_add_func("/gnostr/timeline-geometry/width-bucket-change",
                  test_width_bucket_changes_do_not_reuse_measurement);
  g_test_add_func("/gnostr/timeline-geometry/measured-height-reuse",
                  test_measured_height_is_reused_when_it_fits_reserved_footprint);
  g_test_add_func("/gnostr/timeline-geometry/no-passive-expansion",
                  test_passive_measurement_cannot_change_effective_height);
  g_test_add_func("/gnostr/timeline-geometry/rich-area-reservations",
                  test_rich_areas_reserve_additional_height);
  g_test_add_func("/gnostr/timeline-geometry/explicit-reservation-fields",
                  test_explicit_reservation_fields_control_height_not_url_sniffing);

  return g_test_run();
}
