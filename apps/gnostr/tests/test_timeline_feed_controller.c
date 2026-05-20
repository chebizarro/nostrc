#include <glib.h>
#include <gio/gio.h>
#include <string.h>

#include "../src/ui/gnostr-timeline-feed-controller.h"

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
          guint8 id_byte)
{
  guint8 id[32];
  fill_id(id, id_byte);
  gnostr_timeline_batch_add_note(batch,
                                 note_key,
                                 created_at,
                                 id,
                                 "pubkey",
                                 "note body",
                                 "Display Name",
                                 "handle",
                                 "https://example.test/avatar.png",
                                 "name@example.test",
                                 NULL,
                                 NULL,
                                 1,
                                 TRUE);
}

static void
batch_add_unprofiled(GnostrTimelineBatch *batch,
                     guint64 note_key,
                     gint64 created_at,
                     guint8 id_byte)
{
  guint8 id[32];
  fill_id(id, id_byte);
  gnostr_timeline_batch_add_note(batch,
                                 note_key,
                                 created_at,
                                 id,
                                 "pubkey",
                                 "note body",
                                 NULL,
                                 NULL,
                                 NULL,
                                 NULL,
                                 NULL,
                                 NULL,
                                 1,
                                 FALSE);
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
batch_add_delete_target(GnostrTimelineBatch *batch,
                        guint8 target_id_byte)
{
  g_autofree char *target_id = event_id_hex_for_byte(target_id_byte);
  GnostrTimelineDeleteTarget target = {
    .target_event_id = target_id,
    .delete_event_id = "delete-event",
  };
  gnostr_timeline_batch_add_delete_target(batch, &target);
}

static GnostrTimelineSnapshot *
dup_controller_snapshot(GnostrTimelineFeedController *controller)
{
  return gnostr_timeline_snapshot_model_dup_snapshot(
    gnostr_timeline_feed_controller_get_model(controller));
}

typedef struct {
  guint count;
  guint emissions;
} PendingCapture;

static void
on_pending_count_changed(GnostrTimelineFeedController *controller,
                         guint count,
                         gpointer user_data)
{
  (void)controller;
  PendingCapture *capture = user_data;
  capture->count = count;
  capture->emissions++;
}

typedef struct {
  double value;
  guint emissions;
} RestoreCapture;

static void
on_restore_scroll(GnostrTimelineFeedController *controller,
                  double value,
                  gpointer user_data)
{
  (void)controller;
  RestoreCapture *capture = user_data;
  capture->value = value;
  capture->emissions++;
}

typedef struct {
  guint emissions;
} PublishedCapture;

static void
on_snapshot_published(GnostrTimelineFeedController *controller,
                      GnostrTimelineSnapshot *snapshot,
                      gpointer user_data)
{
  (void)controller;
  (void)snapshot;
  PublishedCapture *capture = user_data;
  capture->emissions++;
}

static void
drain_main_context_for_ms(guint ms)
{
  gint64 deadline = g_get_monotonic_time() + ((gint64)ms * 1000);
  while (g_get_monotonic_time() < deadline) {
    while (g_main_context_iteration(NULL, FALSE)) {}
    g_usleep(1000);
  }
  while (g_main_context_iteration(NULL, FALSE)) {}
}

static void
test_live_head_pending_while_scrolled_down(void)
{
  GnostrTimelineFeedController *controller =
    gnostr_timeline_feed_controller_new(NULL);
  PendingCapture pending = { 0, 0 };
  g_signal_connect(controller, "pending-count-changed",
                   G_CALLBACK(on_pending_count_changed), &pending);

  GnostrTimelineBatch *refresh = batch_new(GNOSTR_TIMELINE_BATCH_REFRESH, 1);
  batch_add(refresh, 1, 100, 0x11);
  batch_add(refresh, 2, 90, 0x22);
  gnostr_timeline_feed_controller_ingest_batch(controller, refresh);
  gnostr_timeline_feed_controller_compose_now(controller);
  g_object_unref(refresh);

  g_autoptr(GnostrTimelineSnapshot) initial = dup_controller_snapshot(controller);
  g_assert_nonnull(initial);
  g_assert_cmpuint(gnostr_timeline_snapshot_get_n_rows(initial), ==, 2);
  GnostrTimelineSnapshotRow *initial_row = gnostr_timeline_snapshot_get_row(initial, 0);
  g_assert_cmpstr(gnostr_timeline_snapshot_row_get_content(initial_row), ==, "note body");
  g_assert_cmpstr(gnostr_timeline_snapshot_row_get_display_name(initial_row), ==, "Display Name");
  g_assert_cmpstr(gnostr_timeline_snapshot_row_get_handle(initial_row), ==, "handle");
  g_assert_cmpstr(gnostr_timeline_snapshot_row_get_avatar_url(initial_row), ==, "https://example.test/avatar.png");
  g_assert_cmpstr(gnostr_timeline_snapshot_row_get_nip05(initial_row), ==, "name@example.test");

  gnostr_timeline_feed_controller_set_viewport(controller, 48.0, 400.0, 480);
  g_assert_false(gnostr_timeline_feed_controller_get_user_at_top(controller));

  GnostrTimelineBatch *live = batch_new(GNOSTR_TIMELINE_BATCH_LIVE_HEAD, 1);
  batch_add(live, 3, 110, 0x33);
  gnostr_timeline_feed_controller_ingest_batch(controller, live);
  g_object_unref(live);

  g_assert_cmpuint(gnostr_timeline_feed_controller_get_pending_count(controller), ==, 1);
  g_assert_cmpuint(pending.count, ==, 1);
  g_assert_cmpuint(pending.emissions, >=, 1);

  g_autoptr(GnostrTimelineSnapshot) hidden = dup_controller_snapshot(controller);
  g_assert_cmpuint(gnostr_timeline_snapshot_get_n_rows(hidden), ==, 2);
  g_assert_cmpuint(gnostr_timeline_snapshot_get_pending_head_count(hidden), ==, 0);

  g_object_unref(controller);
}

static void
test_admit_pending_head_publishes_snapshot(void)
{
  GnostrTimelineFeedController *controller =
    gnostr_timeline_feed_controller_new(NULL);
  PendingCapture pending = { 0, 0 };
  RestoreCapture restore = { 0.0, 0 };
  g_signal_connect(controller, "pending-count-changed",
                   G_CALLBACK(on_pending_count_changed), &pending);
  g_signal_connect(controller, "restore-scroll",
                   G_CALLBACK(on_restore_scroll), &restore);

  GnostrTimelineBatch *refresh = batch_new(GNOSTR_TIMELINE_BATCH_REFRESH, 1);
  batch_add(refresh, 1, 100, 0x11);
  batch_add(refresh, 2, 90, 0x22);
  gnostr_timeline_feed_controller_ingest_batch(controller, refresh);
  gnostr_timeline_feed_controller_compose_now(controller);
  g_object_unref(refresh);

  gnostr_timeline_feed_controller_set_viewport(controller, 64.0, 400.0, 480);

  GnostrTimelineBatch *live = batch_new(GNOSTR_TIMELINE_BATCH_LIVE_HEAD, 1);
  batch_add(live, 3, 110, 0x33);
  gnostr_timeline_feed_controller_ingest_batch(controller, live);
  g_object_unref(live);
  g_assert_cmpuint(gnostr_timeline_feed_controller_get_pending_count(controller), ==, 1);

  gnostr_timeline_feed_controller_admit_pending_head(controller, TRUE);
  gnostr_timeline_feed_controller_compose_now(controller);

  g_assert_cmpuint(gnostr_timeline_feed_controller_get_pending_count(controller), ==, 0);
  g_assert_cmpuint(pending.count, ==, 0);
  g_assert_cmpuint(restore.emissions, >=, 1);
  g_assert_cmpfloat_with_epsilon(restore.value, 0.0, 0.001);

  g_autoptr(GnostrTimelineSnapshot) snapshot = dup_controller_snapshot(controller);
  g_assert_cmpuint(gnostr_timeline_snapshot_get_n_rows(snapshot), ==, 3);
  GnostrTimelineSnapshotRow *first = gnostr_timeline_snapshot_get_row(snapshot, 0);
  g_assert_cmpstr(gnostr_timeline_snapshot_row_get_note_key(first), ==, "3");

  g_object_unref(controller);
}

static void
test_page_older_preserves_viewport_anchor(void)
{
  GnostrTimelineFeedController *controller =
    gnostr_timeline_feed_controller_new(NULL);
  RestoreCapture restore = { 0.0, 0 };
  g_signal_connect(controller, "restore-scroll",
                   G_CALLBACK(on_restore_scroll), &restore);

  GnostrTimelineBatch *refresh = batch_new(GNOSTR_TIMELINE_BATCH_REFRESH, 1);
  batch_add(refresh, 1, 300, 0x11);
  batch_add(refresh, 2, 200, 0x22);
  batch_add(refresh, 3, 100, 0x33);
  gnostr_timeline_feed_controller_ingest_batch(controller, refresh);
  gnostr_timeline_feed_controller_compose_now(controller);
  g_object_unref(refresh);
  restore.value = 0.0;
  restore.emissions = 0;

  /* Default compositor estimate is 160px.  This anchors 10px into row 2. */
  gnostr_timeline_feed_controller_set_viewport(controller, 170.0, 400.0, 480);

  GnostrTimelineBatch *older = batch_new(GNOSTR_TIMELINE_BATCH_PAGE_OLDER, 1);
  batch_add(older, 4, 50, 0x44);
  gnostr_timeline_feed_controller_ingest_batch(controller, older);
  gnostr_timeline_feed_controller_compose_now(controller);
  g_object_unref(older);

  g_assert_cmpuint(restore.emissions, ==, 1);
  g_assert_cmpfloat_with_epsilon(restore.value, 170.0, 0.001);

  g_autoptr(GnostrTimelineSnapshot) snapshot = dup_controller_snapshot(controller);
  g_assert_cmpuint(gnostr_timeline_snapshot_get_n_rows(snapshot), ==, 4);
  GnostrTimelineSnapshotRow *last = gnostr_timeline_snapshot_get_row(snapshot, 3);
  g_assert_cmpstr(gnostr_timeline_snapshot_row_get_note_key(last), ==, "4");

  g_object_unref(controller);
}

static void
test_stale_generation_is_dropped(void)
{
  GnostrTimelineFeedController *controller =
    gnostr_timeline_feed_controller_new(NULL);

  GnostrTimelineBatch *refresh = batch_new(GNOSTR_TIMELINE_BATCH_REFRESH, 1);
  batch_add(refresh, 1, 100, 0x11);
  gnostr_timeline_feed_controller_ingest_batch(controller, refresh);
  gnostr_timeline_feed_controller_compose_now(controller);
  g_object_unref(refresh);

  GnostrTimelineBatch *stale = batch_new(GNOSTR_TIMELINE_BATCH_LIVE_HEAD, 2);
  batch_add(stale, 2, 200, 0x22);
  gnostr_timeline_feed_controller_ingest_batch(controller, stale);
  g_object_unref(stale);

  g_autoptr(GnostrTimelineSnapshot) snapshot = dup_controller_snapshot(controller);
  g_assert_cmpuint(gnostr_timeline_snapshot_get_n_rows(snapshot), ==, 1);
  GnostrTimelineSnapshotRow *row = gnostr_timeline_snapshot_get_row(snapshot, 0);
  g_assert_cmpstr(gnostr_timeline_snapshot_row_get_note_key(row), ==, "1");

  g_object_unref(controller);
}

static void
test_patch_and_delete_batches_do_not_publish_patch_rows(void)
{
  GnostrTimelineFeedController *controller =
    gnostr_timeline_feed_controller_new(NULL);

  GnostrTimelineBatch *refresh = batch_new(GNOSTR_TIMELINE_BATCH_REFRESH, 1);
  batch_add(refresh, 1, 100, 0x11);
  gnostr_timeline_feed_controller_ingest_batch(controller, refresh);
  gnostr_timeline_feed_controller_compose_now(controller);
  g_object_unref(refresh);

  GnostrTimelineBatch *profile = batch_new(GNOSTR_TIMELINE_BATCH_PROFILE_PATCH, 1);
  batch_add(profile, 2, 500, 0x22);
  gnostr_timeline_feed_controller_ingest_batch(controller, profile);
  gnostr_timeline_feed_controller_compose_now(controller);
  g_object_unref(profile);

  GnostrTimelineBatch *metadata = batch_new(GNOSTR_TIMELINE_BATCH_METADATA_PATCH, 1);
  batch_add(metadata, 3, 600, 0x33);
  gnostr_timeline_feed_controller_ingest_batch(controller, metadata);
  gnostr_timeline_feed_controller_compose_now(controller);
  g_object_unref(metadata);

  GnostrTimelineBatch *delete_batch = batch_new(GNOSTR_TIMELINE_BATCH_DELETE, 1);
  batch_add(delete_batch, 4, 700, 0x11);
  gnostr_timeline_feed_controller_ingest_batch(controller, delete_batch);
  gnostr_timeline_feed_controller_compose_now(controller);
  g_object_unref(delete_batch);

  g_autoptr(GnostrTimelineSnapshot) snapshot = dup_controller_snapshot(controller);
  g_assert_cmpuint(gnostr_timeline_snapshot_get_n_rows(snapshot), ==, 1);
  GnostrTimelineSnapshotRow *row = gnostr_timeline_snapshot_get_row(snapshot, 0);
  g_assert_cmpstr(gnostr_timeline_snapshot_row_get_note_key(row), ==, "1");

  g_object_unref(controller);
}

static void
test_delete_target_removes_visible_row_and_preserves_anchor(void)
{
  GnostrTimelineFeedController *controller =
    gnostr_timeline_feed_controller_new(NULL);
  RestoreCapture restore = { 0.0, 0 };
  g_signal_connect(controller, "restore-scroll",
                   G_CALLBACK(on_restore_scroll), &restore);

  GnostrTimelineBatch *refresh = batch_new(GNOSTR_TIMELINE_BATCH_REFRESH, 1);
  batch_add(refresh, 1, 300, 0x11);
  batch_add(refresh, 2, 200, 0x22);
  batch_add(refresh, 3, 100, 0x33);
  gnostr_timeline_feed_controller_ingest_batch(controller, refresh);
  gnostr_timeline_feed_controller_compose_now(controller);
  g_object_unref(refresh);
  restore.value = 0.0;
  restore.emissions = 0;

  /* Anchor 10px into the second row. Delete an older visible row. */
  gnostr_timeline_feed_controller_set_viewport(controller, 170.0, 400.0, 480);

  GnostrTimelineBatch *delete_batch = batch_new(GNOSTR_TIMELINE_BATCH_DELETE, 1);
  batch_add_delete_target(delete_batch, 0x33);
  gnostr_timeline_feed_controller_ingest_batch(controller, delete_batch);
  gnostr_timeline_feed_controller_compose_now(controller);
  g_object_unref(delete_batch);

  g_assert_cmpuint(restore.emissions, ==, 1);
  g_assert_cmpfloat_with_epsilon(restore.value, 170.0, 0.001);

  g_autoptr(GnostrTimelineSnapshot) snapshot = dup_controller_snapshot(controller);
  g_assert_cmpuint(gnostr_timeline_snapshot_get_n_rows(snapshot), ==, 2);
  g_autofree char *deleted_id = event_id_hex_for_byte(0x33);
  guint index = 0;
  g_assert_false(gnostr_timeline_snapshot_lookup_event(snapshot, deleted_id, &index));

  g_object_unref(controller);
}

static void
test_delete_target_removes_pending_head_and_updates_count(void)
{
  GnostrTimelineFeedController *controller =
    gnostr_timeline_feed_controller_new(NULL);
  PendingCapture pending = { 0, 0 };
  g_signal_connect(controller, "pending-count-changed",
                   G_CALLBACK(on_pending_count_changed), &pending);

  GnostrTimelineBatch *refresh = batch_new(GNOSTR_TIMELINE_BATCH_REFRESH, 1);
  batch_add(refresh, 1, 100, 0x11);
  gnostr_timeline_feed_controller_ingest_batch(controller, refresh);
  gnostr_timeline_feed_controller_compose_now(controller);
  g_object_unref(refresh);

  gnostr_timeline_feed_controller_set_viewport(controller, 48.0, 400.0, 480);

  GnostrTimelineBatch *live = batch_new(GNOSTR_TIMELINE_BATCH_LIVE_HEAD, 1);
  batch_add(live, 2, 200, 0x22);
  gnostr_timeline_feed_controller_ingest_batch(controller, live);
  g_object_unref(live);
  g_assert_cmpuint(gnostr_timeline_feed_controller_get_pending_count(controller), ==, 1);

  GnostrTimelineBatch *delete_batch = batch_new(GNOSTR_TIMELINE_BATCH_DELETE, 1);
  batch_add_delete_target(delete_batch, 0x22);
  gnostr_timeline_feed_controller_ingest_batch(controller, delete_batch);
  gnostr_timeline_feed_controller_compose_now(controller);
  g_object_unref(delete_batch);

  g_assert_cmpuint(gnostr_timeline_feed_controller_get_pending_count(controller), ==, 0);
  g_assert_cmpuint(pending.count, ==, 0);

  g_autoptr(GnostrTimelineSnapshot) snapshot = dup_controller_snapshot(controller);
  g_assert_cmpuint(gnostr_timeline_snapshot_get_n_rows(snapshot), ==, 1);
  GnostrTimelineSnapshotRow *row = gnostr_timeline_snapshot_get_row(snapshot, 0);
  g_assert_cmpstr(gnostr_timeline_snapshot_row_get_note_key(row), ==, "1");
  g_autofree char *deleted_id = event_id_hex_for_byte(0x22);
  guint index = 0;
  g_assert_false(gnostr_timeline_snapshot_lookup_event(snapshot, deleted_id, &index));

  g_object_unref(controller);
}

static void
test_metadata_and_profile_patches_replace_rows_without_footprint_change(void)
{
  GnostrTimelineFeedController *controller =
    gnostr_timeline_feed_controller_new(NULL);

  GnostrTimelineBatch *refresh = batch_new(GNOSTR_TIMELINE_BATCH_REFRESH, 1);
  batch_add(refresh, 1, 100, 0x11);
  gnostr_timeline_feed_controller_ingest_batch(controller, refresh);
  gnostr_timeline_feed_controller_compose_now(controller);
  g_object_unref(refresh);

  g_autoptr(GnostrTimelineSnapshot) initial = dup_controller_snapshot(controller);
  GnostrTimelineSnapshotRow *initial_row = gnostr_timeline_snapshot_get_row(initial, 0);
  g_assert_nonnull(initial_row);
  double initial_height = gnostr_timeline_snapshot_row_get_effective_height(initial_row);
  guint64 initial_generation = gnostr_timeline_snapshot_get_generation(initial);

  GnostrTimelineMetadataPatch metadata = {
    .event_id = (char *)gnostr_timeline_snapshot_row_get_event_id(initial_row),
    .has_like_count = TRUE,
    .like_count = 7,
    .has_is_liked = TRUE,
    .is_liked = TRUE,
    .has_repost_count = TRUE,
    .repost_count = 3,
    .has_reply_count = TRUE,
    .reply_count = 2,
    .has_zap_count = TRUE,
    .zap_count = 1,
    .has_zap_total_msat = TRUE,
    .zap_total_msat = 21000,
  };
  GnostrTimelineBatch *metadata_batch = batch_new(GNOSTR_TIMELINE_BATCH_METADATA_PATCH, 1);
  gnostr_timeline_batch_add_metadata_patch(metadata_batch, &metadata);
  gnostr_timeline_feed_controller_ingest_batch(controller, metadata_batch);
  gnostr_timeline_feed_controller_compose_now(controller);
  g_object_unref(metadata_batch);

  GnostrTimelineProfilePatch profile = {
    .pubkey_hex = "pubkey",
    .display_name = "Display Name",
    .handle = "handle",
    .avatar_url = "https://example.test/avatar.png",
    .nip05 = "name@example.test",
  };
  GnostrTimelineBatch *profile_batch = batch_new(GNOSTR_TIMELINE_BATCH_PROFILE_PATCH, 1);
  gnostr_timeline_batch_add_profile_patch(profile_batch, &profile);
  gnostr_timeline_feed_controller_ingest_batch(controller, profile_batch);
  gnostr_timeline_feed_controller_compose_now(controller);
  g_object_unref(profile_batch);

  g_autoptr(GnostrTimelineSnapshot) patched = dup_controller_snapshot(controller);
  g_assert_cmpuint(gnostr_timeline_snapshot_get_n_rows(patched), ==, 1);
  g_assert_cmpuint(gnostr_timeline_snapshot_get_generation(patched), >, initial_generation);

  GnostrTimelineSnapshotRow *patched_row = gnostr_timeline_snapshot_get_row(patched, 0);
  g_assert_cmpstr(gnostr_timeline_snapshot_row_get_event_id(patched_row), ==,
                  gnostr_timeline_snapshot_row_get_event_id(initial_row));
  g_assert_cmpfloat_with_epsilon(gnostr_timeline_snapshot_row_get_effective_height(patched_row),
                                 initial_height,
                                 0.001);
  g_assert_cmpuint(gnostr_timeline_snapshot_row_get_like_count(patched_row), ==, 7);
  g_assert_true(gnostr_timeline_snapshot_row_get_is_liked(patched_row));
  g_assert_cmpuint(gnostr_timeline_snapshot_row_get_repost_count(patched_row), ==, 3);
  g_assert_cmpuint(gnostr_timeline_snapshot_row_get_reply_count(patched_row), ==, 2);
  g_assert_cmpuint(gnostr_timeline_snapshot_row_get_zap_count(patched_row), ==, 1);
  g_assert_cmpint(gnostr_timeline_snapshot_row_get_zap_total_msat(patched_row), ==, 21000);
  g_assert_cmpstr(gnostr_timeline_snapshot_row_get_display_name(patched_row), ==, "Display Name");
  g_assert_cmpstr(gnostr_timeline_snapshot_row_get_handle(patched_row), ==, "handle");
  g_assert_cmpstr(gnostr_timeline_snapshot_row_get_avatar_url(patched_row), ==, "https://example.test/avatar.png");
  g_assert_cmpstr(gnostr_timeline_snapshot_row_get_nip05(patched_row), ==, "name@example.test");

  g_object_unref(controller);
}

static void
test_unprofiled_rows_wait_for_profile_hydration(void)
{
  GnostrTimelineFeedController *controller =
    gnostr_timeline_feed_controller_new(NULL);

  GnostrTimelineBatch *refresh = batch_new(GNOSTR_TIMELINE_BATCH_REFRESH, 1);
  batch_add_unprofiled(refresh, 1, 100, 0x11);
  gnostr_timeline_feed_controller_ingest_batch(controller, refresh);
  gnostr_timeline_feed_controller_compose_now(controller);
  g_object_unref(refresh);

  g_autoptr(GnostrTimelineSnapshot) hidden = dup_controller_snapshot(controller);
  g_assert_nonnull(hidden);
  g_assert_cmpuint(gnostr_timeline_snapshot_get_n_rows(hidden), ==, 0);

  GnostrTimelineProfilePatch profile = {
    .pubkey_hex = "pubkey",
    .display_name = "Display Name",
    .handle = "handle",
    .avatar_url = "https://example.test/avatar.png",
    .nip05 = "name@example.test",
  };
  GnostrTimelineBatch *profile_batch = batch_new(GNOSTR_TIMELINE_BATCH_PROFILE_PATCH, 1);
  gnostr_timeline_batch_add_profile_patch(profile_batch, &profile);
  gnostr_timeline_feed_controller_ingest_batch(controller, profile_batch);
  gnostr_timeline_feed_controller_compose_now(controller);
  g_object_unref(profile_batch);

  g_autoptr(GnostrTimelineSnapshot) revealed = dup_controller_snapshot(controller);
  g_assert_cmpuint(gnostr_timeline_snapshot_get_n_rows(revealed), ==, 1);
  GnostrTimelineSnapshotRow *row = gnostr_timeline_snapshot_get_row(revealed, 0);
  g_assert_cmpstr(gnostr_timeline_snapshot_row_get_display_name(row), ==, "Display Name");
  g_assert_cmpstr(gnostr_timeline_snapshot_row_get_avatar_url(row), ==, "https://example.test/avatar.png");

  g_object_unref(controller);
}

static void
test_geometry_measurement_recomposes_with_cached_height(void)
{
  GnostrTimelineFeedController *controller =
    gnostr_timeline_feed_controller_new(NULL);

  GnostrTimelineBatch *refresh = batch_new(GNOSTR_TIMELINE_BATCH_REFRESH, 1);
  batch_add(refresh, 1, 100, 0x11);
  gnostr_timeline_feed_controller_ingest_batch(controller, refresh);
  gnostr_timeline_feed_controller_compose_now(controller);
  g_object_unref(refresh);

  PublishedCapture published = { 0 };
  g_signal_connect(controller, "snapshot-published",
                   G_CALLBACK(on_snapshot_published), &published);

  g_autoptr(GnostrTimelineSnapshot) initial = dup_controller_snapshot(controller);
  GnostrTimelineSnapshotRow *initial_row = gnostr_timeline_snapshot_get_row(initial, 0);
  g_autofree char *token =
    gnostr_timeline_feed_controller_dup_geometry_token_for_row(initial_row);

  gnostr_timeline_feed_controller_record_geometry(controller,
                                                  token,
                                                  gnostr_timeline_snapshot_get_generation(initial),
                                                  480,
                                                  222);
  gnostr_timeline_feed_controller_compose_now(controller);

  g_autoptr(GnostrTimelineSnapshot) measured = dup_controller_snapshot(controller);
  GnostrTimelineSnapshotRow *measured_row = gnostr_timeline_snapshot_get_row(measured, 0);
  g_assert_true(gnostr_timeline_snapshot_row_get_geometry_measured(measured_row));
  g_assert_cmpfloat_with_epsilon(gnostr_timeline_snapshot_row_get_effective_height(measured_row), 222.0, 0.001);
  g_assert_cmpuint(published.emissions, ==, 1);

  g_autofree char *measured_token =
    gnostr_timeline_feed_controller_dup_geometry_token_for_row(measured_row);
  gnostr_timeline_feed_controller_record_geometry(controller,
                                                  measured_token,
                                                  gnostr_timeline_snapshot_get_generation(measured),
                                                  480,
                                                  222);
  drain_main_context_for_ms(80);
  g_assert_cmpuint(published.emissions, ==, 1);

  g_object_unref(controller);
}

int
main(int argc,
     char **argv)
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/gnostr/timeline-feed-controller/live-head-pending",
                  test_live_head_pending_while_scrolled_down);
  g_test_add_func("/gnostr/timeline-feed-controller/admit-pending-head",
                  test_admit_pending_head_publishes_snapshot);
  g_test_add_func("/gnostr/timeline-feed-controller/page-older-anchor",
                  test_page_older_preserves_viewport_anchor);
  g_test_add_func("/gnostr/timeline-feed-controller/stale-generation",
                  test_stale_generation_is_dropped);
  g_test_add_func("/gnostr/timeline-feed-controller/patch-delete-no-visible-rows",
                  test_patch_and_delete_batches_do_not_publish_patch_rows);
  g_test_add_func("/gnostr/timeline-feed-controller/delete-target-removes-visible-row",
                  test_delete_target_removes_visible_row_and_preserves_anchor);
  g_test_add_func("/gnostr/timeline-feed-controller/delete-target-removes-pending-head",
                  test_delete_target_removes_pending_head_and_updates_count);
  g_test_add_func("/gnostr/timeline-feed-controller/metadata-profile-patches",
                  test_metadata_and_profile_patches_replace_rows_without_footprint_change);
  g_test_add_func("/gnostr/timeline-feed-controller/unprofiled-rows-wait-for-hydration",
                  test_unprofiled_rows_wait_for_profile_hydration);
  g_test_add_func("/gnostr/timeline-feed-controller/geometry-measurement",
                  test_geometry_measurement_recomposes_with_cached_height);

  return g_test_run();
}
