/**
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * test_image_viewer_remote_media.c - Privacy gate coverage for image viewer fetches.
 */

#include <glib.h>
#include <gio/gio.h>
#include <gtk/gtk.h>

#include "ui/gnostr-image-viewer.h"

#define GNOSTR_CLIENT_SCHEMA_ID "org.gnostr.Client"
#define GNOSTR_CLIENT_LOAD_REMOTE_MEDIA_KEY "load-remote-media"

static gboolean
ensure_client_settings_schema_available(void)
{
  GSettingsSchemaSource *source = g_settings_schema_source_get_default();
  if (!source) {
    g_test_skip("GSettings schema source unavailable");
    return FALSE;
  }

  g_autoptr(GSettingsSchema) schema =
    g_settings_schema_source_lookup(source, GNOSTR_CLIENT_SCHEMA_ID, TRUE);
  if (!schema ||
      !g_settings_schema_has_key(schema, GNOSTR_CLIENT_LOAD_REMOTE_MEDIA_KEY)) {
    g_test_skip("org.gnostr.Client/load-remote-media schema key unavailable");
    return FALSE;
  }

  return TRUE;
}

static GSettings *
open_client_settings(void)
{
  if (!ensure_client_settings_schema_available())
    return NULL;
  return g_settings_new(GNOSTR_CLIENT_SCHEMA_ID);
}

static gboolean
set_remote_media_enabled(gboolean enabled)
{
  g_autoptr(GSettings) settings = open_client_settings();
  if (!settings)
    return FALSE;

  g_assert_true(g_settings_set_boolean(settings,
                                       GNOSTR_CLIENT_LOAD_REMOTE_MEDIA_KEY,
                                       enabled));
  return TRUE;
}

static void
test_remote_media_setting_helper_tracks_gsettings(void)
{
  if (!set_remote_media_enabled(FALSE))
    return;
  g_assert_false(gnostr_image_viewer_remote_media_allowed_for_testing());

  if (!set_remote_media_enabled(TRUE))
  if (!set_remote_media_enabled(TRUE))
    return;
  g_assert_true(gnostr_image_viewer_remote_media_allowed_for_testing());
}

static void
test_viewer_shows_load_action_when_remote_media_disabled(void)
{
  if (!set_remote_media_enabled(FALSE))
    return;

  GnostrImageViewer *viewer = gnostr_image_viewer_new(NULL);
  g_assert_nonnull(viewer);

  gnostr_image_viewer_set_image_url(viewer, "https://example.com/test.png");

  g_assert_true(gnostr_image_viewer_is_load_action_visible_for_testing(viewer));

  g_object_unref(viewer);
}

static void
test_enabled_mode_leaves_auto_load_enabled(void)
{
  set_remote_media_enabled(TRUE);
  g_assert_true(gnostr_image_viewer_remote_media_allowed_for_testing());

  GnostrImageViewer *viewer = gnostr_image_viewer_new(NULL);
  g_assert_nonnull(viewer);

  gnostr_image_viewer_set_image_url(viewer, "https://example.com/test.png");
  g_assert_false(gnostr_image_viewer_is_load_action_visible_for_testing(viewer));

  g_object_unref(viewer);
}

static void
test_explicit_load_action_hides_blocked_overlay(void)
{
  if (!set_remote_media_enabled(FALSE))
    return;

  GnostrImageViewer *viewer = gnostr_image_viewer_new(NULL);
  g_assert_nonnull(viewer);

  gnostr_image_viewer_set_image_url(viewer, "https://example.com/test.png");
  g_assert_true(gnostr_image_viewer_is_load_action_visible_for_testing(viewer));

  gnostr_image_viewer_activate_load_action_for_testing(viewer);
  g_assert_false(gnostr_image_viewer_is_load_action_visible_for_testing(viewer));

  g_object_unref(viewer);
}

int
main(int argc, char *argv[])
{
  g_setenv("GSETTINGS_BACKEND", "memory", TRUE);
  g_setenv("GNOSTR_IMAGE_VIEWER_TEST_SKIP_FETCH", "1", TRUE);
  gtk_test_init(&argc, &argv, NULL);

  g_test_add_func("/gnostr/image-viewer/remote-media/helper-tracks-gsettings",
                  test_remote_media_setting_helper_tracks_gsettings);
  g_test_add_func("/gnostr/image-viewer/remote-media/disabled-shows-load-action",
                  test_viewer_shows_load_action_when_remote_media_disabled);
  g_test_add_func("/gnostr/image-viewer/remote-media/enabled-allows-auto-load",
                  test_enabled_mode_leaves_auto_load_enabled);
  g_test_add_func("/gnostr/image-viewer/remote-media/explicit-load-hides-overlay",
                  test_explicit_load_action_hides_blocked_overlay);

  return g_test_run();
}
