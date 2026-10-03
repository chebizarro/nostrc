#include "gh-mls-invites-dialog.h"

#include "gh-mls-copy.h"
#include "gh-recipient.h"

#include <glib/gi18n.h>

/* ---- GhMlsInviteRow (data/ui/gh-mls-invite-row.blp) ------------------------------------- */

#define GH_TYPE_MLS_INVITE_ROW (gh_mls_invite_row_get_type())
G_DECLARE_FINAL_TYPE(GhMlsInviteRow, gh_mls_invite_row, GH, MLS_INVITE_ROW, AdwActionRow)

struct _GhMlsInviteRow {
  AdwActionRow parent_instance;
  AdwAvatar *avatar;
  GtkWidget *decline_button;
  GtkWidget *accept_button;
  gchar *wrapper_id;
};

G_DEFINE_FINAL_TYPE(GhMlsInviteRow, gh_mls_invite_row, ADW_TYPE_ACTION_ROW)

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
  gtk_widget_class_bind_template_child(widget_class, GhMlsInviteRow, decline_button);
  gtk_widget_class_bind_template_child(widget_class, GhMlsInviteRow, accept_button);
}

static void
gh_mls_invite_row_init(GhMlsInviteRow *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
}

/* ---- GhMlsInvitesDialog --------------------------------------------------------------- */

struct _GhMlsInvitesDialog {
  AdwDialog parent_instance;
  AdwToastOverlay *toasts;
  GtkStack *stack;
  AdwPreferencesGroup *invites_group;

  GhMlsUiContext context;   /* service weak-watched, the others referenced */
  GPtrArray *rows;          /* GhMlsInviteRow in invites_group */
  gchar *last_toast;
};

enum { SIGNAL_OPEN_GROUP, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhMlsInvitesDialog, gh_mls_invites_dialog, ADW_TYPE_DIALOG)

static void
show_toast(GhMlsInvitesDialog *self, AdwToast *toast)
{
  g_free(self->last_toast);
  self->last_toast = g_strdup(adw_toast_get_title(toast));
  adw_toast_overlay_add_toast(self->toasts, toast);
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

static void
refresh(GhMlsInvitesDialog *self)
{
  /* Clear focus before removing rows: on libadwaita 1.5 (Ubuntu 24.04),
   * adw_dialog_set_focus() fires during gtk_widget_unparent() via the
   * set-focus-child signal chain and receives a half-destroyed widget,
   * hitting "gtk_widget_get_can_focus: assertion 'GTK_IS_WIDGET (widget)'
   * failed".  Clearing the root's focus first prevents the stale callback.
   * Ref old rows too, then drop the refs on the next idle, so GTK's
   * post-unparent bookkeeping can still inspect them safely. */
  if (self->rows->len > 0) {
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
    if (root)
      gtk_root_set_focus(root, NULL);
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
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), group_label);
    adw_avatar_set_text(row->avatar, group_label);
    g_autofree gchar *subtitle = NULL;
    if (invite->is_dm) {
      subtitle = contact
        ? g_strdup(_("Marmot private message"))
        : g_strdup_printf(_("Marmot private message · %s, not in your contacts"), npub);
    } else {
      subtitle = gh_mls_invite_subtitle(npub, name, contact, invite->member_count);
    }
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
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
  gtk_stack_set_visible_child_name(self->stack, self->rows->len > 0 ? "list" : "empty");
  if (stale)
    g_idle_add(drop_stale_rows, stale);
}

static void
action_accept(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  GhMlsInvitesDialog *self = GH_MLS_INVITES_DIALOG(widget);
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
  const gchar *name = gh_mls_group_get_name(group);
  g_autofree gchar *title = g_strdup_printf(_("You joined “%s”"),
                                            name && *name ? name : _("Unnamed Group"));
  AdwToast *toast = adw_toast_new(title);
  adw_toast_set_button_label(toast, _("_Open"));
  adw_toast_set_action_name(toast, "mls-invites.open");
  adw_toast_set_action_target(toast, "s", gh_mls_group_get_group_id(group));
  show_toast(self, toast);
  refresh(self);
}

static void
action_decline(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  GhMlsInvitesDialog *self = GH_MLS_INVITES_DIALOG(widget);
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
action_open(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  GhMlsInvitesDialog *self = GH_MLS_INVITES_DIALOG(widget);
  GhMlsGroup *group = self->context.service
    ? gh_mls_service_lookup(self->context.service, g_variant_get_string(parameter, NULL))
    : NULL;
  if (!group)
    return;
  g_autoptr(GhMlsGroup) held = g_object_ref(group);
  g_object_ref(self);
  adw_dialog_close(ADW_DIALOG(self));
  g_signal_emit(self, signals[SIGNAL_OPEN_GROUP], 0, held);
  g_object_unref(self);
}

static void
on_service_gone(gpointer data, GObject *where)
{
  GhMlsInvitesDialog *self = data;
  (void)where;
  self->context.service = NULL;
  adw_dialog_force_close(ADW_DIALOG(self));
}

GhMlsInvitesDialog *
gh_mls_invites_dialog_new(const GhMlsUiContext *context)
{
  g_return_val_if_fail(context != NULL && GH_IS_MLS_SERVICE(context->service), NULL);
  g_return_val_if_fail(GH_IS_CONVERSATION_STORE(context->model), NULL);
  GhMlsInvitesDialog *self = g_object_new(GH_TYPE_MLS_INVITES_DIALOG, NULL);
  self->context = *context;
  self->context.default_relays = NULL;
  g_object_weak_ref(G_OBJECT(context->service), on_service_gone, self);
  /* Only the service and the model (who is a contact) are needed here. */
  self->context.accounts = NULL;
  self->context.settings = NULL;
  g_object_ref(context->model);
  g_signal_connect_object(context->service, "invite-received", G_CALLBACK(refresh), self,
                          G_CONNECT_SWAPPED);
  refresh(self);
  return self;
}

guint
gh_mls_invites_dialog_get_n_invites(GhMlsInvitesDialog *self)
{
  g_return_val_if_fail(GH_IS_MLS_INVITES_DIALOG(self), 0);
  return self->rows->len;
}

gboolean
gh_mls_invites_dialog_describe(GhMlsInvitesDialog *self, const gchar *wrapper_id,
                               const gchar **title, const gchar **subtitle)
{
  g_return_val_if_fail(GH_IS_MLS_INVITES_DIALOG(self), FALSE);
  for (guint i = 0; i < self->rows->len; i++) {
    GhMlsInviteRow *row = g_ptr_array_index(self->rows, i);
    if (g_strcmp0(row->wrapper_id, wrapper_id) != 0)
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
gh_mls_invites_dialog_get_last_toast(GhMlsInvitesDialog *self)
{
  g_return_val_if_fail(GH_IS_MLS_INVITES_DIALOG(self), NULL);
  return self->last_toast;
}

static void
gh_mls_invites_dialog_dispose(GObject *object)
{
  GhMlsInvitesDialog *self = GH_MLS_INVITES_DIALOG(object);
  if (self->context.service) {
    g_object_weak_unref(G_OBJECT(self->context.service), on_service_gone, self);
    self->context.service = NULL;
  }
  g_clear_object(&self->context.model);
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_MLS_INVITES_DIALOG);
  G_OBJECT_CLASS(gh_mls_invites_dialog_parent_class)->dispose(object);
}

static void
gh_mls_invites_dialog_finalize(GObject *object)
{
  GhMlsInvitesDialog *self = GH_MLS_INVITES_DIALOG(object);
  g_ptr_array_unref(self->rows);
  g_free(self->last_toast);
  G_OBJECT_CLASS(gh_mls_invites_dialog_parent_class)->finalize(object);
}

static void
gh_mls_invites_dialog_class_init(GhMlsInvitesDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  object_class->dispose = gh_mls_invites_dialog_dispose;
  object_class->finalize = gh_mls_invites_dialog_finalize;
  signals[SIGNAL_OPEN_GROUP] = g_signal_new("open-group", G_TYPE_FROM_CLASS(klass),
                                            G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                            G_TYPE_NONE, 1, GH_TYPE_MLS_GROUP);
  gtk_widget_class_set_template_from_resource(
    widget_class, "/org/nostr/Groundhog/ui/gh-mls-invites-dialog.ui");
  gtk_widget_class_bind_template_child(widget_class, GhMlsInvitesDialog, toasts);
  gtk_widget_class_bind_template_child(widget_class, GhMlsInvitesDialog, stack);
  gtk_widget_class_bind_template_child(widget_class, GhMlsInvitesDialog, invites_group);
  gtk_widget_class_install_action(widget_class, "mls-invites.accept", "s", action_accept);
  gtk_widget_class_install_action(widget_class, "mls-invites.decline", "s", action_decline);
  gtk_widget_class_install_action(widget_class, "mls-invites.open", "s", action_open);
}

static void
gh_mls_invites_dialog_init(GhMlsInvitesDialog *self)
{
  self->rows = g_ptr_array_new();
  gtk_widget_init_template(GTK_WIDGET(self));
}
