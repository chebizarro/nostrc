/* Check that gh-account-ui.c attaches the account pages and menu compiled
 * from data/ui/gh-account-ui.blp to a real GhWindow template, and that its
 * keyboard-focus and screen-reader announcement behavior (added for
 * narrow/adaptive + accessibility work) only reacts to a real account-state
 * transition, not to every incidental "changed"/network-monitor
 * notification. It registers the compiled GResource and uses a fake identity
 * store instead of the signer. The focus test never maps a window, so it
 * proves construction and focus bookkeeping only; a real screen reader
 * announcement still needs a manual GNOME check. The header test presents
 * windows at 360, 620 and 960 px to check that the sidebar title is not
 * ellipsized (nostrc-qp24.70).
 */
#include "gh-account-ui.h"
#include "gh-identity.h"
#include "gh-nip46-pair-dialog.h"
#include "../ui/gh-test-dialog.h"
#if GROUNDHOG_HAVE_INBOX
#include "gh-conversation-list.h"
#include <nostr/nip19/nip19.h>
#include <stdlib.h>
#include <string.h>
#endif

#include "nostrc-test-gdk-frame.h"

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
is_unselected(gpointer data)
{
  return gh_account_controller_get_state(data) == GH_ACCOUNT_STATE_UNSELECTED;
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

static GtkWidget *
find_action(GtkWidget *widget, const char *name)
{
  if (GTK_IS_ACTIONABLE(widget) &&
      g_strcmp0(gtk_actionable_get_action_name(GTK_ACTIONABLE(widget)), name) == 0)
    return widget;
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    GtkWidget *found = find_action(c, name);
    if (found)
      return found;
  }
  return NULL;
}

static GhNip46PairDialog *
find_pair_dialog(GtkWidget *widget)
{
  if (GH_IS_NIP46_PAIR_DIALOG(widget))
    return GH_NIP46_PAIR_DIALOG(widget);
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    GhNip46PairDialog *found = find_pair_dialog(c);
    if (found)
      return found;
  }
  return NULL;
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
    { "account-unselected", "_Choose Account…" },
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
    if (GTK_IS_BOX(button)) {
      /* No identities / store unavailable: Add Remote Signer… first, then
       * the page's own action (nostrc-p15n5.3). */
      GtkWidget *remote = gtk_widget_get_first_child(button);
      g_assert_true(GTK_IS_BUTTON(remote));
      g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(remote)), ==, "Add _Remote Signer…");
      g_assert_cmpstr(gtk_actionable_get_action_name(GTK_ACTIONABLE(remote)), ==,
                      "account.add-remote");
      button = gtk_widget_get_next_sibling(remote);
    }
    g_assert_true(GTK_IS_BUTTON(button));
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(button)), ==, pages[i].action_label);
    g_assert_true(gtk_button_get_use_underline(GTK_BUTTON(button)));
    if (g_strcmp0(pages[i].name, "account-unselected") != 0)
      g_assert_cmpstr(gtk_actionable_get_action_name(GTK_ACTIONABLE(button)), ==,
                      "account.refresh");
    g_assert_true(gtk_widget_has_css_class(button, "pill"));
  }
  /* Standalone onboarding is only for builds without account support. */
  g_assert_null(gtk_stack_get_child_by_name(stack, "onboarding"));

  /* nostrc-qp24.70: no account button in the sidebar header (its title
   * needs the room); the account menu is the main menu's first entry. The
   * header's only menu button is the main menu. */
  GtkMenuButton *button = find_menu_button(GTK_WIDGET(gh_sidebar_page_get_header(sidebar)));
  g_assert_nonnull(button);
  g_assert_true((GObject *)button == gtk_widget_get_template_child(GTK_WIDGET(sidebar),
                                                                    GH_TYPE_SIDEBAR_PAGE,
                                                                    "primary_button"));
  GMenuModel *primary = gtk_menu_button_get_menu_model(button);
  /* Account, then relay groups (G20b), Preferences and Shortcuts, Quit. */
  g_assert_cmpint(g_menu_model_get_n_items(primary), ==, 4);
  g_autoptr(GMenuModel) groups_section = g_menu_model_get_item_link(primary, 1,
                                                                    G_MENU_LINK_SECTION);
  g_assert_nonnull(groups_section);
  g_autofree char *join_action = NULL;
  g_assert_true(g_menu_model_get_item_attribute(groups_section, 0, G_MENU_ATTRIBUTE_ACTION, "s",
                                                &join_action));
  g_assert_cmpstr(join_action, ==, "win.join-group");
  g_autoptr(GMenuModel) account_section = g_menu_model_get_item_link(primary, 0,
                                                                     G_MENU_LINK_SECTION);
  g_assert_nonnull(account_section);
  g_assert_cmpint(g_menu_model_get_n_items(account_section), ==, 1);
  g_autofree char *account_label = NULL;
  g_assert_true(g_menu_model_get_item_attribute(account_section, 0, G_MENU_ATTRIBUTE_LABEL, "s",
                                                &account_label));
  g_assert_cmpstr(account_label, ==, "_Account");
  assert_menu_item(account_section, 0, "_Account", "account.open", NULL);
  g_assert_null(g_menu_model_get_item_link(account_section, 0, G_MENU_LINK_SUBMENU));
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
  /* No account: the template's title, no subtitle (nostrc-qp24.70). */
  AdwWindowTitle *title = gh_sidebar_page_get_window_title(gh_window_get_sidebar(window));
  g_assert_cmpstr(adw_window_title_get_title(title), ==, "Groundhog");
  g_assert_cmpstr(adw_window_title_get_subtitle(title), ==, "");

  GtkWidget *visible = gtk_stack_get_child_by_name(stack, "account-none");
  GtkWidget *action = find_action(visible, "account.refresh");
  g_assert_true(GTK_IS_BUTTON(action));
  /* Without identities, and when the store is unavailable, the page offers a
   * remote signer (nostrc-p15n5.3). */
  GtkWidget *remote = find_action(visible, "account.add-remote");
  g_assert_true(GTK_IS_BUTTON(remote));
  g_assert_true(GTK_IS_BUTTON(find_action(
    gtk_stack_get_child_by_name(stack, "account-store-unavailable"), "account.add-remote")));
  /* Reaching an actionable empty state must hand keyboard/screen-reader
   * focus straight to its named focus target, its Refresh button. */
  g_assert_true(gtk_window_get_focus(GTK_WINDOW(window)) == action);
  /* The window's status has no account, so no sidebar banner competes with
   * the page; the reason sending is unavailable waits for a conversation. */
  g_assert_false(gh_status_get_banner(gh_window_get_status(window)) != GH_STATUS_BANNER_NONE);
  /* The transitions are announced ("Read-only: ...") to an active window
   * only; this one was never shown. */
  guint announced = gh_account_ui_get_announcements(window);
  g_assert_cmpuint(announced, ==, 0);

  /* Simulate the user having since moved focus elsewhere (or nowhere). */
  gtk_window_set_focus(GTK_WINDOW(window), NULL);

  /* A network-monitor blip re-runs update() without changing account state;
   * it must not steal focus back onto the action button. */
  g_object_notify(G_OBJECT(g_network_monitor_get_default()), "network-available");
  g_main_context_iteration(NULL, FALSE);
  g_assert_null(gtk_window_get_focus(GTK_WINDOW(window)));
  g_assert_cmpuint(gh_account_ui_get_announcements(window), ==, announced);

  /* An identity appears but none is chosen: the focus target is named
   * explicitly as the page's Choose Account button (charter §7.14,
   * qp24.8.6). */
  g_mutex_lock(&store.lock);
  store.empty = FALSE;
  g_mutex_unlock(&store.lock);
  gh_account_controller_refresh(controller);
  spin_until(is_unselected, controller);
  g_assert_cmpstr(gtk_stack_get_visible_child_name(stack), ==, "account-unselected");
  GtkWidget *unselected = gtk_stack_get_child_by_name(stack, "account-unselected");
  GtkWidget *account_button = adw_status_page_get_child(ADW_STATUS_PAGE(unselected));
  g_assert_true(GTK_IS_BUTTON(account_button));
  GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(window));
  g_assert_nonnull(focus);
  g_assert_true(focus == GTK_WIDGET(account_button) ||
                gtk_widget_is_ancestor(focus, GTK_WIDGET(account_button)));
  g_assert_cmpuint(gh_account_ui_get_announcements(window), ==, announced);

  /* In the main window, the page's remote signer button opens the pair
   * dialog. */
  g_assert_null(find_pair_dialog(GTK_WIDGET(window)));
  g_signal_emit_by_name(remote, "clicked");
  GhNip46PairDialog *pair = find_pair_dialog(GTK_WIDGET(window));
  g_assert_nonnull(pair);
  adw_dialog_close(ADW_DIALOG(pair));

  /* Destroying the window drops gh-account-ui's own controller reference
   * (see account_ui_free); only after that do we drop ours and wait for
   * finalization, so no worker-thread callback can outlive this test. */
  gtk_window_destroy(GTK_WINDOW(window));
  gpointer weak = controller;
  g_object_add_weak_pointer(G_OBJECT(controller), &weak);
  g_object_unref(controller);
  spin_until(is_null, &weak);
}

/* ---- nostrc-qp24.70: the sidebar header's title ------------------------------- */

static GtkLabel *
find_label(GtkWidget *widget, const char *text)
{
  if (GTK_IS_LABEL(widget) && g_strcmp0(gtk_label_get_text(GTK_LABEL(widget)), text) == 0)
    return GTK_LABEL(widget);
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    GtkLabel *found = find_label(c, text);
    if (found)
      return found;
  }
  return NULL;
}

typedef struct {
  GtkWidget *window;
  int width;
} Sized;

/* Laid out at the width asked for: the window's border box, which is what
 * the default size sizes. Its content can be narrower: without a compositor
 * (X11 under Xvfb, the CI job) GTK draws the window's frame (.solid-csd, a
 * 5 px border and padding) inside it, where elsewhere it is a shadow outside
 * (nostrc-qp24.88). */
static gboolean
is_laid_out(gpointer data)
{
  Sized *sized = data;
  graphene_rect_t bounds;
  return gtk_widget_get_mapped(sized->window) &&
         gtk_widget_compute_bounds(sized->window, sized->window, &bounds) &&
         (int)bounds.size.width == sized->width;
}

static gboolean
is_active_account(gpointer data)
{
  return gh_account_controller_get_state(data) == GH_ACCOUNT_STATE_ACTIVE;
}

/* The account menu lives in the main menu, so the header holds only search,
 * New Message, the main menu and the window controls, and the active
 * account's name is the title, without the app's name over it. The title is
 * shown whole at the 360 px minimum (collapsed) and in the narrowest split
 * sidebar (280 px) under GNOME's close-only controls, and at 960 px under
 * three window buttons, as in the bead's screenshots (on macOS GTK always
 * uses its native buttons). Message Requests mode and back keep it. */
static void
test_header_title_fits(void)
{
  static const struct {
    int width;
    const char *layout;
  } sizes[] = {
    { 360, "appmenu:close" },
    { 620, "appmenu:close" },
    { 960, "close,minimize,maximize:" },
  };
  /* The account icon of the account pages and Preferences resolves from the
   * application's resource path (GtkApplication adds it; added here by
   * hand) even where the icon theme has none, e.g. without Adwaita's. */
  GtkIconTheme *theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
  gtk_icon_theme_add_resource_path(theme, "/org/nostr/Groundhog/icons");
  g_assert_true(gtk_icon_theme_has_icon(theme, "avatar-default-symbolic"));

  FakeStore store = { 0 };
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_string(settings, "current-npub", "npub1test");
  GhAccountController *controller =
    gh_account_controller_new_full(settings, NULL, fake_list, &store);
  spin_until(is_active_account, controller);

  for (guint i = 0; i < G_N_ELEMENTS(sizes); i++) {
    g_object_set(gtk_settings_get_default(), "gtk-decoration-layout", sizes[i].layout, NULL);
    GhWindow *window = gh_window_new(NULL);
    gh_account_ui_attach(window, controller, settings);
    GhSidebarPage *sidebar = gh_window_get_sidebar(window);
    AdwWindowTitle *title = gh_sidebar_page_get_window_title(sidebar);
    g_assert_cmpstr(adw_window_title_get_title(title), ==, "Test");
    g_assert_cmpstr(adw_window_title_get_subtitle(title), ==, "");
    /* Message Requests (with none, it returns at once) gives it back. */
    gh_sidebar_page_set_show_requests(sidebar, TRUE);
    g_assert_cmpstr(adw_window_title_get_title(title), ==, "Test");
    gtk_window_set_default_size(GTK_WINDOW(window), sizes[i].width, 400);
    gtk_window_present(GTK_WINDOW(window));
    Sized sized = { GTK_WIDGET(window), sizes[i].width };
    gpointer open_switcher = NULL;
    spin_until(is_laid_out, &sized);
    while (g_main_context_iteration(NULL, FALSE))
      ;
    g_assert_true(gtk_widget_get_mapped(GTK_WIDGET(sidebar)));
    GtkLabel *label = find_label(GTK_WIDGET(title), "Test");
    g_assert_nonnull(label);
    g_assert_true(gtk_widget_get_mapped(GTK_WIDGET(label)));
    if (pango_layout_is_ellipsized(gtk_label_get_layout(label)))
      g_error("the sidebar title is ellipsized at %d px (%s): %d px wide in a %d px sidebar",
              sizes[i].width, sizes[i].layout, gtk_widget_get_width(GTK_WIDGET(label)),
              gtk_widget_get_width(GTK_WIDGET(sidebar)));
    if (i == 0) {
      g_assert_true(gtk_widget_activate_action(GTK_WIDGET(window), "account.open", NULL));
      while (g_main_context_iteration(NULL, FALSE))
        ;
      GtkMenuButton *primary = find_menu_button(GTK_WIDGET(gh_sidebar_page_get_header(sidebar)));
      GtkPopover *switcher = NULL;
      for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(primary)); child;
           child = gtk_widget_get_next_sibling(child))
        if (GTK_IS_POPOVER(child) && !GTK_IS_POPOVER_MENU(child))
          switcher = GTK_POPOVER(child);
      g_assert_nonnull(switcher);
      g_assert_true(gtk_widget_get_visible(GTK_WIDGET(switcher)));
      g_assert_nonnull(find_label(GTK_WIDGET(switcher), "Grotto"));
      GtkWidget *add = find_action(GTK_WIDGET(switcher), "account.add-remote");
      g_assert_nonnull(add);
      /* A closed switcher is finalized, not kept unparented and unrealized:
       * GTK 4.14's tooltip hover timeout would then query the pointer on its
       * destroyed surface (gdk_surface_get_device_position critical). */
      gpointer gone = switcher;
      g_object_add_weak_pointer(G_OBJECT(switcher), &gone);
      g_signal_emit_by_name(add, "clicked");
      spin_until(is_null, &gone);
      GhNip46PairDialog *pair = find_pair_dialog(GTK_WIDGET(window));
      g_assert_nonnull(pair);
      spin_until(gh_test_dialog_shown, pair);
      adw_dialog_close(ADW_DIALOG(pair));
      /* It opens again, with the same accounts. */
      g_assert_true(gtk_widget_activate_action(GTK_WIDGET(window), "account.open", NULL));
      while (g_main_context_iteration(NULL, FALSE))
        ;
      switcher = NULL;
      for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(primary)); child;
           child = gtk_widget_get_next_sibling(child))
        if (GTK_IS_POPOVER(child) && !GTK_IS_POPOVER_MENU(child))
          switcher = GTK_POPOVER(child);
      g_assert_nonnull(switcher);
      g_assert_true(gtk_widget_get_visible(GTK_WIDGET(switcher)));
      g_assert_nonnull(find_label(GTK_WIDGET(switcher), "Grotto"));
      /* It holds the window's focus. Popped down and, within the same frame,
       * unparented with the window: GTK 4.14 keeps a reference to a focused
       * widget hidden and then removed before the next frame unless the
       * focus leaves it first (gh-unparent.h). The leaked switcher was the
       * unrealized popover the tooltip's hover timeout fired on in CI. */
      GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(window));
      g_assert_true(focus && gtk_widget_is_ancestor(focus, GTK_WIDGET(switcher)));
      open_switcher = switcher;
      g_object_add_weak_pointer(G_OBJECT(switcher), &open_switcher);
      gtk_popover_popdown(switcher);
    }
    gtk_window_destroy(GTK_WINDOW(window));
    g_assert_null(open_switcher);
  }
  g_object_set(gtk_settings_get_default(), "gtk-decoration-layout", "appmenu:close", NULL);
  g_settings_reset(settings, "current-npub");
  gpointer weak = controller;
  g_object_add_weak_pointer(G_OBJECT(controller), &weak);
  g_object_unref(controller);
  spin_until(is_null, &weak);
}

#if GROUNDHOG_HAVE_INBOX
static GStrv
picture_consent_list(gpointer data, GError **error)
{
  (void)error;
  GStrv keys = g_new0(gchar *, 2);
  keys[0] = g_strdup(data);
  return keys;
}

static gboolean
picture_consent_set(gpointer data, const gchar *pubkey, gint64 at, GError **error)
{
  (void)data; (void)pubkey; (void)at; (void)error;
  return TRUE;
}

static gboolean
picture_consent_clear(gpointer data, GError **error)
{
  (void)data; (void)error;
  return TRUE;
}

static void
picture_changed(GhPictureCache *cache, const gchar *pubkey, gpointer data)
{
  (void)cache;
  g_ptr_array_add(data, g_strdup(pubkey));
}

static void
test_picture_consent_switch_notifies(void)
{
  static const GhPictureConsentBackend backend = {
    picture_consent_list, picture_consent_set, picture_consent_clear
  };
  const gchar *a = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  const gchar *b = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  g_autoptr(GhPictureCache) cache = gh_picture_cache_new(NULL);
  g_autoptr(GPtrArray) changed = g_ptr_array_new_with_free_func(g_free);
  g_signal_connect(cache, "picture-changed", G_CALLBACK(picture_changed), changed);
  gh_picture_cache_set_consent(cache, &backend, (gpointer)a);
  g_assert_true(g_ptr_array_find_with_equal_func(changed, a, g_str_equal, NULL));
  g_ptr_array_set_size(changed, 0);
  gh_picture_cache_set_consent(cache, &backend, (gpointer)b);
  g_assert_true(g_ptr_array_find_with_equal_func(changed, a, g_str_equal, NULL));
  g_assert_true(g_ptr_array_find_with_equal_func(changed, b, g_str_equal, NULL));
  g_ptr_array_set_size(changed, 0);
  gh_picture_cache_set_consent(cache, &backend, (gpointer)a);
  g_assert_true(gh_picture_cache_is_allowed(cache, a));
  g_assert_false(gh_picture_cache_is_allowed(cache, b));
  g_assert_true(g_ptr_array_find_with_equal_func(changed, a, g_str_equal, NULL));
}

static gchar *
npub_for_byte(guint8 value)
{
  guint8 pubkey[32];
  memset(pubkey, value, sizeof pubkey);
  char *encoded = NULL;
  g_assert_cmpint(nostr_nip19_encode_npub(pubkey, &encoded), ==, 0);
  gchar *npub = g_strdup(encoded);
  free(encoded);
  return npub;
}

static GPtrArray *
one_identity(gpointer data, GError **error)
{
  (void)error;
  GPtrArray *ids = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
  GhIdentityInfo *info = g_new0(GhIdentityInfo, 1);
  info->npub = g_strdup(data);
  g_ptr_array_add(ids, info);
  return ids;
}

static void
test_all_own_pictures_without_consent(void)
{
  g_autofree gchar *local = npub_for_byte(1);
  g_autofree gchar *remote = npub_for_byte(2);
  g_autofree gchar *local_key = gh_identity_pubkey_hex(local);
  g_autofree gchar *remote_key = gh_identity_pubkey_hex(remote);
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_string(settings, "current-npub", local);
  g_settings_set_string(settings, "current-backend", "grotto");
  GhAccountController *controller = gh_account_controller_new_full_with_remote_list(
    settings, NULL, one_identity, local, one_identity, remote);
  spin_until(is_active_account, controller);
  GPtrArray *identities = gh_account_controller_get_identities(controller);
  g_assert_cmpuint(identities->len, ==, 2);
  g_autoptr(GhConversationStore) conversations = gh_conversation_store_new();
  gh_conversation_store_set_account(conversations, local_key, NULL, NULL, NULL);
  for (guint enabled = 0; enabled < 2; enabled++) {
    g_settings_set_boolean(settings, "load-profile-pictures", enabled);
    GhWindow *window = gh_window_new(NULL);
    gh_account_ui_attach(window, controller, settings);
    gh_conversation_list_attach(window, conversations, settings);
    /* The app binds the directory after attaching the conversation list. */
    gh_account_ui_set_name_source(window, NULL, NULL);
    GhPictureCache *cache = gh_conversation_list_get_picture_cache(window);
    g_assert_nonnull(cache);
    g_assert_cmpint(gh_picture_cache_is_allowed(cache, local_key), ==, enabled);
    g_assert_cmpint(gh_picture_cache_is_allowed(cache, remote_key), ==, enabled);
    gtk_window_destroy(GTK_WINDOW(window));
  }
  g_object_unref(controller);
}
#endif

int
main(int argc, char **argv)
{
  if (!gtk_init_check()) {
    g_printerr("groundhog-account-ui test skipped: no graphical display\n");
    return 77;
  }
  adw_init();
  groundhog_register_resource();
  /* The application's stylesheet, as main.c loads it, and as in the shell
   * tests: 1sp = 1px, no animations. */
  g_autoptr(GtkCssProvider) css = gtk_css_provider_new();
  gtk_css_provider_load_from_resource(css, "/org/nostr/Groundhog/style.css");
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_set(gtk_settings_get_default(), "gtk-xft-dpi", 96 * 1024,
               "gtk-enable-animations", FALSE, NULL);

  g_test_init(&argc, &argv, NULL);
  nostrc_test_tolerate_gdk_frame_warning();
  g_test_add_func("/groundhog/account-ui/focus-and-announce-only-on-transition",
                  test_focus_and_announce_only_on_transition);
  g_test_add_func("/groundhog/account-ui/header-title-fits", test_header_title_fits);
#if GROUNDHOG_HAVE_INBOX
  g_test_add_func("/groundhog/account-ui/all-own-pictures-without-consent",
                  test_all_own_pictures_without_consent);
  g_test_add_func("/groundhog/account-ui/picture-consent-switch-notifies",
                  test_picture_consent_switch_notifies);
#endif
  return g_test_run();
}
