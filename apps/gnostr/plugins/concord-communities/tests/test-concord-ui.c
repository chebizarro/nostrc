#include "../gn-concord-community-service.h"
#include "../ui/gn-concord-communities-panel.h"
#include "../ui/gn-concord-composer.h"
#include "../ui/gn-concord-invite-dialog.h"

#include <gtk/gtk.h>

static gboolean gtk_available;

static void test_composer_text_roundtrip(void) {
  if (!gtk_available) {
    g_test_skip("GTK display is unavailable");
    return;
  }
  GtkWidget *widget = GTK_WIDGET(gn_concord_composer_new());
  g_object_ref_sink(widget);
  GnConcordComposer *composer = GN_CONCORD_COMPOSER(widget);
  gn_concord_composer_set_text(composer, "hello\nworld");
  g_autofree gchar *text = gn_concord_composer_get_text(composer);
  g_assert_cmpstr(text, ==, "hello\nworld");
  gn_concord_composer_clear(composer);
  g_clear_pointer(&text, g_free);
  text = gn_concord_composer_get_text(composer);
  g_assert_cmpstr(text, ==, "");
  gn_concord_composer_set_send_sensitive(composer, FALSE);
  g_object_unref(widget);
}

static void test_empty_panel_constructs_and_disposes(void) {
  if (!gtk_available) {
    g_test_skip("GTK display is unavailable");
    return;
  }
  g_autoptr(GnConcordCommunityService) service =
    gn_concord_community_service_new_offline(NULL);
  GtkWidget *panel = GTK_WIDGET(gn_concord_communities_panel_new(service));
  g_assert_true(GN_IS_CONCORD_COMMUNITIES_PANEL(panel));
  g_object_ref_sink(panel);
  g_object_unref(panel);
}

/* A Community with no live link is Private, and the window says so rather
 * than showing an empty list. Minting is also gated on CREATE_INVITE, which
 * an offline service with no folded Roster grants nobody. */
static void test_invite_dialog_constructs_empty(void) {
  if (!gtk_available) {
    g_test_skip("GTK display is unavailable");
    return;
  }
  g_autoptr(GnConcordCommunityService) service =
    gn_concord_community_service_new_offline(NULL);
  GnConcordInviteDialog *dialog = gn_concord_invite_dialog_new(
    service,
    "1111111111111111111111111111111111111111111111111111111111111111");
  g_assert_true(GN_IS_CONCORD_INVITE_DIALOG(dialog));
  g_object_ref_sink(dialog);

  g_autoptr(GPtrArray) links = gn_concord_community_service_get_invites(
    service,
    "1111111111111111111111111111111111111111111111111111111111111111");
  g_assert_cmpuint(links->len, ==, 0);

  gtk_window_destroy(GTK_WINDOW(dialog));
  g_object_unref(dialog);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  gtk_available = gtk_init_check();
  g_test_add_func("/concord/ui/composer-text", test_composer_text_roundtrip);
  g_test_add_func("/concord/ui/empty-panel",
                  test_empty_panel_constructs_and_disposes);
  g_test_add_func("/concord/ui/invite-dialog",
                  test_invite_dialog_constructs_empty);
  return g_test_run();
}
