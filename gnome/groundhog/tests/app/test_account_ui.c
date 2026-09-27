/* Headless check that gh-account-ui.c's keyboard-focus and screen-reader
 * announcement behavior (added for narrow/adaptive + accessibility work)
 * only reacts to a real account-state transition, not to every incidental
 * "changed"/network-monitor notification. It builds real GTK/libadwaita
 * widgets (no compiled GResource or signer needed) but never maps or
 * presents a window, so it proves construction and focus bookkeeping only;
 * a real screen reader announcement still needs a manual GNOME check.
 */
#include "gh-account-ui.h"
#include "gh-identity.h"
#include "gh-shell.h"

typedef struct {
  GMutex lock;
  gboolean empty;
} FakeStore;

static GPtrArray *
fake_list(gpointer data, GError **error)
{
  FakeStore *store = data;
  (void)error;
  g_mutex_lock(&store->lock);
  gboolean empty = store->empty;
  g_mutex_unlock(&store->lock);
  GPtrArray *ids = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
  if (!empty) {
    GhIdentityInfo *info = g_new0(GhIdentityInfo, 1);
    info->npub = g_strdup("npub1test");
    info->label = g_strdup("Test");
    g_ptr_array_add(ids, info);
  }
  return ids;
}

static gboolean
deadline_hit(gpointer data)
{
  *(gboolean *)data = TRUE;
  return G_SOURCE_REMOVE;
}

static void
spin_until_at(gboolean (*pred)(gpointer), gpointer data, int line)
{
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds(5, deadline_hit, &expired);
  while (!pred(data) && !expired)
    g_main_context_iteration(NULL, TRUE);
  if (expired)
    g_error("condition waited for at line %d did not hold within 5s", line);
  g_source_remove(timer);
}
#define spin_until(pred, data) spin_until_at((pred), (data), __LINE__)

static gboolean
is_no_identities(gpointer data)
{
  return gh_account_controller_get_state(data) == GH_ACCOUNT_STATE_NO_IDENTITIES;
}

static gboolean
is_null(gpointer data)
{
  return *(gpointer *)data == NULL;
}

static void
test_focus_and_announce_only_on_transition(void)
{
  FakeStore store = { 0 };
  store.empty = TRUE;
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  GhAccountController *controller =
    gh_account_controller_new_full(settings, NULL, fake_list, &store);

  GtkWidget *header, *title, *stack, *banner;
  AdwNavigationPage *sidebar = gh_shell_sidebar_page(&header, &title, &stack);
  AdwNavigationPage *content = gh_shell_content_page(&banner);
  GtkWidget *toasts = adw_toast_overlay_new();
  GtkWidget *window = adw_application_window_new(NULL);
  /* Real widget hierarchy, as main.c builds it: gtk_widget_grab_focus() only
   * moves a window's focus for a widget rooted under that same window. */
  GtkWidget *split = adw_navigation_split_view_new();
  adw_navigation_split_view_set_sidebar(ADW_NAVIGATION_SPLIT_VIEW(split), sidebar);
  adw_navigation_split_view_set_content(ADW_NAVIGATION_SPLIT_VIEW(split), content);
  adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(toasts), split);
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), toasts);

  gh_account_ui_attach(window, controller, settings, ADW_HEADER_BAR(header),
                       ADW_WINDOW_TITLE(title), GTK_STACK(stack), ADW_BANNER(banner),
                       ADW_TOAST_OVERLAY(toasts));

  spin_until(is_no_identities, controller);
  g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(stack)), ==, "account-none");

  GtkWidget *visible = gtk_stack_get_child_by_name(GTK_STACK(stack), "account-none");
  gpointer action = g_object_get_data(G_OBJECT(visible), "gh-state-action");
  g_assert_nonnull(action);
  /* Reaching an actionable empty state must hand keyboard/screen-reader
   * focus straight to its one button. */
  g_assert_true(gtk_window_get_focus(GTK_WINDOW(window)) == GTK_WIDGET(action));

  /* Simulate the user having since moved focus elsewhere (or nowhere). */
  gtk_window_set_focus(GTK_WINDOW(window), NULL);

  /* A network-monitor blip re-runs update() without changing account state;
   * it must not steal focus back onto the action button. */
  g_object_notify(G_OBJECT(g_network_monitor_get_default()), "network-available");
  g_main_context_iteration(NULL, FALSE);
  g_assert_null(gtk_window_get_focus(GTK_WINDOW(window)));

  /* Destroying the window drops gh-account-ui's own controller reference
   * (see account_ui_free); only after that do we drop ours and wait for
   * finalization, so no worker-thread callback can outlive this test. */
  gtk_window_destroy(GTK_WINDOW(window));
  gpointer weak = controller;
  g_object_add_weak_pointer(G_OBJECT(controller), &weak);
  g_object_unref(controller);
  spin_until(is_null, &weak);
}

int
main(int argc, char **argv)
{
  if (!gtk_init_check()) {
    g_printerr("groundhog-account-ui test skipped: no graphical display\n");
    return 77;
  }
  adw_init();

  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/account-ui/focus-and-announce-only-on-transition",
                  test_focus_and_announce_only_on_transition);
  return g_test_run();
}
