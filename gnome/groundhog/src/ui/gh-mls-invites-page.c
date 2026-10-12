#include "gh-mls-invites-page.h"

#include "gh-mls-copy.h"
#include "gh-mls-key-packages.h"
#include "gh-shell.h"
#include "gh-recipient.h"

#include <glib/gi18n.h>

/* ---- GhMlsInviteRow (data/ui/gh-mls-invite-row.blp) ------------------------------------- */

#define GH_TYPE_MLS_INVITE_ROW (gh_mls_invite_row_get_type())
G_DECLARE_FINAL_TYPE(GhMlsInviteRow, gh_mls_invite_row, GH, MLS_INVITE_ROW, AdwPreferencesRow)

struct _GhMlsInviteRow {
  AdwPreferencesRow parent_instance;
  AdwAvatar *avatar;
  GtkLabel *title_label;
  GtkLabel *subtitle_label;
  GtkWidget *decline_button;
  GtkWidget *accept_button;
  gchar *wrapper_id;
};

G_DEFINE_FINAL_TYPE(GhMlsInviteRow, gh_mls_invite_row, ADW_TYPE_PREFERENCES_ROW)

static void
gh_mls_invite_row_finalize(GObject *object)
{
  g_free(GH_MLS_INVITE_ROW(object)->wrapper_id);
  G_OBJECT_CLASS(gh_mls_invite_row_parent_class)->finalize(object);
}

static void
gh_mls_invite_row_class_init(GhMlsInviteRowClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  G_OBJECT_CLASS(klass)->finalize = gh_mls_invite_row_finalize;
  gtk_widget_class_set_template_from_resource(widget_class,
                                              "/org/nostr/Groundhog/ui/gh-mls-invite-row.ui");
  gtk_widget_class_bind_template_child(widget_class, GhMlsInviteRow, avatar);
  gtk_widget_class_bind_template_child(widget_class, GhMlsInviteRow, title_label);
  gtk_widget_class_bind_template_child(widget_class, GhMlsInviteRow, subtitle_label);
  gtk_widget_class_bind_template_child(widget_class, GhMlsInviteRow, decline_button);
  gtk_widget_class_bind_template_child(widget_class, GhMlsInviteRow, accept_button);
}

static void
gh_mls_invite_row_init(GhMlsInviteRow *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
}

/* The words, plain text: the title as the row's (PreferencesRow) title too. */
static void
gh_mls_invite_row_set_text(GhMlsInviteRow *self, const gchar *title, const gchar *subtitle)
{
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self), title);
  gtk_label_set_text(self->title_label, title);
  gtk_label_set_text(self->subtitle_label, subtitle);
}

/* ---- GhMlsInvitesPage --------------------------------------------------------------- */

struct _GhMlsInvitesPage {
  AdwNavigationPage parent_instance;
  AdwActionRow *key_row;
  GtkWidget *empty_row;
  AdwPreferencesGroup *invites_group;
  AdwPreferencesGroup *refused_group;
  AdwActionRow *old_keys_row;
  GtkWidget *old_keys_button;

  GhMlsUiContext context;   /* service weak-watched, the others referenced */
  GPtrArray *rows;          /* GhMlsInviteRow in invites_group */
  GPtrArray *refused_rows;  /* AdwActionRow in refused_group ("wrapper-id" data) */
  gchar *last_toast;
  gboolean focus_on_map;    /* shown while off screen (collapsed) */
  GCancellable *old_keys_cancellable;  /* a look or a removal in flight */
};

enum { SIGNAL_OPEN_GROUP, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhMlsInvitesPage, gh_mls_invites_page, ADW_TYPE_NAVIGATION_PAGE)

static void
show_toast(GhMlsInvitesPage *self, AdwToast *toast)
{
  g_free(self->last_toast);
  self->last_toast = g_strdup(adw_toast_get_title(toast));
  /* The window's toasts: the panel is narrow, and Escape stays Back. */
  GtkWidget *overlay = gtk_widget_get_ancestor(GTK_WIDGET(self), ADW_TYPE_TOAST_OVERLAY);
  if (overlay)
    adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(overlay), toast);
  else
    g_object_unref(toast);
}

static gboolean
drop_stale_rows(gpointer data)
{
  GPtrArray *stale = data;
  for (guint i = 0; i < stale->len; i++)
    g_object_unref(g_ptr_array_index(stale, i));
  g_ptr_array_unref(stale);
  return G_SOURCE_REMOVE;
}

/* "Could Not Be Opened": invitations libmarmot refused, said as such. */
static void
add_refused_rows(GhMlsInvitesPage *self)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) refused = self->context.service
    ? gh_mls_service_list_refused_invites(self->context.service, &error) : NULL;
  if (error)
    g_message("Groundhog could not list the invitations it could not open: %s",
              error->message);
  for (guint i = 0; refused && i < refused->len; i++) {
    GhMlsRefusedInvite *invite = g_ptr_array_index(refused, i);
    /* No inviter: refused before Groundhog listed refusals, and libmarmot
     * keeps no sender for one. */
    gboolean contact = invite->inviter &&
                       gh_mls_is_contact(self->context.model, invite->inviter);
    const gchar *name = contact && self->context.display_name
      ? self->context.display_name(invite->inviter, self->context.names_data) : NULL;
    g_autofree gchar *npub = invite->inviter ? gh_recipient_npub_short(invite->inviter) : NULL;
    const gchar *who = name && *name ? name : npub ? npub : _("an unknown sender");
    AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
    g_autofree gchar *title = invite->inviter
      ? g_strdup_printf(_("Invitation from %s"), who)
      : g_strdup(_("An earlier invitation (sender not recorded)"));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_action_row_set_subtitle(row, invite->key_missing
      ? _("Made for a key this device doesn’t have. Ask them to invite you again.")
      : _("It could not be read. Ask them to invite you again."));
    GtkWidget *icon = gtk_image_new_from_icon_name("dialog-warning-symbolic");
    gtk_accessible_update_property(GTK_ACCESSIBLE(icon), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   _("Could not be opened"), -1);
    adw_action_row_add_prefix(row, icon);
    GtkWidget *dismiss = gtk_button_new_with_mnemonic(_("_Dismiss"));
    gtk_widget_set_valign(dismiss, GTK_ALIGN_CENTER);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(dismiss), "mls-invites.dismiss");
    gtk_actionable_set_action_target(GTK_ACTIONABLE(dismiss), "s", invite->wrapper_id);
    g_autofree gchar *dismiss_label = g_strdup_printf(_("Dismiss the invitation from %s"), who);
    gtk_accessible_update_property(GTK_ACCESSIBLE(dismiss), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   dismiss_label, -1);
    adw_action_row_add_suffix(row, dismiss);
    g_object_set_data_full(G_OBJECT(row), "wrapper-id", g_strdup(invite->wrapper_id), g_free);
    adw_preferences_group_add(self->refused_group, GTK_WIDGET(row));
    g_ptr_array_add(self->refused_rows, row);
  }
}

/* The first invitation's row, else the empty state's. */
static void
focus_list(GhMlsInvitesPage *self)
{
  gtk_widget_grab_focus(self->rows->len > 0 ? g_ptr_array_index(self->rows, 0)
                                            : self->empty_row);
}

static void
refresh(GhMlsInvitesPage *self)
{
  /* A focused row (its Accept, Decline or Dismiss) is about to go: move
   * the focus off it first, so GTK never tracks a half-destroyed widget as
   * focused (libadwaita 1.5 asserted on that). Ref old rows too, then drop
   * the refs on the next idle, so GTK's post-unparent bookkeeping can still
   * inspect them safely. The focus comes back to the list afterwards, so
   * the keyboard (Escape, Tab) stays in the panel. */
  gboolean refocus = FALSE;
  if (self->rows->len > 0 || self->refused_rows->len > 0) {
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
    GtkWidget *focus = root ? gtk_root_get_focus(root) : NULL;
    if (focus && gtk_widget_is_ancestor(focus, GTK_WIDGET(self))) {
      gtk_root_set_focus(root, NULL);
      refocus = TRUE;
    }
  }
  GPtrArray *stale = NULL;
  if (self->rows->len > 0) {
    stale = g_ptr_array_new();
    for (guint i = 0; i < self->rows->len; i++) {
      GtkWidget *row = g_ptr_array_index(self->rows, i);
      g_object_ref(row);
      g_ptr_array_add(stale, row);
      adw_preferences_group_remove(self->invites_group, row);
    }
    g_ptr_array_set_size(self->rows, 0);
  }
  if (self->refused_rows->len > 0) {
    if (!stale)
      stale = g_ptr_array_new();
    for (guint i = 0; i < self->refused_rows->len; i++) {
      GtkWidget *row = g_ptr_array_index(self->refused_rows, i);
      g_object_ref(row);
      g_ptr_array_add(stale, row);
      adw_preferences_group_remove(self->refused_group, row);
    }
    g_ptr_array_set_size(self->refused_rows, 0);
  }
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) invites = self->context.service
    ? gh_mls_service_list_invites(self->context.service, &error) : NULL;
  if (error)
    g_message("Groundhog could not list encrypted-group invitations: %s", error->message);
  for (guint i = 0; invites && i < invites->len; i++) {
    GhMlsInvite *invite = g_ptr_array_index(invites, i);
    GhMlsInviteRow *row = g_object_new(GH_TYPE_MLS_INVITE_ROW, NULL);
    row->wrapper_id = g_strdup(invite->wrapper_id);
    /* PD-8: only a contact's cached name; a stranger is their npub. */
    gboolean contact = gh_mls_is_contact(self->context.model, invite->inviter);
    const gchar *name = contact && self->context.display_name
      ? self->context.display_name(invite->inviter, self->context.names_data) : NULL;
    g_autofree gchar *npub = gh_recipient_npub_short(invite->inviter);
    /* DM invites (2-member, no name) show the person, not "Unnamed Group". */
    g_autofree gchar *group_label = NULL;
    if (invite->is_dm) {
      group_label = name && *name ? g_strdup(name) : g_strdup(npub);
    } else {
      group_label = invite->group_name && *invite->group_name
        ? g_strdup(invite->group_name) : g_strdup(_("Unnamed Group"));
    }
    adw_avatar_set_text(row->avatar, group_label);
    g_autofree gchar *subtitle = NULL;
    if (invite->is_dm) {
      subtitle = contact
        ? g_strdup(_("Marmot private message"))
        : g_strdup_printf(_("Marmot private message · %s, not in your contacts"), npub);
    } else {
      subtitle = gh_mls_invite_subtitle(npub, name, contact, invite->member_count);
    }
    gh_mls_invite_row_set_text(row, group_label, subtitle);
    gtk_actionable_set_action_target(GTK_ACTIONABLE(row->accept_button), "s",
                                     invite->wrapper_id);
    gtk_actionable_set_action_target(GTK_ACTIONABLE(row->decline_button), "s",
                                     invite->wrapper_id);
    g_autofree gchar *accept = invite->is_dm
      ? g_strdup_printf(_("Accept the message from %s"), group_label)
      : g_strdup_printf(_("Accept the invitation to %s"), group_label);
    g_autofree gchar *decline = invite->is_dm
      ? g_strdup_printf(_("Decline the message from %s"), group_label)
      : g_strdup_printf(_("Decline the invitation to %s"), group_label);
    gtk_accessible_update_property(GTK_ACCESSIBLE(row->accept_button),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, accept, -1);
    gtk_accessible_update_property(GTK_ACCESSIBLE(row->decline_button),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, decline, -1);
    adw_preferences_group_add(self->invites_group, GTK_WIDGET(row));
    g_ptr_array_add(self->rows, row);
  }
  add_refused_rows(self);
  gtk_widget_set_visible(self->empty_row, self->rows->len == 0);
  gtk_widget_set_visible(GTK_WIDGET(self->refused_group), self->refused_rows->len > 0);
  if (refocus)
    focus_list(self);
  if (stale)
    g_idle_add(drop_stale_rows, stale);
}

/* This device's invitation key, live while the panel is open. */
static void
sync_key(GhMlsInvitesPage *self)
{
  GhMlsService *service = self->context.service;
  if (!service)
    return;
  adw_action_row_set_subtitle(self->key_row,
    gh_mls_device_key_copy(gh_mls_service_get_key_package_state(service),
                           gh_mls_service_get_key_package_held(service),
                           gh_mls_service_get_identity_state(service)));
  /* Removing the others leaves this device's key alone: only once it is
   * published, never while a look or a removal runs. */
  gtk_widget_set_sensitive(self->old_keys_button,
                           !self->old_keys_cancellable &&
                           gh_mls_service_get_key_package_state(service) ==
                             GH_MLS_KEY_PACKAGE_PUBLISHED);
}

/* ---- Old invitation keys (nostrc-ab9kn) ------------------------------------------------ */

static void old_keys_list(GhMlsInvitesPage *self, gboolean ask);

static guint
old_keys_count(GPtrArray *listed)
{
  guint n = 0;
  for (guint i = 0; listed && i < listed->len; i++)
    n += !((GhMlsOwnKeyPackage *)g_ptr_array_index(listed, i))->current;
  return n;
}

static void
old_keys_settle(GhMlsInvitesPage *self, const gchar *subtitle)
{
  g_clear_object(&self->old_keys_cancellable);
  adw_action_row_set_subtitle(self->old_keys_row, subtitle);
  sync_key(self);
}

static void
old_keys_removed(GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GhMlsInvitesPage) self = data;
  g_autoptr(GError) error = NULL;
  guint removed = 0;
  gboolean ok = gh_mls_service_remove_other_key_packages_finish(GH_MLS_SERVICE(source), result,
                                                                &removed, &error);
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) || !self->old_keys_cancellable)
    return;
  if (!ok) {
    g_message("Groundhog could not remove old invitation keys: %s", error->message);
    old_keys_settle(self, _("Your relays did not take the request. Try again later."));
    show_toast(self, adw_toast_new(_("Old invitation keys could not be removed")));
    return;
  }
  g_autofree gchar *words = g_strdup_printf(
    g_dngettext(NULL, "Asked your relays to remove %u old invitation key",
                "Asked your relays to remove %u old invitation keys", removed), removed);
  show_toast(self, adw_toast_new(words));
  /* What the relays hold now. */
  g_clear_object(&self->old_keys_cancellable);
  old_keys_list(self, FALSE);
}

static void
old_keys_response(AdwAlertDialog *alert, const gchar *response, gpointer data)
{
  GhMlsInvitesPage *self = data;
  GPtrArray *listed = g_object_get_data(G_OBJECT(alert), "listed");
  if (g_strcmp0(response, "remove") != 0 || !listed || !self->context.service ||
      self->old_keys_cancellable)
    return;
  self->old_keys_cancellable = g_cancellable_new();
  adw_action_row_set_subtitle(self->old_keys_row, _("Asking your relays to remove them…"));
  sync_key(self);
  gh_mls_service_remove_other_key_packages_async(self->context.service, listed,
                                                 self->old_keys_cancellable, old_keys_removed,
                                                 g_object_ref(self));
}

/* "Published Oct 8, 2026" in the local time zone. */
static gchar *
old_key_date(gint64 created_at)
{
  g_autoptr(GDateTime) when = g_date_time_new_from_unix_local(created_at);
  g_autofree gchar *date = when ? g_date_time_format(when, "%x") : NULL;
  return date ? g_strdup_printf(_("Published %s"), date) : g_strdup(_("Publication date unknown"));
}

/* The confirmation: every key found, this device's marked as kept. */
static void
old_keys_confirm(GhMlsInvitesPage *self, GPtrArray *listed, guint others)
{
  g_autofree gchar *heading = g_strdup_printf(
    g_dngettext(NULL, "Remove %u Old Invitation Key?", "Remove %u Old Invitation Keys?", others),
    others);
  AdwAlertDialog *alert = ADW_ALERT_DIALOG(adw_alert_dialog_new(heading,
    _("People who invite you pick one of your invitation keys, and an invitation made for a key "
      "this device doesn’t have can’t be opened here. This asks your relays to delete every "
      "key below except this device’s.\n\nKeys from your other devices or apps, such as "
      "White Noise, stop working for new invitations until those apps publish new ones. "
      "Groups you already joined are not affected.")));
  GtkWidget *list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(list, "boxed-list");
  for (guint i = 0; i < listed->len; i++) {
    GhMlsOwnKeyPackage *kp = g_ptr_array_index(listed, i);
    AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), kp->current
      ? _("This device (kept)")
      : kp->kind == GH_MLS_KIND_LEGACY_KEY_PACKAGE ? _("Older-format key")
                                                   : _("Another device, app or install"));
    g_autofree gchar *date = old_key_date(kp->created_at);
    adw_action_row_set_subtitle(row, date);
    GtkWidget *icon = gtk_image_new_from_icon_name(kp->current ? "emblem-ok-symbolic"
                                                               : "user-trash-symbolic");
    gtk_accessible_update_property(GTK_ACCESSIBLE(icon), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   kp->current ? _("Kept") : _("Removed"), -1);
    adw_action_row_add_prefix(row, icon);
    gtk_list_box_append(GTK_LIST_BOX(list), GTK_WIDGET(row));
  }
  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroller), TRUE);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroller), 260);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), list);
  adw_alert_dialog_set_extra_child(alert, scroller);
  adw_alert_dialog_add_responses(alert, "cancel", _("_Cancel"), "remove", _("_Remove"), NULL);
  adw_alert_dialog_set_response_appearance(alert, "remove", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(alert, "cancel");
  adw_alert_dialog_set_close_response(alert, "cancel");
  g_object_set_data_full(G_OBJECT(alert), "listed", g_ptr_array_ref(listed),
                         (GDestroyNotify)g_ptr_array_unref);
  g_signal_connect_object(alert, "response", G_CALLBACK(old_keys_response), self, 0);
  adw_dialog_present(ADW_DIALOG(alert), GTK_WIDGET(self));
}

typedef struct {
  GhMlsInvitesPage *page;   /* referenced */
  gboolean ask;             /* the user asked: confirm when there are others */
} OldKeysLook;

static void
old_keys_listed(GObject *source, GAsyncResult *result, gpointer data)
{
  OldKeysLook *look = data;
  g_autoptr(GhMlsInvitesPage) self = look->page;
  gboolean ask = look->ask;
  g_free(look);
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) listed = gh_mls_service_list_own_key_packages_finish(
    GH_MLS_SERVICE(source), result, &error);
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) || !self->old_keys_cancellable)
    return;
  if (!listed) {
    g_message("Groundhog could not list its invitation keys: %s", error->message);
    old_keys_settle(self, _("Your relays could not be checked. Try again later."));
    if (ask)
      show_toast(self, adw_toast_new(_("Your relays could not be checked")));
    return;
  }
  guint others = old_keys_count(listed);
  if (others == 0) {
    old_keys_settle(self, _("No other keys are on your relays."));
    if (ask)
      show_toast(self, adw_toast_new(_("No old invitation keys on your relays")));
    return;
  }
  g_autofree gchar *found = g_strdup_printf(
    g_dngettext(NULL, "%u other key is on your relays.", "%u other keys are on your relays.",
                others), others);
  old_keys_settle(self, found);
  if (ask)
    old_keys_confirm(self, listed, others);
}

static void
old_keys_list(GhMlsInvitesPage *self, gboolean ask)
{
  if (!self->context.service || self->old_keys_cancellable)
    return;
  self->old_keys_cancellable = g_cancellable_new();
  adw_action_row_set_subtitle(self->old_keys_row, _("Looking on your relays…"));
  sync_key(self);
  OldKeysLook *look = g_new0(OldKeysLook, 1);
  look->page = g_object_ref(self);
  look->ask = ask;
  gh_mls_service_list_own_key_packages_async(self->context.service, self->old_keys_cancellable,
                                             old_keys_listed, look);
}

static void
action_review_keys(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  (void)parameter;
  old_keys_list(GH_MLS_INVITES_PAGE(widget), TRUE);
}

static void
action_accept(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  GhMlsInvitesPage *self = GH_MLS_INVITES_PAGE(widget);
  const gchar *wrapper = g_variant_get_string(parameter, NULL);
  if (!self->context.service)
    return;
  g_autoptr(GError) error = NULL;
  GhMlsGroup *group = gh_mls_service_accept_invite(self->context.service, wrapper, &error);
  if (!group) {
    g_message("Groundhog could not accept an encrypted-group invitation: %s", error->message);
    g_autofree gchar *words = gh_mls_error_copy(error);
    show_toast(self, adw_toast_new(words));
    refresh(self);
    return;
  }
  refresh(self);
  /* Joined: the panels close and the group opens (gh-mls-ui.c), with the
   * window's "You joined" toast. */
  g_autoptr(GhMlsGroup) held = g_object_ref(group);
  g_object_ref(self);
  g_signal_emit(self, signals[SIGNAL_OPEN_GROUP], 0, held);
  g_object_unref(self);
}

static void
action_decline(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  GhMlsInvitesPage *self = GH_MLS_INVITES_PAGE(widget);
  const gchar *wrapper = g_variant_get_string(parameter, NULL);
  if (!self->context.service)
    return;
  g_autoptr(GError) error = NULL;
  if (!gh_mls_service_decline_invite(self->context.service, wrapper, &error)) {
    g_message("Groundhog could not decline an encrypted-group invitation: %s", error->message);
    g_autofree gchar *words = gh_mls_error_copy(error);
    show_toast(self, adw_toast_new(words));
  } else {
    show_toast(self, adw_toast_new(_("Invitation declined")));
  }
  refresh(self);
}

static void
action_dismiss(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  GhMlsInvitesPage *self = GH_MLS_INVITES_PAGE(widget);
  if (!self->context.service)
    return;
  g_autoptr(GError) error = NULL;
  if (!gh_mls_service_dismiss_refused_invite(self->context.service,
                                             g_variant_get_string(parameter, NULL), &error))
    g_message("Groundhog could not dismiss an invitation: %s", error->message);
  refresh(self);
}

static void
on_service_gone(gpointer data, GObject *where)
{
  GhMlsInvitesPage *self = data;
  (void)where;
  self->context.service = NULL;
  gh_sidebar_panel_leave(ADW_NAVIGATION_PAGE(self));
}

GhMlsInvitesPage *
gh_mls_invites_page_new(const GhMlsUiContext *context)
{
  g_return_val_if_fail(context != NULL && GH_IS_MLS_SERVICE(context->service), NULL);
  g_return_val_if_fail(GH_IS_CONVERSATION_STORE(context->model), NULL);
  GhMlsInvitesPage *self = g_object_new(GH_TYPE_MLS_INVITES_PAGE, NULL);
  self->context = *context;
  self->context.default_relays = NULL;
  g_object_weak_ref(G_OBJECT(context->service), on_service_gone, self);
  /* Only the service and the model (who is a contact) are needed here. */
  self->context.accounts = NULL;
  self->context.settings = NULL;
  g_object_ref(context->model);
  g_signal_connect_object(context->service, "invite-received", G_CALLBACK(refresh), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(context->service, "invite-refused", G_CALLBACK(refresh), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(context->service, "notify::key-package-state", G_CALLBACK(sync_key),
                          self, G_CONNECT_SWAPPED);
  g_signal_connect_object(context->service, "notify::key-package-held", G_CALLBACK(sync_key),
                          self, G_CONNECT_SWAPPED);
  g_signal_connect_object(context->service, "notify::identity-state", G_CALLBACK(sync_key),
                          self, G_CONNECT_SWAPPED);
  sync_key(self);
  refresh(self);
  return self;
}

guint
gh_mls_invites_page_get_n_invites(GhMlsInvitesPage *self)
{
  g_return_val_if_fail(GH_IS_MLS_INVITES_PAGE(self), 0);
  return self->rows->len;
}

guint
gh_mls_invites_page_get_n_refused(GhMlsInvitesPage *self)
{
  g_return_val_if_fail(GH_IS_MLS_INVITES_PAGE(self), 0);
  return self->refused_rows->len;
}

gboolean
gh_mls_invites_page_describe(GhMlsInvitesPage *self, const gchar *wrapper_id,
                               const gchar **title, const gchar **subtitle)
{
  g_return_val_if_fail(GH_IS_MLS_INVITES_PAGE(self), FALSE);
  for (guint i = 0; i < self->rows->len; i++) {
    GhMlsInviteRow *row = g_ptr_array_index(self->rows, i);
    if (g_strcmp0(row->wrapper_id, wrapper_id) != 0)
      continue;
    if (title)
      *title = gtk_label_get_text(row->title_label);
    if (subtitle)
      *subtitle = gtk_label_get_text(row->subtitle_label);
    return TRUE;
  }
  for (guint i = 0; i < self->refused_rows->len; i++) {
    GtkWidget *row = g_ptr_array_index(self->refused_rows, i);
    if (g_strcmp0(g_object_get_data(G_OBJECT(row), "wrapper-id"), wrapper_id) != 0)
      continue;
    if (title)
      *title = adw_preferences_row_get_title(ADW_PREFERENCES_ROW(row));
    if (subtitle)
      *subtitle = adw_action_row_get_subtitle(ADW_ACTION_ROW(row));
    return TRUE;
  }
  return FALSE;
}

const gchar *
gh_mls_invites_page_get_key_status(GhMlsInvitesPage *self)
{
  g_return_val_if_fail(GH_IS_MLS_INVITES_PAGE(self), NULL);
  return adw_action_row_get_subtitle(self->key_row);
}

const gchar *
gh_mls_invites_page_get_last_toast(GhMlsInvitesPage *self)
{
  g_return_val_if_fail(GH_IS_MLS_INVITES_PAGE(self), NULL);
  return self->last_toast;
}

const gchar *
gh_mls_invites_page_get_old_keys_status(GhMlsInvitesPage *self)
{
  g_return_val_if_fail(GH_IS_MLS_INVITES_PAGE(self), NULL);
  return adw_action_row_get_subtitle(self->old_keys_row);
}

gboolean
gh_mls_invites_page_get_old_keys_busy(GhMlsInvitesPage *self)
{
  g_return_val_if_fail(GH_IS_MLS_INVITES_PAGE(self), FALSE);
  return self->old_keys_cancellable != NULL;
}

static void
gh_mls_invites_page_dispose(GObject *object)
{
  GhMlsInvitesPage *self = GH_MLS_INVITES_PAGE(object);
  if (self->old_keys_cancellable) {
    g_cancellable_cancel(self->old_keys_cancellable);
    g_clear_object(&self->old_keys_cancellable);
  }
  if (self->context.service) {
    g_object_weak_unref(G_OBJECT(self->context.service), on_service_gone, self);
    self->context.service = NULL;
  }
  g_clear_object(&self->context.model);
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_MLS_INVITES_PAGE);
  G_OBJECT_CLASS(gh_mls_invites_page_parent_class)->dispose(object);
}

static void
gh_mls_invites_page_finalize(GObject *object)
{
  GhMlsInvitesPage *self = GH_MLS_INVITES_PAGE(object);
  g_ptr_array_unref(self->rows);
  g_ptr_array_unref(self->refused_rows);
  g_free(self->last_toast);
  G_OBJECT_CLASS(gh_mls_invites_page_parent_class)->finalize(object);
}

/* The keyboard starts in the panel, on the first invitation's row (or the
 * empty state), so Escape and Tab work at once; nothing is pressed by Enter
 * or Space there. */
static void
gh_mls_invites_page_shown(AdwNavigationPage *page)
{
  GhMlsInvitesPage *self = GH_MLS_INVITES_PAGE(page);
  /* Shown before it is on screen (the collapsed sidebar comes back with
   * it): the focus moves in once it is, after the sidebar's own. */
  if (gtk_widget_get_mapped(GTK_WIDGET(self)))
    focus_list(self);
  else
    self->focus_on_map = TRUE;
  if (ADW_NAVIGATION_PAGE_CLASS(gh_mls_invites_page_parent_class)->shown)
    ADW_NAVIGATION_PAGE_CLASS(gh_mls_invites_page_parent_class)->shown(page);
}

static void
gh_mls_invites_page_map(GtkWidget *widget)
{
  GhMlsInvitesPage *self = GH_MLS_INVITES_PAGE(widget);
  GTK_WIDGET_CLASS(gh_mls_invites_page_parent_class)->map(widget);
  if (self->focus_on_map) {
    self->focus_on_map = FALSE;
    focus_list(self);
  }
}

static void
gh_mls_invites_page_class_init(GhMlsInvitesPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  object_class->dispose = gh_mls_invites_page_dispose;
  object_class->finalize = gh_mls_invites_page_finalize;
  ADW_NAVIGATION_PAGE_CLASS(klass)->shown = gh_mls_invites_page_shown;
  widget_class->map = gh_mls_invites_page_map;
  signals[SIGNAL_OPEN_GROUP] = g_signal_new("open-group", G_TYPE_FROM_CLASS(klass),
                                            G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                            G_TYPE_NONE, 1, GH_TYPE_MLS_GROUP);
  gtk_widget_class_set_template_from_resource(
    widget_class, "/org/nostr/Groundhog/ui/gh-mls-invites-page.ui");
  gtk_widget_class_bind_template_child(widget_class, GhMlsInvitesPage, key_row);
  gtk_widget_class_bind_template_child(widget_class, GhMlsInvitesPage, empty_row);
  gtk_widget_class_bind_template_child(widget_class, GhMlsInvitesPage, invites_group);
  gtk_widget_class_bind_template_child(widget_class, GhMlsInvitesPage, refused_group);
  gtk_widget_class_bind_template_child(widget_class, GhMlsInvitesPage, old_keys_row);
  gtk_widget_class_bind_template_child(widget_class, GhMlsInvitesPage, old_keys_button);
  gtk_widget_class_install_action(widget_class, "mls-invites.review-keys", NULL,
                                  action_review_keys);
  gtk_widget_class_install_action(widget_class, "mls-invites.accept", "s", action_accept);
  gtk_widget_class_install_action(widget_class, "mls-invites.decline", "s", action_decline);
  gtk_widget_class_install_action(widget_class, "mls-invites.dismiss", "s", action_dismiss);
}

static void
gh_mls_invites_page_init(GhMlsInvitesPage *self)
{
  self->rows = g_ptr_array_new();
  self->refused_rows = g_ptr_array_new();
  gtk_widget_init_template(GTK_WIDGET(self));
}
