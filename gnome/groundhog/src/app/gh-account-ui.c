#include "gh-account-ui.h"
#include "gh-identity.h"

#include <string.h>

typedef struct {
  GhAccountController *controller;
  GSettings *settings;
  /* Not owned: the window outlives ui, since ui is destroyed as the
   * window's own object data (see gh_account_ui_attach). The widgets below
   * are template children of that window. */
  GtkWidget *window;
  AdwWindowTitle *title;
  GtkStack *stack;
  AdwBanner *banner;
  AdwToastOverlay *toasts;
  GMenu *identities_menu;
  GSimpleAction *select;
  /* The previously visible stack page name, so update() only moves keyboard
   * focus and announces a status change on an actual state transition, not
   * on every redundant "changed"/network-monitor notification. */
  gchar *last_page;
} GhAccountUi;

static void
account_ui_free(gpointer data)
{
  GhAccountUi *ui = data;
  g_clear_object(&ui->controller);
  g_clear_object(&ui->settings);
  g_clear_object(&ui->identities_menu);
  g_clear_object(&ui->select);
  g_free(ui->last_page);
  g_free(ui);
}

static gchar *
identity_title(const GhIdentityInfo *info)
{
  if (info->label && *info->label)
    return g_strdup(info->label);
  gsize len = strlen(info->npub);
  return len > 16 ? g_strdup_printf("%.10s…%s", info->npub, info->npub + len - 6)
                  : g_strdup(info->npub);
}

/* Builder ids in data/ui/gh-account-ui.blp and the sidebar stack name each
 * page is added under; page_for_state() selects by stack name. */
static const struct {
  const gchar *id;
  const gchar *name;
} account_pages[] = {
  { "account_discovering", "account-discovering" },
  { "account_store_unavailable", "account-store-unavailable" },
  { "account_none", "account-none" },
  { "account_unselected", "account-unselected" },
  { "account_missing", "account-missing" },
};

static const gchar *
page_for_state(GhAccountState state)
{
  switch (state) {
  case GH_ACCOUNT_STATE_DISCOVERING: return "account-discovering";
  case GH_ACCOUNT_STATE_STORE_UNAVAILABLE: return "account-store-unavailable";
  case GH_ACCOUNT_STATE_NO_IDENTITIES: return "account-none";
  case GH_ACCOUNT_STATE_UNSELECTED: return "account-unselected";
  case GH_ACCOUNT_STATE_SELECTED_MISSING: return "account-missing";
  case GH_ACCOUNT_STATE_ACTIVE:
  default:
    /* No conversation backend exists yet; never fabricate rows. */
    return "empty";
  }
}

static void
update(GhAccountUi *ui)
{
  GhAccountState state = gh_account_controller_get_state(ui->controller);
  GPtrArray *identities = gh_account_controller_get_identities(ui->controller);
  const gchar *active = gh_account_controller_get_active_npub(ui->controller);
  g_autofree gchar *current = g_settings_get_string(ui->settings, "current-npub");
  g_autofree gchar *method = g_settings_get_string(ui->settings, "signer-method");
  gboolean online = g_network_monitor_get_network_available(g_network_monitor_get_default());
  g_autofree gchar *limits =
    gh_account_describe_limits(state,
                               gh_account_controller_get_signer_availability(ui->controller),
                               method, online);
  g_autofree gchar *subtitle = NULL;
  const gchar *page_name = page_for_state(state);

  gtk_stack_set_visible_child_name(ui->stack, page_name);
  adw_banner_set_title(ui->banner, limits);
  adw_banner_set_revealed(ui->banner, TRUE);

  /* Only react to an actual state transition: a keyboard/screen-reader user
   * mid-interaction should not be interrupted by a redundant re-announce or
   * have focus stolen every time the network monitor merely re-confirms the
   * connection it already reported. */
  if (g_strcmp0(ui->last_page, page_name) != 0) {
    g_free(ui->last_page);
    ui->last_page = g_strdup(page_name);

    if (GTK_IS_ACCESSIBLE(ui->window))
      gtk_accessible_announce(GTK_ACCESSIBLE(ui->window), limits,
                              GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);

    /* Move keyboard/screen-reader focus straight to the page's one
     * actionable control (an AdwStatusPage child, see gh-account-ui.blp)
     * instead of leaving it wherever it was, often the header bar several
     * tabs away. Pages without an action leave focus alone. */
    GtkWidget *visible = gtk_stack_get_child_by_name(ui->stack, page_name);
    GtkWidget *action = ADW_IS_STATUS_PAGE(visible)
                          ? adw_status_page_get_child(ADW_STATUS_PAGE(visible)) : NULL;
    if (action)
      gtk_widget_grab_focus(action);
  }

  g_menu_remove_all(ui->identities_menu);
  for (guint i = 0; identities && i < identities->len; i++) {
    const GhIdentityInfo *info = g_ptr_array_index(identities, i);
    g_autofree gchar *label = identity_title(info);
    g_autoptr(GMenuItem) item = g_menu_item_new(label, NULL);
    g_menu_item_set_action_and_target_value(item, "account.select",
                                            g_variant_new_string(info->npub));
    g_menu_append_item(ui->identities_menu, item);
    if (g_strcmp0(info->npub, active) == 0)
      subtitle = g_steal_pointer(&label);
  }
  g_simple_action_set_state(ui->select, g_variant_new_string(current));
  adw_window_title_set_subtitle(ui->title, subtitle ? subtitle : "No account");
}

/* Connected swapped to the window, so the handlers die with it. */
static void
on_window_state_source(GtkWidget *window)
{
  update(g_object_get_data(G_OBJECT(window), "groundhog-account-ui"));
}

static void
on_select(GSimpleAction *action, GVariant *value, gpointer data)
{
  GhAccountUi *ui = data;
  g_autoptr(GError) error = NULL;
  (void)action;
  if (!gh_account_controller_select(ui->controller, g_variant_get_string(value, NULL),
                                    &error)) {
    adw_toast_overlay_add_toast(ui->toasts, adw_toast_new(error->message));
    update(ui);
  }
}

static void
on_refresh(GSimpleAction *action, GVariant *value, gpointer data)
{
  GhAccountUi *ui = data;
  (void)action;
  (void)value;
  gh_account_controller_refresh(ui->controller);
}

void
gh_account_ui_attach(GhWindow *window, GhAccountController *controller, GSettings *settings)
{
  GhSidebarPage *sidebar = gh_window_get_sidebar(window);
  g_autoptr(GtkBuilder) builder =
    gtk_builder_new_from_resource("/org/nostr/Groundhog/ui/gh-account-ui.ui");
  GhAccountUi *ui = g_new0(GhAccountUi, 1);
  ui->controller = g_object_ref(controller);
  ui->settings = g_object_ref(settings);
  ui->window = GTK_WIDGET(window);
  ui->title = gh_sidebar_page_get_window_title(sidebar);
  ui->stack = gh_sidebar_page_get_stack(sidebar);
  ui->banner = gh_content_page_get_banner(gh_window_get_content(window));
  ui->toasts = gh_window_get_toasts(window);
  g_object_set_data_full(G_OBJECT(window), "groundhog-account-ui", ui, account_ui_free);

  for (guint i = 0; i < G_N_ELEMENTS(account_pages); i++)
    gtk_stack_add_named(ui->stack,
                        GTK_WIDGET(gtk_builder_get_object(builder, account_pages[i].id)),
                        account_pages[i].name);
  adw_header_bar_pack_start(gh_sidebar_page_get_header(sidebar),
                            GTK_WIDGET(gtk_builder_get_object(builder, "account_button")));
  /* The identity rows are the one dynamic part of the menu. */
  ui->identities_menu = G_MENU(g_object_ref(gtk_builder_get_object(builder, "identities")));

  g_autoptr(GSimpleActionGroup) group = g_simple_action_group_new();
  ui->select = g_simple_action_new_stateful("select", G_VARIANT_TYPE_STRING,
                                            g_variant_new_string(""));
  g_signal_connect(ui->select, "change-state", G_CALLBACK(on_select), ui);
  g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(ui->select));
  g_autoptr(GSimpleAction) refresh = g_simple_action_new("refresh", NULL);
  g_signal_connect(refresh, "activate", G_CALLBACK(on_refresh), ui);
  g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(refresh));
  gtk_widget_insert_action_group(GTK_WIDGET(window), "account", G_ACTION_GROUP(group));

  g_signal_connect_object(controller, "changed", G_CALLBACK(on_window_state_source),
                          window, G_CONNECT_SWAPPED);
  g_signal_connect_object(g_network_monitor_get_default(), "notify::network-available",
                          G_CALLBACK(on_window_state_source), window, G_CONNECT_SWAPPED);
  update(ui);
}
