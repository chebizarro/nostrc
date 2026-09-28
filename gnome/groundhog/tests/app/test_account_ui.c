/* Check that gh-account-ui.c attaches the account pages and menu compiled
 * from data/ui/gh-account-ui.blp to a real GhWindow template, and that its
 * keyboard-focus and screen-reader announcement behavior (added for
 * narrow/adaptive + accessibility work) only reacts to a real account-state
 * transition, not to every incidental "changed"/network-monitor
 * notification. It registers the compiled GResource and uses a fake identity
 * store instead of the signer, but never maps or presents a window, so it
 * proves construction and focus bookkeeping only; a real screen reader
 * announcement still needs a manual GNOME check.
 */
#include "gh-account-ui.h"
#include "gh-identity.h"

void groundhog_register_resource(void);

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
assert_menu_item(GMenuModel *model, int index, const char *label, const char *action,
                 const char *target)
{
  g_autofree char *item_label = NULL;
  g_autofree char *item_action = NULL;
  g_autofree char *item_target = NULL;
  g_assert_true(g_menu_model_get_item_attribute(model, index, G_MENU_ATTRIBUTE_LABEL, "s",
                                                &item_label));
  g_assert_cmpstr(item_label, ==, label);
  g_assert_true(g_menu_model_get_item_attribute(model, index, G_MENU_ATTRIBUTE_ACTION, "s",
                                                &item_action));
  g_assert_cmpstr(item_action, ==, action);
  if (target) {
    g_assert_true(g_menu_model_get_item_attribute(model, index, G_MENU_ATTRIBUTE_TARGET, "s",
                                                  &item_target));
    g_assert_cmpstr(item_target, ==, target);
  } else {
    g_assert_false(g_menu_model_get_item_attribute(model, index, G_MENU_ATTRIBUTE_TARGET, "s",
                                                   &item_target));
  }
}

static GtkMenuButton *
find_menu_button(GtkWidget *widget)
{
  if (GTK_IS_MENU_BUTTON(widget))
    return GTK_MENU_BUTTON(widget);
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    GtkMenuButton *found = find_menu_button(c);
    if (found)
      return found;
  }
  return NULL;
}

/* The account pages and header menu come from gh-account-ui.blp, under the
 * stack names page_for_state() selects. */
static void
assert_account_widgets(GhWindow *window)
{
  static const struct {
    const char *name;
    const char *action_label;
  } pages[] = {
    { "account-discovering", NULL },
    { "account-store-unavailable", "_Try Again" },
    { "account-none", "_Refresh" },
    { "account-unselected", NULL },
    { "account-missing", "_Refresh" },
  };
  GhSidebarPage *sidebar = gh_window_get_sidebar(window);
  GtkStack *stack = gh_sidebar_page_get_stack(sidebar);

  for (guint i = 0; i < G_N_ELEMENTS(pages); i++) {
    GtkWidget *page = gtk_stack_get_child_by_name(stack, pages[i].name);
    g_assert_true(ADW_IS_STATUS_PAGE(page));
    g_assert_true(gtk_widget_has_css_class(page, "groundhog-shell-status"));
    GtkWidget *button = adw_status_page_get_child(ADW_STATUS_PAGE(page));
    if (!pages[i].action_label) {
      g_assert_null(button);
      continue;
    }
    g_assert_true(GTK_IS_BUTTON(button));
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(button)), ==, pages[i].action_label);
    g_assert_true(gtk_button_get_use_underline(GTK_BUTTON(button)));
    g_assert_cmpstr(gtk_actionable_get_action_name(GTK_ACTIONABLE(button)), ==,
                    "account.refresh");
    g_assert_true(gtk_widget_has_css_class(button, "pill"));
  }
  /* Standalone onboarding is only for builds without account support. */
  g_assert_null(gtk_stack_get_child_by_name(stack, "onboarding"));

  /* AdwHeaderBar nests packed children inside its own boxes. */
  GtkMenuButton *button = find_menu_button(GTK_WIDGET(gh_sidebar_page_get_header(sidebar)));
  g_assert_nonnull(button);
  g_assert_cmpstr(gtk_menu_button_get_icon_name(button), ==, "avatar-default-symbolic");
  g_assert_cmpstr(gtk_widget_get_tooltip_text(GTK_WIDGET(button)), ==, "Account");
  gtk_test_accessible_assert_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      "Account");

  GMenuModel *menu = gtk_menu_button_get_menu_model(button);
  g_assert_cmpint(g_menu_model_get_n_items(menu), ==, 2);
  GMenuModel *identities = g_menu_model_get_item_link(menu, 0, G_MENU_LINK_SECTION);
  GMenuModel *other = g_menu_model_get_item_link(menu, 1, G_MENU_LINK_SECTION);
  g_assert_nonnull(identities);
  g_assert_nonnull(other);
  g_assert_cmpint(g_menu_model_get_n_items(other), ==, 2);
  assert_menu_item(other, 0, "No Account (Read-Only)", "account.select", "");
  assert_menu_item(other, 1, "_Refresh Accounts", "account.refresh", NULL);
  g_object_unref(identities);
  g_object_unref(other);
}

static void
test_focus_and_announce_only_on_transition(void)
{
  FakeStore store = { 0 };
  store.empty = TRUE;
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  GhAccountController *controller =
    gh_account_controller_new_full(settings, NULL, fake_list, &store);

  /* The real window template, as main.c creates it: gtk_widget_grab_focus()
   * only moves a window's focus for a widget rooted under that same window. */
  GhWindow *window = gh_window_new(NULL);
  GtkStack *stack = gh_sidebar_page_get_stack(gh_window_get_sidebar(window));

  gh_account_ui_attach(window, controller, settings);
  assert_account_widgets(window);

  spin_until(is_no_identities, controller);
  g_assert_cmpstr(gtk_stack_get_visible_child_name(stack), ==, "account-none");
  g_assert_cmpstr(adw_window_title_get_subtitle(
                    gh_sidebar_page_get_window_title(gh_window_get_sidebar(window))), ==,
                  "No account");

  GtkWidget *visible = gtk_stack_get_child_by_name(stack, "account-none");
  GtkWidget *action = adw_status_page_get_child(ADW_STATUS_PAGE(visible));
  g_assert_true(GTK_IS_BUTTON(action));
  /* Reaching an actionable empty state must hand keyboard/screen-reader
   * focus straight to its one button. */
  g_assert_true(gtk_window_get_focus(GTK_WINDOW(window)) == action);

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
  groundhog_register_resource();

  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/account-ui/focus-and-announce-only-on-transition",
                  test_focus_and_announce_only_on_transition);
  return g_test_run();
}
