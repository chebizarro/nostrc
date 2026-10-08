#include "gh-account-ui.h"
#include "gh-identity.h"
#include "gh-display-name.h"
#include "gh-nip46-pair-dialog.h"
#if GROUNDHOG_HAVE_INBOX
#include "gh-conversation-list.h"
#endif

#include <glib/gi18n.h>

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
  GtkMenuButton *primary;
  GtkWidget *choose_button;
  GtkPopover *account_popover;
  GtkListBox *accounts_list;
  GtkWidget *open_anchor;
  guint open_idle;
#if GROUNDHOG_HAVE_INBOX
  GhPictureCache *picture_cache; /* borrowed from the window */
#endif
  GSimpleAction *select;
  GSimpleAction *remove_remote;
  GhNip46CredentialStore *credentials;
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

static void names_gone(gpointer data, GObject *where_the_object_was);
static void on_window_state_source(GtkWidget *window);

static void
account_ui_free(gpointer data)
{
  GhAccountUi *ui = data;
  if (ui->names) {
    g_signal_handlers_disconnect_by_data(ui->names, ui);
    g_object_weak_unref(ui->names, names_gone, ui);
  }
  g_clear_object(&ui->controller);
  g_clear_object(&ui->settings);
  g_clear_handle_id(&ui->open_idle, g_source_remove);
  if (ui->account_popover) {
    GtkWidget *popover = GTK_WIDGET(ui->account_popover);
    if (gtk_widget_get_parent(popover))
      gtk_widget_unparent(popover);
    g_clear_object(&ui->account_popover);
  }
  g_clear_object(&ui->select);
  g_clear_object(&ui->remove_remote);
  g_clear_object(&ui->credentials);
  g_clear_handle_id(&ui->focus_idle, g_source_remove);
  g_free(ui->last_page);
  g_free(ui);
}

static gchar *
identity_title(const GhIdentityInfo *info)
{
  if (info->backend != GH_SIGNER_BACKEND_NIP46 && info->label && *info->label)
    return g_strdup(info->label);
  gsize len = strlen(info->npub);
  return len > 16 ? g_strdup_printf("%.10s…%s", info->npub, info->npub + len - 6)
                  : g_strdup(info->npub);
}

static gchar *
account_display_name(GhAccountUi *ui, const GhIdentityInfo *info, const gchar *pubkey)
{
  const gchar *name = ui->name && ui->names && pubkey ? ui->name(ui->names, pubkey) : NULL;
  if (name && *name)
    return g_strdup(name);
  if (pubkey)
    return gh_display_name_for(pubkey);
  gsize len = strlen(info->npub);
  return len > 16 ? g_strdup_printf("%.10s…%s", info->npub, info->npub + len - 6)
                  : g_strdup(info->npub);
}

static GtkWidget *
account_row_new(GhAccountUi *ui, const GhIdentityInfo *info, gboolean active)
{
  g_autofree gchar *pubkey = info ? gh_identity_pubkey_hex(info->npub) : NULL;
  g_autofree gchar *title = info ? account_display_name(ui, info, pubkey)
                                   : g_strdup(_("No Account (Read-Only)"));
  const gchar *backend = info ? (info->backend == GH_SIGNER_BACKEND_NIP46
    ? _("Remote signer") : _("Grotto")) : _("Read-only");
  g_autofree gchar *target = info ? (info->backend == GH_SIGNER_BACKEND_NIP46
    ? g_strdup_printf("nip46:%s", info->npub) : g_strdup(info->npub)) : g_strdup("");
  GtkWidget *row = gtk_list_box_row_new();
  g_object_set_data_full(G_OBJECT(row), "account-target", g_steal_pointer(&target), g_free);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top(box, 6);
  gtk_widget_set_margin_bottom(box, 6);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  AdwAvatar *avatar = ADW_AVATAR(adw_avatar_new(36, title, TRUE));
#if GROUNDHOG_HAVE_INBOX
  if (pubkey && ui->picture_cache) {
    if (g_settings_get_boolean(ui->settings, "load-profile-pictures") &&
        !gh_picture_cache_is_allowed(ui->picture_cache, pubkey))
      gh_picture_cache_allow(ui->picture_cache, pubkey);
    GdkTexture *picture = gh_conversation_list_get_picture(GH_WINDOW(ui->window), pubkey);
    if (picture)
      adw_avatar_set_custom_image(avatar, GDK_PAINTABLE(picture));
  }
#endif
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(avatar));
  GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand(labels, TRUE);
  GtkWidget *name = gtk_label_new(title);
  gtk_label_set_xalign(GTK_LABEL(name), 0);
  gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(labels), name);
  GtkWidget *subtitle = gtk_label_new(backend);
  gtk_label_set_xalign(GTK_LABEL(subtitle), 0);
  gtk_widget_add_css_class(subtitle, "dim-label");
  gtk_box_append(GTK_BOX(labels), subtitle);
  gtk_box_append(GTK_BOX(box), labels);
  GtkWidget *check = gtk_image_new_from_icon_name("object-select-symbolic");
  gtk_widget_set_visible(check, active);
  gtk_box_append(GTK_BOX(box), check);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  g_autofree gchar *accessible = g_strdup_printf("%s, %s", title, backend);
  gtk_accessible_update_property(GTK_ACCESSIBLE(row), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 accessible, -1);
  gtk_accessible_update_state(GTK_ACCESSIBLE(row), GTK_ACCESSIBLE_STATE_SELECTED,
                              active, -1);
  return row;
}

static void
rebuild_accounts(GhAccountUi *ui, GPtrArray *identities, const gchar *active,
                 GhSignerBackend active_backend)
{
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(ui->accounts_list))))
    gtk_list_box_remove(ui->accounts_list, child);
  GtkListBoxRow *selected = NULL;
  for (guint i = 0; identities && i < identities->len; i++) {
    const GhIdentityInfo *info = g_ptr_array_index(identities, i);
    gboolean is_active = info->backend == active_backend &&
                         g_strcmp0(info->npub, active) == 0;
    GtkWidget *row = account_row_new(ui, info, is_active);
    gtk_list_box_append(ui->accounts_list, row);
    if (is_active)
      selected = GTK_LIST_BOX_ROW(row);
  }
  GtkWidget *readonly = account_row_new(ui, NULL, !active || !*active);
  gtk_list_box_append(ui->accounts_list, readonly);
  if (!selected)
    selected = GTK_LIST_BOX_ROW(readonly);
  gtk_list_box_select_row(ui->accounts_list, selected);
}

static void
account_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
  GhAccountUi *ui = data;
  (void)list;
  const gchar *target = g_object_get_data(G_OBJECT(row), "account-target");
  gtk_popover_popdown(ui->account_popover);
  g_action_change_state(G_ACTION(ui->select), g_variant_new_string(target));
}

static GtkWidget *
account_action_button(const gchar *label, const gchar *action)
{
  GtkWidget *button = gtk_button_new_with_mnemonic(label);
  gtk_actionable_set_action_name(GTK_ACTIONABLE(button), action);
  gtk_widget_add_css_class(button, "flat");
  gtk_widget_set_halign(button, GTK_ALIGN_FILL);
  return button;
}

static void
account_popover_closed(GtkPopover *popover, gpointer data)
{
  (void)data;
  if (gtk_widget_get_parent(GTK_WIDGET(popover)))
    gtk_widget_unparent(GTK_WIDGET(popover));
}

static void
account_anchor_unmapped(GtkWidget *anchor, gpointer data)
{
  GhAccountUi *ui = data;
  if (gtk_widget_get_parent(GTK_WIDGET(ui->account_popover)) == anchor) {
    gtk_popover_popdown(ui->account_popover);
    if (gtk_widget_get_parent(GTK_WIDGET(ui->account_popover)) == anchor)
      gtk_widget_unparent(GTK_WIDGET(ui->account_popover));
  }
}

static void
build_account_popover(GhAccountUi *ui)
{
  ui->account_popover = GTK_POPOVER(g_object_ref_sink(gtk_popover_new()));
  gtk_popover_set_has_arrow(ui->account_popover, TRUE);
  g_signal_connect(ui->account_popover, "closed", G_CALLBACK(account_popover_closed), ui);
  g_signal_connect(ui->primary, "unmap", G_CALLBACK(account_anchor_unmapped), ui);
  g_signal_connect(ui->primary, "destroy", G_CALLBACK(account_anchor_unmapped), ui);
  g_signal_connect(ui->choose_button, "unmap", G_CALLBACK(account_anchor_unmapped), ui);
  g_signal_connect(ui->choose_button, "destroy", G_CALLBACK(account_anchor_unmapped), ui);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_size_request(box, 270, -1);
  gtk_widget_set_margin_top(box, 8);
  gtk_widget_set_margin_bottom(box, 8);
  GtkWidget *heading = gtk_label_new(_("Account"));
  gtk_widget_add_css_class(heading, "heading");
  gtk_widget_set_halign(heading, GTK_ALIGN_START);
  gtk_widget_set_margin_start(heading, 12);
  gtk_box_append(GTK_BOX(box), heading);
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 330);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
  ui->accounts_list = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(ui->accounts_list, GTK_SELECTION_SINGLE);
  gtk_widget_add_css_class(GTK_WIDGET(ui->accounts_list), "boxed-list");
  g_signal_connect(ui->accounts_list, "row-activated", G_CALLBACK(account_row_activated), ui);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(ui->accounts_list));
  gtk_box_append(GTK_BOX(box), scroll);
  gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
  GtkWidget *add = account_action_button(_("Add Remote Signer…"), "account.add-remote");
  GtkWidget *remove = account_action_button(_("Remove Remote Signer…"), "account.remove-remote");
  GtkWidget *refresh = account_action_button(_("_Refresh Accounts"), "account.refresh");
  gtk_box_append(GTK_BOX(box), add);
  gtk_box_append(GTK_BOX(box), remove);
  gtk_box_append(GTK_BOX(box), refresh);
  g_signal_connect_swapped(add, "clicked", G_CALLBACK(gtk_popover_popdown), ui->account_popover);
  g_signal_connect_swapped(remove, "clicked", G_CALLBACK(gtk_popover_popdown), ui->account_popover);
  g_signal_connect_swapped(refresh, "clicked", G_CALLBACK(gtk_popover_popdown), ui->account_popover);
  gtk_popover_set_child(ui->account_popover, box);
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

static gboolean
open_account_popover(gpointer data)
{
  GhAccountUi *ui = data;
  ui->open_idle = 0;
  GtkWidget *popover = GTK_WIDGET(ui->account_popover);
  if (gtk_widget_get_parent(popover) != ui->open_anchor) {
    gtk_widget_unparent(popover);
    gtk_widget_set_parent(popover, ui->open_anchor);
  }
  gtk_popover_popup(ui->account_popover);
  return G_SOURCE_REMOVE;
}

static void
queue_account_popover(GhAccountUi *ui, GtkWidget *anchor)
{
  g_clear_handle_id(&ui->open_idle, g_source_remove);
  gtk_popover_popdown(ui->account_popover);
  ui->open_anchor = anchor;
  ui->open_idle = g_idle_add(open_account_popover, ui);
}

static void
on_open_account(GSimpleAction *action, GVariant *value, gpointer data)
{
  GhAccountUi *ui = data;
  (void)action; (void)value;
  gtk_menu_button_popdown(ui->primary);
  queue_account_popover(ui, GTK_WIDGET(ui->primary));
}

static void
on_choose_account(GtkButton *button, gpointer data)
{
  GhAccountUi *ui = data;
  queue_account_popover(ui, GTK_WIDGET(button));
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

#if GROUNDHOG_HAVE_INBOX
  GhPictureCache *cache = gh_conversation_list_get_picture_cache(GH_WINDOW(ui->window));
  if (cache && cache != ui->picture_cache) {
    ui->picture_cache = cache;
    g_signal_connect_object(cache, "picture-changed", G_CALLBACK(on_window_state_source),
                            ui->window, G_CONNECT_SWAPPED);
  }
#endif
  rebuild_accounts(ui, identities, active, active_backend);
  for (guint i = 0; identities && i < identities->len; i++) {
    const GhIdentityInfo *info = g_ptr_array_index(identities, i);
    if (info->backend == active_backend && g_strcmp0(info->npub, active) == 0) {
      g_autofree gchar *pubkey = gh_identity_pubkey_hex(info->npub);
      const gchar *name = ui->name && ui->names && pubkey ? ui->name(ui->names, pubkey) : NULL;
      subtitle = name && *name ? g_strdup(name) : identity_title(info);
      break;
    }
  }
  g_autofree gchar *current_backend = g_settings_get_string(ui->settings, "current-backend");
  g_autofree gchar *selected_target = *current && g_strcmp0(current_backend, "nip46") == 0 ?
    g_strdup_printf("nip46:%s", current) : g_strdup(current);
  g_simple_action_set_state(ui->select, g_variant_new_string(selected_target));
  g_simple_action_set_enabled(ui->remove_remote,
    *current && g_strcmp0(current_backend, "nip46") == 0);
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

static void
on_add_remote(GSimpleAction *action, GVariant *value, gpointer data)
{
  (void)action; (void)value;
  GhAccountUi *ui = data;
  GhNip46PairConfig config = { .accounts = ui->controller,
    .settings = ui->settings, .credentials = ui->credentials,
    .display_name = ui->name, .name_source = ui->names };
  GhNip46PairDialog *dialog = gh_nip46_pair_dialog_new(&config);
  adw_dialog_present(ADW_DIALOG(dialog), ui->window);
}

static void
remove_done(GObject *source, GAsyncResult *result, gpointer data)
{
  GhWindow *window = data;
  GhAccountUi *ui = g_object_get_data(G_OBJECT(window), "groundhog-account-ui");
  g_autoptr(GError) error = NULL;
  gboolean removed = gh_nip46_credential_store_delete_finish(
    GH_NIP46_CREDENTIAL_STORE(source), result, &error);
  if (ui) {
    if (removed) {
      gh_account_controller_refresh(ui->controller);
      adw_toast_overlay_add_toast(ui->toasts,
        adw_toast_new(_("Remote signer removed. Stored messages were kept.")));
    } else {
      adw_toast_overlay_add_toast(ui->toasts, adw_toast_new(error ? error->message :
        _("Could not remove the remote signer.")));
    }
  }
  g_object_unref(window);
}

static void
on_remove_response(AdwAlertDialog *alert, const gchar *response, gpointer data)
{
  (void)alert;
  if (!g_str_equal(response, "remove")) return;
  GhWindow *window = data;
  GhAccountUi *ui = g_object_get_data(G_OBJECT(window), "groundhog-account-ui");
  if (!ui) return;
  g_autofree gchar *backend = g_settings_get_string(ui->settings, "current-backend");
  g_autofree gchar *npub = g_settings_get_string(ui->settings, "current-npub");
  if (!g_str_equal(backend, "nip46") || !*npub) return;
  g_autofree gchar *pubkey = gh_identity_pubkey_hex(npub);
  g_autoptr(GError) error = NULL;
  if (!pubkey || !gh_account_controller_select_backend(ui->controller,
      GH_SIGNER_BACKEND_GROTTO, "", &error)) {
    adw_toast_overlay_add_toast(ui->toasts, adw_toast_new(error ? error->message :
      _("Could not deselect the remote signer.")));
    return;
  }
  gh_nip46_credential_store_delete_async(ui->credentials, pubkey, TRUE, NULL,
                                          remove_done, g_object_ref(window));
}

static void
on_remove_remote(GSimpleAction *action, GVariant *value, gpointer data)
{
  (void)action; (void)value;
  GhAccountUi *ui = data;
  AdwDialog *dialog = adw_alert_dialog_new(_("Remove Remote Signer?"),
    _("Groundhog will forget this signer's connection. Your encrypted messages and their storage key will remain. You may also need to revoke Groundhog in the signer app."));
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", _("Cancel"),
                                 "remove", _("Remove Remote Signer"), NULL);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "remove",
                                           ADW_RESPONSE_DESTRUCTIVE);
  g_signal_connect_object(dialog, "response", G_CALLBACK(on_remove_response), ui->window, 0);
  adw_dialog_present(dialog, ui->window);
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
  ui->credentials = gh_nip46_credential_store_new();
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
  ui->primary = GTK_MENU_BUTTON(
    gtk_widget_get_template_child(GTK_WIDGET(sidebar), GH_TYPE_SIDEBAR_PAGE, "primary_button"));
  g_menu_prepend_section(G_MENU(gtk_menu_button_get_menu_model(ui->primary)), NULL,
                         G_MENU_MODEL(gtk_builder_get_object(builder, "account_section")));
  ui->choose_button = GTK_WIDGET(gtk_builder_get_object(builder, "account_choose_button"));
  g_signal_connect(ui->choose_button, "clicked", G_CALLBACK(on_choose_account), ui);
  build_account_popover(ui);

  g_autoptr(GSimpleActionGroup) group = g_simple_action_group_new();
  ui->select = g_simple_action_new_stateful("select", G_VARIANT_TYPE_STRING,
                                            g_variant_new_string(""));
  g_signal_connect(ui->select, "change-state", G_CALLBACK(on_select), ui);
  g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(ui->select));
  g_autoptr(GSimpleAction) open = g_simple_action_new("open", NULL);
  g_signal_connect(open, "activate", G_CALLBACK(on_open_account), ui);
  g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(open));
  g_autoptr(GSimpleAction) refresh = g_simple_action_new("refresh", NULL);
  g_signal_connect(refresh, "activate", G_CALLBACK(on_refresh), ui);
  g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(refresh));
  g_autoptr(GSimpleAction) add_remote = g_simple_action_new("add-remote", NULL);
  g_signal_connect(add_remote, "activate", G_CALLBACK(on_add_remote), ui);
  g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(add_remote));
  ui->remove_remote = g_simple_action_new("remove-remote", NULL);
  g_signal_connect(ui->remove_remote, "activate", G_CALLBACK(on_remove_remote), ui);
  g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(ui->remove_remote));
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
  if (ui->names) {
    g_signal_handlers_disconnect_by_data(ui->names, ui);
    g_object_weak_unref(ui->names, names_gone, ui);
  }
  ui->name = name;
  ui->names = source;
  if (ui->names) {
    g_object_weak_ref(ui->names, names_gone, ui);
      g_signal_connect(ui->names, "profile-changed", G_CALLBACK(on_profile_changed), ui);
  }
  update(ui);
}
