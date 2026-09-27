#include "gh-account-ui.h"
#include "gh-identity.h"

#include <string.h>

typedef struct {
  GhAccountController *controller;
  GSettings *settings;
  AdwWindowTitle *title;
  GtkStack *stack;
  AdwBanner *banner;
  AdwToastOverlay *toasts;
  GMenu *identities_menu;
  GSimpleAction *select;
} GhAccountUi;

static void
account_ui_free(gpointer data)
{
  GhAccountUi *ui = data;
  g_clear_object(&ui->controller);
  g_clear_object(&ui->settings);
  g_clear_object(&ui->identities_menu);
  g_clear_object(&ui->select);
  g_free(ui);
}

static GtkWidget *
state_page(const char *icon, const char *title, const char *description,
           const char *button_label)
{
  GtkWidget *page = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(page), icon);
  adw_status_page_set_title(ADW_STATUS_PAGE(page), title);
  adw_status_page_set_description(ADW_STATUS_PAGE(page), description);
  gtk_widget_add_css_class(page, "groundhog-shell-status");
  if (button_label) {
    GtkWidget *button = gtk_button_new_with_mnemonic(button_label);
    gtk_widget_set_halign(button, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(button, "pill");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(button), "account.refresh");
    adw_status_page_set_child(ADW_STATUS_PAGE(page), button);
  }
  return page;
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

  gtk_stack_set_visible_child_name(ui->stack, page_for_state(state));
  adw_banner_set_title(ui->banner, limits);
  adw_banner_set_revealed(ui->banner, TRUE);

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
gh_account_ui_attach(GtkWidget *window, GhAccountController *controller,
                     GSettings *settings, AdwHeaderBar *sidebar_header,
                     AdwWindowTitle *title, GtkStack *sidebar_stack,
                     AdwBanner *banner, AdwToastOverlay *toasts)
{
  GhAccountUi *ui = g_new0(GhAccountUi, 1);
  ui->controller = g_object_ref(controller);
  ui->settings = g_object_ref(settings);
  ui->title = title;
  ui->stack = sidebar_stack;
  ui->banner = banner;
  ui->toasts = toasts;
  g_object_set_data_full(G_OBJECT(window), "groundhog-account-ui", ui, account_ui_free);

  gtk_stack_add_named(sidebar_stack,
                      state_page("content-loading-symbolic", "Looking for Accounts",
                                 "Reading public identity names from the Nostr signer's "
                                 "store. Private keys are never read.", NULL),
                      "account-discovering");
  gtk_stack_add_named(sidebar_stack,
                      state_page("dialog-warning-symbolic", "Account Store Unavailable",
                                 "Groundhog could not read the signer's identity list. "
                                 "Unlock the keyring or start the Nostr signer, then try "
                                 "again.", "_Try Again"),
                      "account-store-unavailable");
  gtk_stack_add_named(sidebar_stack,
                      state_page("avatar-default-symbolic", "No Nostr Identities",
                                 "Add or import a key with the Nostr signer, then refresh. "
                                 "Groundhog never stores or reads private keys.", "_Refresh"),
                      "account-none");
  gtk_stack_add_named(sidebar_stack,
                      state_page("avatar-default-symbolic", "Choose an Account",
                                 "Pick an identity from the account menu. This choice is "
                                 "Groundhog's own and does not change Gnostr.", NULL),
                      "account-unselected");
  gtk_stack_add_named(sidebar_stack,
                      state_page("dialog-warning-symbolic", "Account Unavailable",
                                 "The selected identity is no longer in the signer's store. "
                                 "Choose another account or refresh.", "_Refresh"),
                      "account-missing");

  ui->identities_menu = g_menu_new();
  g_autoptr(GMenu) menu = g_menu_new();
  g_autoptr(GMenu) other = g_menu_new();
  g_autoptr(GMenuItem) none = g_menu_item_new("No Account (Read-Only)", NULL);
  g_menu_item_set_action_and_target_value(none, "account.select", g_variant_new_string(""));
  g_menu_append_item(other, none);
  g_menu_append(other, "_Refresh Accounts", "account.refresh");
  g_menu_append_section(menu, NULL, G_MENU_MODEL(ui->identities_menu));
  g_menu_append_section(menu, NULL, G_MENU_MODEL(other));
  GtkWidget *button = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button), "avatar-default-symbolic");
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(button), G_MENU_MODEL(menu));
  gtk_widget_set_tooltip_text(button, "Account");
  gtk_accessible_update_property(GTK_ACCESSIBLE(button),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Account", -1);
  adw_header_bar_pack_start(sidebar_header, button);

  g_autoptr(GSimpleActionGroup) group = g_simple_action_group_new();
  ui->select = g_simple_action_new_stateful("select", G_VARIANT_TYPE_STRING,
                                            g_variant_new_string(""));
  g_signal_connect(ui->select, "change-state", G_CALLBACK(on_select), ui);
  g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(ui->select));
  g_autoptr(GSimpleAction) refresh = g_simple_action_new("refresh", NULL);
  g_signal_connect(refresh, "activate", G_CALLBACK(on_refresh), ui);
  g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(refresh));
  gtk_widget_insert_action_group(window, "account", G_ACTION_GROUP(group));

  g_signal_connect_object(controller, "changed", G_CALLBACK(on_window_state_source),
                          window, G_CONNECT_SWAPPED);
  g_signal_connect_object(g_network_monitor_get_default(), "notify::network-available",
                          G_CALLBACK(on_window_state_source), window, G_CONNECT_SWAPPED);
  update(ui);
}
