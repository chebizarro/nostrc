/**
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * test_media_source.c - Gnostr remote-media policy for the nostr-gtk media
 * widgets (nostrc-8xfib.4; was test_image_viewer_remote_media.c).
 */

#include "nostrc-test-gdk-frame.h"
#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <nostr-gtk-1.0/gn-media-viewer.h>
#include <nostr-gtk-1.0/gn-video-player.h>

#include "services/gnostr-media-source.h"
#include "ui/gnostr-main-window.h"

#define GNOSTR_CLIENT_SCHEMA_ID "org.gnostr.Client"
#define GNOSTR_CLIENT_LOAD_REMOTE_MEDIA_KEY "load-remote-media"

/* App-shell boundary (nostrc-0cfsg.36.23). The media source reports a copied
 * link through gnostr_main_window_show_toast(); its real definition, in
 * src/ui/gnostr-main-window-utilities.c, needs the whole main window, which
 * this test does not build. So the test defines it and records every call,
 * and the link-copied cases assert what the media source asked for. What the
 * real function does with a window that is not a GnostrMainWindow is the
 * main window's own contract, not tested here. */
static guint toast_count;
static GtkWidget *toast_window; /* compared by address only */
static char *toast_message;

void
gnostr_main_window_show_toast(GtkWidget *window, const char *message)
{
  toast_count++;
  toast_window = window;
  g_free(toast_message);
  toast_message = g_strdup(message);
}

static void
reset_toasts(void)
{
  toast_count = 0;
  toast_window = NULL;
  g_clear_pointer(&toast_message, g_free);
}

static GSettings *
open_client_settings(void)
{
  GSettingsSchemaSource *source = g_settings_schema_source_get_default();
  g_autoptr(GSettingsSchema) schema =
    source ? g_settings_schema_source_lookup(source, GNOSTR_CLIENT_SCHEMA_ID, TRUE) : NULL;
  if (!schema || !g_settings_schema_has_key(schema, GNOSTR_CLIENT_LOAD_REMOTE_MEDIA_KEY)) {
    g_test_skip("org.gnostr.Client/load-remote-media schema key unavailable");
    return NULL;
  }
  return g_settings_new(GNOSTR_CLIENT_SCHEMA_ID);
}

static gboolean
set_remote_media_enabled(gboolean enabled)
{
  g_autoptr(GSettings) settings = open_client_settings();
  if (!settings) return FALSE;
  g_assert_true(g_settings_set_boolean(settings, GNOSTR_CLIENT_LOAD_REMOTE_MEDIA_KEY, enabled));
  return TRUE;
}

static void
test_policy_tracks_gsettings(void)
{
  GnMediaSource *source = gnostr_media_source_get();
  const char *url = "https://example.com/test.png";
  if (!set_remote_media_enabled(FALSE)) return;
  /* Disabled: an image waits for Load, a video stays blocked (as before). */
  g_assert_cmpint(gn_media_source_get_policy(source, url, GN_MEDIA_KIND_IMAGE), ==,
                  GN_MEDIA_POLICY_ASK);
  g_assert_cmpint(gn_media_source_get_policy(source, url, GN_MEDIA_KIND_VIDEO), ==,
                  GN_MEDIA_POLICY_BLOCKED);
  if (!set_remote_media_enabled(TRUE)) return;
  g_assert_cmpint(gn_media_source_get_policy(source, url, GN_MEDIA_KIND_IMAGE), ==,
                  GN_MEDIA_POLICY_ALLOW);
  g_assert_cmpint(gn_media_source_get_policy(source, url, GN_MEDIA_KIND_VIDEO), ==,
                  GN_MEDIA_POLICY_ALLOW);
}

static void
test_viewer_shows_load_action_when_remote_media_disabled(void)
{
  if (!set_remote_media_enabled(FALSE)) return;
  GnMediaViewer *viewer = gn_media_viewer_new(NULL);
  const char *urls[] = { "https://example.com/test.png", NULL };
  gn_media_viewer_set_gallery(viewer, urls, 0);
  gn_media_viewer_set_source(viewer, gnostr_media_source_get());
  g_assert_true(gn_media_viewer_get_load_offered(viewer));
  g_assert_false(gn_media_viewer_get_loading(viewer));
  g_assert_cmpstr(gn_media_viewer_get_message(viewer), ==, "Remote media is blocked");
  gtk_window_destroy(GTK_WINDOW(viewer));
}

static void
test_video_blocked_when_remote_media_disabled(void)
{
  if (!set_remote_media_enabled(FALSE)) return;
  GnVideoPlayer *player = gn_video_player_new();
  g_object_ref_sink(player);
  gn_video_player_set_source(player, gnostr_media_source_get());
  gn_video_player_set_url(player, "https://example.com/clip.mp4");
  /* No stream, so the media backend never saw the URL. */
  g_assert_null(gn_video_player_get_stream(player));
  g_object_unref(player);
}

/* Copy link toasts on the window the viewer belongs to, without loading the
 * blocked image. */
static void
test_copied_link_toasts_parent_window(void)
{
  if (!set_remote_media_enabled(FALSE)) return;
  GtkWidget *parent = gtk_window_new();
  GnMediaViewer *viewer = gn_media_viewer_new(GTK_WINDOW(parent));
  const char *urls[] = { "https://example.com/test.png", NULL };
  gn_media_viewer_set_gallery(viewer, urls, 0);
  gn_media_viewer_set_source(viewer, gnostr_media_source_get());
  reset_toasts();

  gn_media_viewer_copy_link(viewer);
  g_assert_cmpuint(toast_count, ==, 1);
  g_assert_true(toast_window == parent);
  g_assert_cmpstr(toast_message, ==, _("Link copied"));
  g_assert_true(gn_media_viewer_get_load_offered(viewer));
  g_assert_false(gn_media_viewer_get_loading(viewer));

  gtk_window_destroy(GTK_WINDOW(viewer));
  gtk_window_destroy(GTK_WINDOW(parent));
  reset_toasts();
}

/* A viewer with no parent window has nowhere to toast. */
static void
test_copied_link_without_parent_window_no_toast(void)
{
  if (!set_remote_media_enabled(FALSE)) return;
  GnMediaViewer *viewer = gn_media_viewer_new(NULL);
  const char *urls[] = { "https://example.com/test.png", NULL };
  gn_media_viewer_set_gallery(viewer, urls, 0);
  gn_media_viewer_set_source(viewer, gnostr_media_source_get());
  reset_toasts();

  gn_media_viewer_copy_link(viewer);
  g_assert_cmpuint(toast_count, ==, 0);
  g_assert_false(gn_media_viewer_get_loading(viewer));

  gtk_window_destroy(GTK_WINDOW(viewer));
}

static void
test_player_settings_bound(void)
{
  g_autoptr(GSettings) settings = open_client_settings();
  if (!settings) return;
  g_settings_set_boolean(settings, "video-autoplay", TRUE);
  g_settings_set_boolean(settings, "video-loop", FALSE);
  GnVideoPlayer *player = gn_video_player_new();
  g_object_ref_sink(player);
  gn_video_player_set_source(player, gnostr_media_source_get());
  g_assert_true(gn_video_player_get_autoplay(player));
  g_assert_false(gn_video_player_get_loop(player));
  g_settings_set_boolean(settings, "video-loop", TRUE);
  g_assert_true(gn_video_player_get_loop(player));
  g_object_unref(player);
}

int
main(int argc, char *argv[])
{
  g_setenv("GSETTINGS_BACKEND", "memory", TRUE);
  gtk_test_init(&argc, &argv, NULL);
  nostrc_test_tolerate_gdk_frame_warning();

  g_test_add_func("/gnostr/media-source/policy-tracks-gsettings", test_policy_tracks_gsettings);
  g_test_add_func("/gnostr/media-source/disabled-viewer-offers-load",
                  test_viewer_shows_load_action_when_remote_media_disabled);
  g_test_add_func("/gnostr/media-source/disabled-video-blocked",
                  test_video_blocked_when_remote_media_disabled);
  g_test_add_func("/gnostr/media-source/player-settings-bound", test_player_settings_bound);
  g_test_add_func("/gnostr/media-source/copied-link-toasts-parent-window",
                  test_copied_link_toasts_parent_window);
  g_test_add_func("/gnostr/media-source/copied-link-without-parent-window-no-toast",
                  test_copied_link_without_parent_window_no_toast);
  return g_test_run();
}
