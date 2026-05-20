#include <glib.h>
#include <gio/gio.h>
#include <string.h>

#include "../src/model/gnostr-timeline-batch.h"
#include "../src/model/gnostr-timeline-hydrator.h"

static void
fill_id(guint8 out[32],
        guint8 byte)
{
  memset(out, byte, 32);
}

static GnostrTimelineBatch *
batch_new(GnostrTimelineBatchKind kind,
          guint64 generation)
{
  return gnostr_timeline_batch_new(kind, generation);
}

static void
batch_add(GnostrTimelineBatch *batch,
          guint64 note_key,
          gint64 created_at,
          guint8 id_byte,
          const char *content,
          const char *display_name,
          const char *handle,
          const char *avatar_url,
          gboolean has_profile)
{
  guint8 id[32];
  fill_id(id, id_byte);
  gnostr_timeline_batch_add_note(batch,
                                 note_key,
                                 created_at,
                                 id,
                                 "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
                                 content,
                                 display_name,
                                 handle,
                                 avatar_url,
                                 has_profile ? "name@example.test" : NULL,
                                 NULL,
                                 NULL,
                                 1,
                                 has_profile);
}

static char *
event_id_hex_for_byte(guint8 byte)
{
  guint8 id[32];
  fill_id(id, byte);

  char *hex = g_malloc0(65);
  for (guint i = 0; i < 32; i++)
    g_snprintf(hex + (i * 2), 3, "%02x", id[i]);
  return hex;
}

static void
test_sorting_and_dedup_for_display_batches(void)
{
  GnostrTimelineHydrator *hydrator = gnostr_timeline_hydrator_new(7);

  GnostrTimelineBatch *refresh = batch_new(GNOSTR_TIMELINE_BATCH_REFRESH, 7);
  batch_add(refresh, 1, 100, 0x33, "older", "A", "a", NULL, TRUE);
  batch_add(refresh, 2, 200, 0x22, "newer-b", "B", "b", NULL, TRUE);
  batch_add(refresh, 3, 200, 0x11, "newer-a", "C", "c", NULL, TRUE);
  batch_add(refresh, 4, 300, 0x11, "duplicate-newer", "D", "d", NULL, TRUE);

  g_autoptr(GPtrArray) items = gnostr_timeline_hydrator_hydrate_batch(hydrator, refresh);
  g_assert_nonnull(items);
  g_assert_cmpuint(items->len, ==, 3);

  g_autofree char *id11 = event_id_hex_for_byte(0x11);
  g_autofree char *id22 = event_id_hex_for_byte(0x22);
  g_autofree char *id33 = event_id_hex_for_byte(0x33);
  GnostrTimelineItemViewModel *first = g_ptr_array_index(items, 0);
  GnostrTimelineItemViewModel *second = g_ptr_array_index(items, 1);
  GnostrTimelineItemViewModel *third = g_ptr_array_index(items, 2);
  g_assert_cmpstr(gnostr_timeline_item_view_model_get_event_id(first), ==, id11);
  g_assert_cmpstr(gnostr_timeline_item_view_model_get_event_id(second), ==, id22);
  g_assert_cmpstr(gnostr_timeline_item_view_model_get_event_id(third), ==, id33);

  g_object_unref(refresh);

  GnostrTimelineBatch *live = batch_new(GNOSTR_TIMELINE_BATCH_LIVE_HEAD, 7);
  batch_add(live, 5, 400, 0x44, "live", "Live", "live", NULL, TRUE);
  g_autoptr(GPtrArray) live_items = gnostr_timeline_hydrator_hydrate_batch(hydrator, live);
  g_assert_nonnull(live_items);
  g_assert_cmpuint(live_items->len, ==, 1);
  g_object_unref(live);

  GnostrTimelineBatch *older = batch_new(GNOSTR_TIMELINE_BATCH_PAGE_OLDER, 7);
  batch_add(older, 6, 50, 0x55, "older", "Older", "older", NULL, TRUE);
  g_autoptr(GPtrArray) older_items = gnostr_timeline_hydrator_hydrate_batch(hydrator, older);
  g_assert_nonnull(older_items);
  g_assert_cmpuint(older_items->len, ==, 1);
  g_object_unref(older);

  g_object_unref(hydrator);
}

static void
test_quote_repost_and_action_vm_data_are_carried(void)
{
  GnostrTimelineHydrator *hydrator = gnostr_timeline_hydrator_new(4);
  GnostrTimelineBatch *batch = batch_new(GNOSTR_TIMELINE_BATCH_REFRESH, 4);

  guint8 id[32];
  fill_id(id, 0x88);
  GnostrTimelineBatchEntry entry = {
    .note_key = 11,
    .created_at = 222,
    .pubkey_hex = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    .content = "quoting and reposting",
    .display_name = "Author",
    .avatar_url = "https://example.test/a.png",
    .root_id = "1111111111111111111111111111111111111111111111111111111111111111",
    .reply_id = "2222222222222222222222222222222222222222222222222222222222222222",
    .parent_pubkey = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
    .parent_display_name = "Parent Author",
    .quoted_event_id = "3333333333333333333333333333333333333333333333333333333333333333",
    .quoted_pubkey = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
    .quoted_display_name = "Quote Author",
    .quoted_content = "quoted content <b>needs escaping</b>",
    .quoted_created_at = 111,
    .quoted_kind = 1,
    .quoted_resolved = TRUE,
    .reposted_event_id = "4444444444444444444444444444444444444444444444444444444444444444",
    .reposted_pubkey = "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
    .reposted_display_name = "Original Author",
    .reposted_avatar_url = "https://example.test/original.png",
    .reposted_nip05 = "original@example.test",
    .reposted_content = "original reposted note",
    .reposted_created_at = 99,
    .reposted_kind = 1,
    .reposted_resolved = TRUE,
    .kind = 6,
    .has_profile = TRUE,
    .logged_in = TRUE,
    .is_bookmarked = TRUE,
    .is_pinned = TRUE,
    .zap_target = "lnurl1target",
  };
  memcpy(entry.event_id, id, sizeof(id));
  gnostr_timeline_batch_add_entry(batch, &entry);

  g_autoptr(GPtrArray) items = gnostr_timeline_hydrator_hydrate_batch(hydrator, batch);
  g_assert_nonnull(items);
  g_assert_cmpuint(items->len, ==, 1);
  GnostrTimelineItemViewModel *vm = g_ptr_array_index(items, 0);

  g_assert_true(gnostr_timeline_item_view_model_get_parent_available(vm));
  g_assert_cmpstr(gnostr_timeline_item_view_model_get_parent_pubkey(vm), ==, entry.parent_pubkey);
  g_assert_cmpstr(gnostr_timeline_item_view_model_get_parent_display_name(vm), ==, "Parent Author");
  g_assert_cmpuint(gnostr_timeline_item_view_model_get_context_reservation_count(vm), ==, 1);

  g_assert_cmpint(gnostr_timeline_item_view_model_get_quote_state(vm), ==, GNOSTR_TIMELINE_PREVIEW_RESOLVED);
  g_assert_cmpstr(gnostr_timeline_item_view_model_get_quoted_event_id(vm), ==, entry.quoted_event_id);
  g_assert_cmpstr(gnostr_timeline_item_view_model_get_quoted_pubkey(vm), ==, entry.quoted_pubkey);
  g_assert_cmpstr(gnostr_timeline_item_view_model_get_quoted_display_name(vm), ==, "Quote Author");
  g_assert_cmpstr(gnostr_timeline_item_view_model_get_quoted_content(vm), ==, "quoted content <b>needs escaping</b>");
  g_assert_cmpstr(gnostr_timeline_item_view_model_get_quoted_rendered_content(vm), ==, "quoted content &lt;b&gt;needs escaping&lt;/b&gt;");
  g_assert_cmpint(gnostr_timeline_item_view_model_get_quoted_created_at(vm), ==, 111);
  g_assert_cmpint(gnostr_timeline_item_view_model_get_quoted_kind(vm), ==, 1);
  g_assert_cmpuint(gnostr_timeline_item_view_model_get_quote_preview_reservation_count(vm), ==, 1);

  g_assert_cmpint(gnostr_timeline_item_view_model_get_repost_state(vm), ==, GNOSTR_TIMELINE_PREVIEW_RESOLVED);
  g_assert_cmpstr(gnostr_timeline_item_view_model_get_reposted_event_id(vm), ==, entry.reposted_event_id);
  g_assert_cmpstr(gnostr_timeline_item_view_model_get_reposted_pubkey(vm), ==, entry.reposted_pubkey);
  g_assert_cmpstr(gnostr_timeline_item_view_model_get_reposted_display_name(vm), ==, "Original Author");
  g_assert_cmpstr(gnostr_timeline_item_view_model_get_reposted_avatar_url(vm), ==, "https://example.test/original.png");
  g_assert_cmpstr(gnostr_timeline_item_view_model_get_reposted_nip05(vm), ==, "original@example.test");
  g_assert_cmpstr(gnostr_timeline_item_view_model_get_reposted_content(vm), ==, "original reposted note");
  g_assert_cmpstr(gnostr_timeline_item_view_model_get_reposted_rendered_content(vm), ==, "original reposted note");
  g_assert_cmpint(gnostr_timeline_item_view_model_get_reposted_created_at(vm), ==, 99);
  g_assert_cmpuint(gnostr_timeline_item_view_model_get_repost_preview_reservation_count(vm), ==, 1);

  g_autofree char *id88 = event_id_hex_for_byte(0x88);
  g_assert_cmpstr(gnostr_timeline_item_view_model_get_action_event_id(vm), ==, id88);
  g_assert_cmpstr(gnostr_timeline_item_view_model_get_action_pubkey(vm), ==, entry.pubkey_hex);
  g_assert_true(gnostr_timeline_item_view_model_get_action_logged_in(vm));
  g_assert_true(gnostr_timeline_item_view_model_get_action_is_bookmarked(vm));
  g_assert_true(gnostr_timeline_item_view_model_get_action_is_pinned(vm));
  g_assert_cmpstr(gnostr_timeline_item_view_model_get_action_zap_target(vm), ==, "lnurl1target");
  g_assert_cmpuint(gnostr_timeline_item_view_model_get_footer_action_reservation_count(vm), ==, 1);

  g_object_unref(batch);
  g_object_unref(hydrator);
}

static void
test_missing_preview_states_are_deterministic(void)
{
  GnostrTimelineHydrator *hydrator = gnostr_timeline_hydrator_new(5);
  GnostrTimelineBatch *batch = batch_new(GNOSTR_TIMELINE_BATCH_REFRESH, 5);

  guint8 id[32];
  fill_id(id, 0x99);
  GnostrTimelineBatchEntry entry = {
    .note_key = 12,
    .created_at = 333,
    .pubkey_hex = "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
    .content = "missing previews",
    .display_name = "Author",
    .quoted_event_id = "5555555555555555555555555555555555555555555555555555555555555555",
    .reposted_event_id = "6666666666666666666666666666666666666666666666666666666666666666",
    .kind = 6,
    .has_profile = TRUE,
  };
  memcpy(entry.event_id, id, sizeof(id));
  gnostr_timeline_batch_add_entry(batch, &entry);

  g_autoptr(GPtrArray) items = gnostr_timeline_hydrator_hydrate_batch(hydrator, batch);
  g_assert_nonnull(items);
  GnostrTimelineItemViewModel *vm = g_ptr_array_index(items, 0);
  g_assert_cmpint(gnostr_timeline_item_view_model_get_quote_state(vm), ==, GNOSTR_TIMELINE_PREVIEW_MISSING);
  g_assert_cmpint(gnostr_timeline_item_view_model_get_repost_state(vm), ==, GNOSTR_TIMELINE_PREVIEW_MISSING);
  g_assert_null(gnostr_timeline_item_view_model_get_quoted_content(vm));
  g_assert_null(gnostr_timeline_item_view_model_get_reposted_content(vm));
  g_assert_cmpuint(gnostr_timeline_item_view_model_get_quote_preview_reservation_count(vm), ==, 1);
  g_assert_cmpuint(gnostr_timeline_item_view_model_get_repost_preview_reservation_count(vm), ==, 1);
  g_assert_cmpstr(gnostr_timeline_item_view_model_get_action_zap_target(vm), ==, entry.pubkey_hex);

  g_object_unref(batch);
  g_object_unref(hydrator);
}

static void
test_missing_metadata_fallbacks_and_reservations(void)
{
  GnostrTimelineHydrator *hydrator = gnostr_timeline_hydrator_new(3);
  GnostrTimelineBatch *batch = batch_new(GNOSTR_TIMELINE_BATCH_REFRESH, 3);

  guint8 id[32];
  fill_id(id, 0x77);
  const char *hashtags[] = { "explicit", NULL };
  GnostrTimelineBatchEntry entry = {
    .note_key = 10,
    .created_at = 123,
    .pubkey_hex = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
    .content = "hello #nostr @alice https://example.test/post https://example.test/image.jpg",
    .root_id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    .reply_id = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
    .quoted_event_id = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
    .reposted_event_id = "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
    .hashtags = (char **)hashtags,
    .content_warning = "spoiler",
    .kind = 6,
    .has_profile = FALSE,
  };
  memcpy(entry.event_id, id, sizeof(id));
  gnostr_timeline_batch_add_entry(batch, &entry);

  g_autoptr(GPtrArray) items = gnostr_timeline_hydrator_hydrate_batch(hydrator, batch);
  g_assert_nonnull(items);
  g_assert_cmpuint(items->len, ==, 1);
  GnostrTimelineItemViewModel *vm = g_ptr_array_index(items, 0);

  g_assert_cmpstr(gnostr_timeline_item_view_model_get_display_name(vm), ==, "01234567...");
  g_assert_cmpstr(gnostr_timeline_item_view_model_get_avatar_url(vm), ==, NULL);
  g_assert_cmpstr(gnostr_timeline_item_view_model_get_avatar_fallback_label(vm), ==, "0");
  g_assert_cmpint(gnostr_timeline_item_view_model_get_avatar_state(vm), ==, GNOSTR_TIMELINE_AVATAR_FALLBACK);
  g_assert_false(gnostr_timeline_item_view_model_get_has_profile(vm));
  g_assert_cmpint(gnostr_timeline_item_view_model_get_moderation_state(vm), ==, GNOSTR_TIMELINE_MODERATION_CONTENT_WARNING);

  const char * const *hashtags_out = gnostr_timeline_item_view_model_get_hashtags(vm);
  g_assert_nonnull(hashtags_out);
  g_assert_cmpstr(hashtags_out[0], ==, "explicit");
  g_assert_cmpstr(hashtags_out[1], ==, "nostr");

  const char * const *mentions = gnostr_timeline_item_view_model_get_mentions(vm);
  g_assert_nonnull(mentions);
  g_assert_cmpstr(mentions[0], ==, "alice");

  const char * const *links = gnostr_timeline_item_view_model_get_links(vm);
  const char * const *media = gnostr_timeline_item_view_model_get_media_urls(vm);
  g_assert_nonnull(links);
  g_assert_nonnull(media);
  g_assert_cmpstr(links[0], ==, "https://example.test/post");
  g_assert_cmpstr(media[0], ==, "https://example.test/image.jpg");
  g_assert_cmpuint(gnostr_timeline_item_view_model_get_link_preview_reservation_count(vm), ==, 1);
  g_assert_cmpuint(gnostr_timeline_item_view_model_get_media_reservation_count(vm), ==, 1);
  g_assert_true(gnostr_timeline_item_view_model_get_has_reply_context_reservation(vm));
  g_assert_true(gnostr_timeline_item_view_model_get_has_quote_context_reservation(vm));
  g_assert_true(gnostr_timeline_item_view_model_get_has_repost_context_reservation(vm));
  g_assert_cmpfloat(gnostr_timeline_item_view_model_get_initial_reserved_height(vm), >, 0.0);
  g_assert_nonnull(gnostr_timeline_item_view_model_get_geometry_signature(vm));

  g_object_unref(batch);
  g_object_unref(hydrator);
}

typedef struct {
  gboolean done;
  gboolean got_items;
} AsyncHydrateCapture;

static void
on_hydrate_done(GObject *source,
                GAsyncResult *result,
                gpointer user_data)
{
  AsyncHydrateCapture *capture = user_data;
  g_autoptr(GPtrArray) items =
    gnostr_timeline_hydrator_hydrate_batch_finish(GNOSTR_TIMELINE_HYDRATOR(source), result, NULL);
  capture->got_items = items != NULL;
  capture->done = TRUE;
}

static void
test_stale_generation_drops_sync_and_async(void)
{
  GnostrTimelineHydrator *hydrator = gnostr_timeline_hydrator_new(9);
  GnostrTimelineBatch *stale = batch_new(GNOSTR_TIMELINE_BATCH_LIVE_HEAD, 8);
  batch_add(stale, 1, 100, 0x11, "stale", "Stale", "stale", NULL, TRUE);

  g_autoptr(GPtrArray) items = gnostr_timeline_hydrator_hydrate_batch(hydrator, stale);
  g_assert_null(items);

  AsyncHydrateCapture capture = { 0 };
  gnostr_timeline_hydrator_hydrate_batch_async(hydrator,
                                               stale,
                                               NULL,
                                               on_hydrate_done,
                                               &capture);
  gint64 deadline = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
  while (!capture.done && g_get_monotonic_time() < deadline)
    g_main_context_iteration(NULL, TRUE);
  g_assert_true(capture.done);
  g_assert_false(capture.got_items);

  g_object_unref(stale);
  g_object_unref(hydrator);
}

int
main(int argc,
     char **argv)
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/gnostr/timeline-hydrator/sort-dedup-display-batches",
                  test_sorting_and_dedup_for_display_batches);
  g_test_add_func("/gnostr/timeline-hydrator/quote-repost-action-vm-data",
                  test_quote_repost_and_action_vm_data_are_carried);
  g_test_add_func("/gnostr/timeline-hydrator/missing-preview-states",
                  test_missing_preview_states_are_deterministic);
  g_test_add_func("/gnostr/timeline-hydrator/missing-metadata-fallbacks-reservations",
                  test_missing_metadata_fallbacks_and_reservations);
  g_test_add_func("/gnostr/timeline-hydrator/stale-generation-drops",
                  test_stale_generation_drops_sync_and_async);

  return g_test_run();
}
