/**
 * test_standalone_core.c — nostr-gtk core widgets without Gnostr
 *
 * Links only libnostr-gtk and its declared dependencies (no apps/gnostr
 * objects). If nostr_gtk_init() or a core widget regresses into referencing
 * a Gnostr app symbol, this test fails to link. At runtime it verifies that
 * the public entry point registers the library resources and the core
 * template widgets, and that those widgets instantiate and finalize.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nostrc-test-gdk-frame.h"
#include <gtk/gtk.h>
#include <nostr-gtk-1.0/nostr-gtk.h>

static void
assert_resource_present (const char *path)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) bytes =
      g_resources_lookup_data (path, G_RESOURCE_LOOKUP_FLAGS_NONE, &error);

  g_assert_no_error (error);
  g_assert_nonnull (bytes);
  g_assert_cmpuint (g_bytes_get_size (bytes), >, 0);
}

static void
test_init_registers_core (void)
{
  /* Run first: before init, nothing in this process has touched the types. */
  g_assert_cmpuint (g_type_from_name ("NostrGtkComposer"), ==, 0);

  nostr_gtk_init ();
  nostr_gtk_init (); /* idempotent */

  assert_resource_present ("/org/nostr/gtk/ui/gnostr-composer.ui");
  assert_resource_present ("/org/nostr/gtk/ui/widgets/gnostr-timeline-view.ui");

  g_assert_cmpuint (g_type_from_name ("NostrGtkComposer"), ==,
                    NOSTR_GTK_TYPE_COMPOSER);
  g_assert_cmpuint (g_type_from_name ("NostrGtkTimelineTabs"), ==,
                    NOSTR_GTK_TYPE_TIMELINE_TABS);
  g_assert_cmpuint (g_type_from_name ("NostrGtkTimelineView"), ==,
                    NOSTR_GTK_TYPE_TIMELINE_VIEW);
}

static void
test_composer_reply_context (void)
{
  GtkWidget *widget = nostr_gtk_composer_new ();
  NostrGtkComposer *composer;

  g_assert_true (NOSTR_GTK_IS_COMPOSER (widget));
  g_object_ref_sink (widget);
  g_object_add_weak_pointer (G_OBJECT (widget), (gpointer *) &widget);
  composer = NOSTR_GTK_COMPOSER (widget);

  g_assert_false (nostr_gtk_composer_is_reply (composer));
  nostr_gtk_composer_set_reply_context (composer, "reply-id", "root-id",
                                        "reply-pubkey", "Alice");
  g_assert_true (nostr_gtk_composer_is_reply (composer));
  g_assert_cmpstr (nostr_gtk_composer_get_reply_to_id (composer), ==, "reply-id");
  g_assert_cmpstr (nostr_gtk_composer_get_root_id (composer), ==, "root-id");
  g_assert_cmpstr (nostr_gtk_composer_get_reply_to_pubkey (composer), ==,
                   "reply-pubkey");

  nostr_gtk_composer_clear_reply_context (composer);
  g_assert_false (nostr_gtk_composer_is_reply (composer));

  g_object_unref (widget);
  g_assert_null (widget);
}

static void
test_timeline_view_tabs (void)
{
  GtkWidget *widget = nostr_gtk_timeline_view_new ();
  NostrGtkTimelineTabs *tabs;
  guint n_before;
  gint index;

  g_assert_true (NOSTR_GTK_IS_TIMELINE_VIEW (widget));
  g_object_ref_sink (widget);
  g_object_add_weak_pointer (G_OBJECT (widget), (gpointer *) &widget);

  tabs = nostr_gtk_timeline_view_get_tabs (NOSTR_GTK_TIMELINE_VIEW (widget));
  g_assert_true (NOSTR_GTK_IS_TIMELINE_TABS (tabs));
  g_assert_nonnull (
      nostr_gtk_timeline_view_get_list_view (NOSTR_GTK_TIMELINE_VIEW (widget)));

  n_before = nostr_gtk_timeline_tabs_get_n_tabs (tabs);
  nostr_gtk_timeline_view_add_hashtag_tab (NOSTR_GTK_TIMELINE_VIEW (widget),
                                           "nostr");
  g_assert_cmpuint (nostr_gtk_timeline_tabs_get_n_tabs (tabs), ==, n_before + 1);

  index = nostr_gtk_timeline_tabs_find_tab_by_type_and_value (
      tabs, GN_TIMELINE_TAB_HASHTAG, "nostr");
  g_assert_cmpint (index, >=, 0);
  g_assert_cmpuint (nostr_gtk_timeline_tabs_get_selected (tabs), ==, (guint) index);

  g_object_unref (widget);
  g_assert_null (widget);
}

static void
test_card_visibility_policy (void)
{
  const char *followed[] = { "author-followed", NULL };
  g_autoptr(GnostrCardVisibilityPolicy) policy =
      gnostr_card_visibility_policy_new ();

  /* NULL policy degrades to explicit-or-profile visibility. */
  g_assert_true (gnostr_card_visibility_policy_should_show (NULL, "a", FALSE, TRUE));
  g_assert_true (gnostr_card_visibility_policy_should_show (NULL, "a", TRUE, FALSE));
  g_assert_false (gnostr_card_visibility_policy_should_show (NULL, "a", FALSE, FALSE));

  g_assert_true (gnostr_card_visibility_policy_replace_follow_snapshot (
      policy, "viewer", followed));
  g_assert_true (gnostr_card_visibility_policy_should_show (
      policy, "author-followed", FALSE, FALSE));
  g_assert_false (gnostr_card_visibility_policy_should_show (
      policy, "author-other", FALSE, FALSE));
}

int
main (int argc, char *argv[])
{
  gtk_test_init (&argc, &argv, NULL);
  nostrc_test_tolerate_gdk_frame_warning();

  g_test_add_func ("/nostr-gtk/standalone/init-registers-core",
                   test_init_registers_core);
  g_test_add_func ("/nostr-gtk/standalone/composer-reply-context",
                   test_composer_reply_context);
  g_test_add_func ("/nostr-gtk/standalone/timeline-view-tabs",
                   test_timeline_view_tabs);
  g_test_add_func ("/nostr-gtk/standalone/card-visibility-policy",
                   test_card_visibility_policy);

  return g_test_run ();
}
