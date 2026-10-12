#include "gh-group-restore-dialog.h"

#include <glib/gi18n.h>

struct _GhGroupRestoreDialog {
  AdwAlertDialog parent_instance;
  GPtrArray *groups; /* GhNip29ListedGroup */
  GPtrArray *checks; /* GtkCheckButton, borrowed from the rows */
};

G_DEFINE_FINAL_TYPE(GhGroupRestoreDialog, gh_group_restore_dialog, ADW_TYPE_ALERT_DIALOG)

static void
sync_restore(GhGroupRestoreDialog *self)
{
  gboolean any = FALSE;
  for (guint i = 0; i < self->checks->len && !any; i++)
    any = gtk_check_button_get_active(g_ptr_array_index(self->checks, i));
  adw_alert_dialog_set_response_enabled(ADW_ALERT_DIALOG(self),
                                        GH_GROUP_RESTORE_RESPONSE_RESTORE, any);
}

/* The relay as people read it: its host (and port), without the scheme. */
static gchar *
relay_label(const gchar *url)
{
  const gchar *rest = strstr(url, "://");
  return g_strdup(rest ? rest + 3 : url);
}

GhGroupRestoreDialog *
gh_group_restore_dialog_new(GPtrArray *groups)
{
  g_return_val_if_fail(groups != NULL && groups->len > 0, NULL);
  GhGroupRestoreDialog *self = g_object_new(GH_TYPE_GROUP_RESTORE_DIALOG, NULL);
  AdwAlertDialog *alert = ADW_ALERT_DIALOG(self);
  adw_alert_dialog_set_heading(alert, _("Restore Groups?"));
  g_autofree gchar *body = g_strdup_printf(
    g_dngettext(NULL,
                "Your group list names a group that isn’t on this device. Restoring asks its "
                "relay to let you in again, signed as you.",
                "Your group list names %u groups that aren’t on this device. Restoring asks "
                "each group’s relay to let you in again, signed as you.",
                groups->len),
    groups->len);
  adw_alert_dialog_set_body(alert, body);

  GtkWidget *list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(list, "boxed-list");
  gtk_accessible_update_property(GTK_ACCESSIBLE(list), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 _("Groups to restore"), -1);
  for (guint i = 0; i < groups->len; i++) {
    const GhNip29ListedGroup *group = g_ptr_array_index(groups, i);
    GhNip29ListedGroup *copy = g_new0(GhNip29ListedGroup, 1);
    copy->relay_url = g_strdup(group->relay_url);
    copy->group_id = g_strdup(group->group_id);
    copy->name = g_strdup(group->name);
    g_ptr_array_add(self->groups, copy);

    const gchar *title = group->name ? group->name : group->group_id;
    g_autofree gchar *relay = relay_label(group->relay_url);
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), relay);
    GtkWidget *check = gtk_check_button_new();
    gtk_widget_set_valign(check, GTK_ALIGN_CENTER);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(check), TRUE);
    g_autofree gchar *label = g_strdup_printf(_("Restore %s on %s"), title, relay);
    gtk_accessible_update_property(GTK_ACCESSIBLE(check), GTK_ACCESSIBLE_PROPERTY_LABEL, label,
                                   -1);
    g_signal_connect_object(check, "toggled", G_CALLBACK(sync_restore), self,
                            G_CONNECT_SWAPPED);
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), check);
    adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row), check);
    gtk_list_box_append(GTK_LIST_BOX(list), row);
    g_ptr_array_add(self->checks, check);
  }
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 320);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
  adw_alert_dialog_set_extra_child(alert, scroll);

  adw_alert_dialog_add_responses(alert, GH_GROUP_RESTORE_RESPONSE_NOT_NOW, _("Not Now"),
                                 GH_GROUP_RESTORE_RESPONSE_RESTORE, _("Restore"), NULL);
  adw_alert_dialog_set_response_appearance(alert, GH_GROUP_RESTORE_RESPONSE_RESTORE,
                                           ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response(alert, GH_GROUP_RESTORE_RESPONSE_NOT_NOW);
  adw_alert_dialog_set_close_response(alert, GH_GROUP_RESTORE_RESPONSE_NOT_NOW);
  sync_restore(self);
  return self;
}

GPtrArray *
gh_group_restore_dialog_dup_selected(GhGroupRestoreDialog *self)
{
  g_return_val_if_fail(GH_IS_GROUP_RESTORE_DIALOG(self), NULL);
  GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)gh_nip29_listed_group_free);
  for (guint i = 0; i < self->groups->len; i++) {
    if (!gtk_check_button_get_active(g_ptr_array_index(self->checks, i)))
      continue;
    const GhNip29ListedGroup *group = g_ptr_array_index(self->groups, i);
    GhNip29ListedGroup *copy = g_new0(GhNip29ListedGroup, 1);
    copy->relay_url = g_strdup(group->relay_url);
    copy->group_id = g_strdup(group->group_id);
    copy->name = g_strdup(group->name);
    g_ptr_array_add(out, copy);
  }
  return out;
}

GtkCheckButton *
gh_group_restore_dialog_get_check(GhGroupRestoreDialog *self, guint index)
{
  g_return_val_if_fail(GH_IS_GROUP_RESTORE_DIALOG(self), NULL);
  return index < self->checks->len ? g_ptr_array_index(self->checks, index) : NULL;
}

static void
gh_group_restore_dialog_finalize(GObject *object)
{
  GhGroupRestoreDialog *self = GH_GROUP_RESTORE_DIALOG(object);
  g_ptr_array_unref(self->groups);
  g_ptr_array_unref(self->checks);
  G_OBJECT_CLASS(gh_group_restore_dialog_parent_class)->finalize(object);
}

static void
gh_group_restore_dialog_class_init(GhGroupRestoreDialogClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = gh_group_restore_dialog_finalize;
}

static void
gh_group_restore_dialog_init(GhGroupRestoreDialog *self)
{
  self->groups = g_ptr_array_new_with_free_func((GDestroyNotify)gh_nip29_listed_group_free);
  self->checks = g_ptr_array_new();
}
