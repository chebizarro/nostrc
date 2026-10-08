#include "gh-account-ui.h"
#include "gh-identity.h"

#include <string.h>

/* Builder ids in data/ui/gh-account-ui.blp, the sidebar stack name each page
 * is added under (page_for_state() selects by name), and the id of the page's
 * explicit focus target: the one control that receives keyboard and
 * screen-reader focus when the page appears, or NULL for none (charter §7.14,
 * never whatever child a status page happens to have). */
static const struct {
  const gchar *id;
  const gchar *name;
  const gchar *focus_id;
} account_pages[] = {
  { "account_discovering", "account-discovering", NULL },
  { "account_store_unavailable", "account-store-unavailable",
    "account_store_unavailable_retry" },
  { "account_none", "account-none", "account_none_refresh" },
  /* Its action is the account menu, also the main menu's first entry. */
  { "account_unselected", "account-unselected", "account_choose_button" },
  { "account_missing", "account-missing", "account_missing_refresh" },
};

typedef struct {
  GhAccountController *controller;
  GSettings *settings;
  /* Not owned: the window outlives ui, since ui is destroyed as the
   * window's own object data (see gh_account_ui_attach). The widgets below
   * are template children of that window or were added to them. */
  GtkWidget *window;
  GhSidebarPage *sidebar;
  GhStatus *status;
  AdwWindowTitle *title;
  GtkStack *stack;
  AdwToastOverlay *toasts;
  GtkWidget *focus_targets[G_N_ELEMENTS(account_pages)];
  GMenu *identities_menu;
  GSimpleAction *select;
  GhAccountNameFunc name;  /* nullable: the account's own kind-0 name */
  GObject *names;          /* weak: the source behind name */
  /* The previously shown account page ("" for the conversation pages), so
   * update() only moves keyboard focus and announces a status change on an
   * actual state transition, not on every redundant "changed" or
   * network-monitor notification. */
  gchar *last_page;
  /* A transition's focus move runs once the transition has settled
   * (focus_idle), and waits for an inactive window (focus_deferred). */
  guint focus_idle;
  gboolean focus_deferred;
  guint announcements; /* made (only while the window is active, §7.14) */
} GhAccountUi;

static void
account_ui_free(gpointer data)
{
  GhAccountUi *ui = data;
  g_clear_object(&ui->controller);
  g_clear_object(&ui->settings);
  g_clear_object(&ui->identities_menu);
  g_clear_object(&ui->select);
  g_clear_handle_id(&ui->focus_idle, g_source_remove);
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

/* The account page for state; NULL when the account is active and the
 * sidebar shows its conversation pages. */
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
    return NULL;
  }
}

static GtkWidget *
focus_target(GhAccountUi *ui, const gchar *page_name)
{
  if (!page_name)
    return gh_sidebar_page_get_focus_target(ui->sidebar);
  for (guint i = 0; i < G_N_ELEMENTS(account_pages); i++)
    if (g_str_equal(account_pages[i].name, page_name))
      return ui->focus_targets[i];
  return NULL;
}

static GhStatusSigner
status_signer(GhSignerAvailability availability)
{
  switch (availability) {
  /* An activatable signer is started by the bus on the first call. */
  case GH_SIGNER_AVAILABILITY_RUNNING:
  case GH_SIGNER_AVAILABILITY_ACTIVATABLE: return GH_STATUS_SIGNER_AVAILABLE;
  case GH_SIGNER_AVAILABILITY_ABSENT: return GH_STATUS_SIGNER_UNAVAILABLE;
  case GH_SIGNER_AVAILABILITY_NO_BUS: return GH_STATUS_SIGNER_NO_BUS;
  case GH_SIGNER_AVAILABILITY_UNKNOWN:
  default:
    return GH_STATUS_SIGNER_UNKNOWN;
  }
}

/* The user is typing: keyboard focus is in a text field or the composer
 * (nostrc-qp24.8.1). */
static gboolean
editing(GhAccountUi *ui)
{
  GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(ui->window));
  return focus && gtk_widget_get_mapped(focus) &&
         (GTK_IS_EDITABLE(focus) || GTK_IS_TEXT_VIEW(focus));
}

/* Charter §7.14: no announcement to an inactive window; on its activation
 * the deferred focus move puts the page's action, which says the same, in
 * front of the screen reader. */
static void
announce(GhAccountUi *ui, const gchar *text)
{
  if (!text || !GTK_IS_ACCESSIBLE(ui->window) || !gtk_window_is_active(GTK_WINDOW(ui->window)))
    return;
  gtk_accessible_announce(GTK_ACCESSIBLE(ui->window), text,
                          GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
  ui->announcements++;
}

/* Keyboard/screen-reader focus to the shown page's named target, unless
 * someone is typing (in a field or the composer that is still on screen), or
 * the window is not active: then it moves once the window is activated
 * again, if nobody is typing by then. */
static void
apply_focus(GhAccountUi *ui)
{
  ui->focus_deferred = FALSE;
  GtkWidget *target = focus_target(ui, *ui->last_page ? ui->last_page : NULL);
  if (!target || editing(ui))
    return;
  if (!gtk_window_is_active(GTK_WINDOW(ui->window))) {
    ui->focus_deferred = TRUE;
    return;
  }
  gtk_widget_grab_focus(target);
}

static gboolean
focus_idle(gpointer data)
{
  GhAccountUi *ui = data;
  ui->focus_idle = 0;
  apply_focus(ui);
  return G_SOURCE_REMOVE;
}

/* A real transition moves focus to the page's target. A window on screen
 * decides once the transition has settled (a conversation that went with
 * the account has taken its composer off screen by then); one never shown
 * gets it at once, as its initial focus. */
static void
move_focus(GhAccountUi *ui)
{
  g_clear_handle_id(&ui->focus_idle, g_source_remove);
  ui->focus_deferred = FALSE;
  if (!gtk_widget_get_mapped(ui->window)) {
    GtkWidget *target = focus_target(ui, *ui->last_page ? ui->last_page : NULL);
    if (target)
      gtk_widget_grab_focus(target);
    return;
  }
  ui->focus_idle = g_idle_add(focus_idle, ui);
}

static void
update(GhAccountUi *ui)
{
  GhAccountState state = gh_account_controller_get_state(ui->controller);
  GPtrArray *identities = gh_account_controller_get_identities(ui->controller);
  const gchar *active = gh_account_controller_get_active_npub(ui->controller);
  g_autofree gchar *current = g_settings_get_string(ui->settings, "current-npub");
  GhSignerBackend active_backend = gh_account_controller_get_active_backend(ui->controller);
  gboolean online = g_network_monitor_get_network_available(g_network_monitor_get_default());
  GhSignerAvailability availability =
    gh_account_controller_get_signer_availability(ui->controller);
  g_autofree gchar *limits = gh_account_controller_describe_limits(ui->controller, online);
  g_autofree gchar *subtitle = NULL;
  const gchar *page_name = page_for_state(state);

  gh_sidebar_page_set_account_page(ui->sidebar, page_name);
  g_object_freeze_notify(G_OBJECT(ui->status));
  gh_status_set_account_active(ui->status, state == GH_ACCOUNT_STATE_ACTIVE);
  gh_status_set_network_available(ui->status, online);
  GhStatusSigner signer_status = status_signer(availability);
  if (state == GH_ACCOUNT_STATE_ACTIVE && active_backend == GH_SIGNER_BACKEND_NIP46) {
    GhRemoteSignerState remote = gh_account_controller_get_remote_state(ui->controller);
    signer_status = remote == GH_REMOTE_SIGNER_READY ? GH_STATUS_SIGNER_AVAILABLE :
      remote == GH_REMOTE_SIGNER_CONNECTING || remote == GH_REMOTE_SIGNER_LOADING_CREDENTIAL ?
      GH_STATUS_SIGNER_UNKNOWN : GH_STATUS_SIGNER_UNAVAILABLE;
  }
  gh_status_set_signer(ui->status, signer_status);
  g_object_thaw_notify(G_OBJECT(ui->status));

  /* Only react to an actual state transition: a keyboard/screen-reader user
   * mid-interaction should not be interrupted by a redundant re-announce or
   * have focus stolen every time the network monitor merely re-confirms the
   * connection it already reported. */
  if (g_strcmp0(ui->last_page, page_name ? page_name : "") != 0) {
    g_free(ui->last_page);
    ui->last_page = g_strdup(page_name ? page_name : "");

    /* Announced even while someone types: it says why sending stopped. */
    announce(ui, limits);

    /* Move keyboard/screen-reader focus straight to the page's named focus
     * target instead of leaving it wherever it was, often the header bar
     * several tabs away. Pages without one leave focus alone. */
    move_focus(ui);
  }

  g_menu_remove_all(ui->identities_menu);
  for (guint i = 0; identities && i < identities->len; i++) {
    const GhIdentityInfo *info = g_ptr_array_index(identities, i);
    g_autofree gchar *title = identity_title(info);
    g_autofree gchar *label = info->backend == GH_SIGNER_BACKEND_NIP46 ?
      g_strdup_printf("%s — Remote signer", title) : g_strdup_printf("%s — Grotto", title);
    g_autofree gchar *target = info->backend == GH_SIGNER_BACKEND_NIP46 ?
      g_strdup_printf("nip46:%s", info->npub) : g_strdup(info->npub);
    g_autoptr(GMenuItem) item = g_menu_item_new(label, NULL);
    g_menu_item_set_action_and_target_value(item, "account.select",
                                            g_variant_new_string(target));
    g_menu_append_item(ui->identities_menu, item);
    if (info->backend == active_backend && g_strcmp0(info->npub, active) == 0) {
      subtitle = g_strdup(title);
      /* The profile name, when the directory has cached it. */
      g_autofree gchar *pubkey = gh_identity_pubkey_hex(info->npub);
      const gchar *name = ui->name && ui->names && pubkey ? ui->name(ui->names, pubkey) : NULL;
      if (name && *name) {
        g_free(subtitle);
        subtitle = g_strdup(name);
      }
    }
  }
  g_autofree gchar *current_backend = g_settings_get_string(ui->settings, "current-backend");
  g_autofree gchar *selected_target = *current && g_strcmp0(current_backend, "nip46") == 0 ?
    g_strdup_printf("nip46:%s", current) : g_strdup(current);
  g_simple_action_set_state(ui->select, g_variant_new_string(selected_target));
  /* The active account names the sidebar, without a subtitle: the app's
   * name is the window's own, and a title over a subtitle is cut short in a
   * narrow sidebar header (nostrc-qp24.70). No account: "Groundhog". */
  gh_sidebar_page_set_title(ui->sidebar, subtitle);
  adw_window_title_set_subtitle(ui->title, "");
}

/* Connected swapped to the window, so the handlers die with it. */
static void
on_window_state_source(GtkWidget *window)
{
  update(g_object_get_data(G_OBJECT(window), "groundhog-account-ui"));
}

static void
on_window_active(GtkWidget *window)
{
  GhAccountUi *ui = g_object_get_data(G_OBJECT(window), "groundhog-account-ui");
  if (ui->focus_deferred && gtk_window_is_active(GTK_WINDOW(window)))
    apply_focus(ui);
}

static void
on_select(GSimpleAction *action, GVariant *value, gpointer data)
{
  GhAccountUi *ui = data;
  g_autoptr(GError) error = NULL;
  (void)action;
  const gchar *target = g_variant_get_string(value, NULL);
  gboolean remote = g_str_has_prefix(target, "nip46:");
  if (!gh_account_controller_select_backend(ui->controller,
        remote ? GH_SIGNER_BACKEND_NIP46 : GH_SIGNER_BACKEND_GROTTO,
        remote ? target + strlen("nip46:") : target, &error)) {
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
  ui->sidebar = sidebar;
  ui->status = gh_window_get_status(window);
  ui->title = gh_sidebar_page_get_window_title(sidebar);
  ui->stack = gh_sidebar_page_get_stack(sidebar);
  ui->toasts = gh_window_get_toasts(window);
  g_object_set_data_full(G_OBJECT(window), "groundhog-account-ui", ui, account_ui_free);

  for (guint i = 0; i < G_N_ELEMENTS(account_pages); i++) {
    gtk_stack_add_named(ui->stack,
                        GTK_WIDGET(gtk_builder_get_object(builder, account_pages[i].id)),
                        account_pages[i].name);
    if (account_pages[i].focus_id)
      ui->focus_targets[i] = GTK_WIDGET(gtk_builder_get_object(builder,
                                                               account_pages[i].focus_id));
  }
  /* The account menu heads the sidebar's main menu instead of taking a
   * header button of its own: at 360 px and in the narrowest split sidebar
   * the header's title needs that room (nostrc-qp24.70). */
  GtkMenuButton *primary = GTK_MENU_BUTTON(
    gtk_widget_get_template_child(GTK_WIDGET(sidebar), GH_TYPE_SIDEBAR_PAGE, "primary_button"));
  g_menu_prepend_section(G_MENU(gtk_menu_button_get_menu_model(primary)), NULL,
                         G_MENU_MODEL(gtk_builder_get_object(builder, "account_section")));
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
  g_signal_connect_object(window, "notify::is-active", G_CALLBACK(on_window_active), window,
                          G_CONNECT_SWAPPED);
  update(ui);
}

guint
gh_account_ui_get_announcements(GhWindow *window)
{
  g_return_val_if_fail(GH_IS_WINDOW(window), 0);
  GhAccountUi *ui = g_object_get_data(G_OBJECT(window), "groundhog-account-ui");
  return ui ? ui->announcements : 0;
}

static void
on_profile_changed(GObject *directory, const gchar *pubkey, gpointer data)
{
  (void)directory; (void)pubkey;
  update(data);
}

static void
names_gone(gpointer data, GObject *where_the_object_was)
{
  (void)where_the_object_was;
  GhAccountUi *ui = data;
  ui->names = NULL;
}

void
gh_account_ui_set_name_source(GhWindow *window, GhAccountNameFunc name, GObject *source)
{
  GhAccountUi *ui = g_object_get_data(G_OBJECT(window), "groundhog-account-ui");
  g_return_if_fail(ui != NULL);
  if (ui->names)
    g_object_weak_unref(ui->names, names_gone, ui);
  ui->name = name;
  ui->names = source;
  if (ui->names) {
    g_object_weak_ref(ui->names, names_gone, ui);
    g_signal_connect_object(ui->names, "profile-changed", G_CALLBACK(on_profile_changed), ui, 0);
  }
  update(ui);
}
