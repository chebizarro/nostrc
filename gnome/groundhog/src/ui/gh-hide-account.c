#include "gh-hide-account.h"
#include <glib/gi18n.h>
#include <string.h>

#define HIDE_DATA "groundhog-hide-account"
typedef struct {
  GtkApplication *app; /* application owns this context */
  GhSocialLists *lists;
  GSimpleAction *action;
  AdwAlertDialog *dialog;
  guint64 generation;
  gchar *account, *target;
} Hide;

static void
clear_dialog(Hide *self)
{
  if (self->dialog) {
    g_signal_handlers_disconnect_by_data(self->dialog, self);
    adw_dialog_force_close(ADW_DIALOG(self->dialog));
    g_clear_object(&self->dialog);
  }
  g_clear_pointer(&self->account, g_free);
  g_clear_pointer(&self->target, g_free);
}
static void
changed(GhSocialLists *lists, Hide *self)
{
  if (self->dialog
      && (self->generation != gh_social_lists_get_generation(lists)
          || g_strcmp0(self->account, gh_social_lists_get_account(lists))))
    clear_dialog(self);
  g_simple_action_set_enabled(self->action, gh_social_lists_get_account(lists) != NULL);
}
static void
response(AdwAlertDialog *dialog, const gchar *answer, Hide *self)
{
  (void)dialog;
  if (g_strcmp0(answer, "hide") || self->generation != gh_social_lists_get_generation(self->lists)
      || g_strcmp0(self->account, gh_social_lists_get_account(self->lists))) {
    clear_dialog(self);
    return;
  }
  GError *error = NULL;
  gboolean saved = gh_social_lists_hide_account(self->lists, self->target, &error);
  GhMessagePolicy *policy = gh_social_lists_get_message_policy(self->lists);
  gboolean projection_failed = saved && policy && gh_message_policy_get_error(policy);
  if (!saved || projection_failed) {
    /* Keep the initiating surface intact. Storage errors contain no payload. */
    GtkWindow *parent_window = gtk_application_get_active_window(self->app);
    GtkWidget *parent = parent_window ? GTK_WIDGET(parent_window) : NULL;
    AdwAlertDialog *failure = ADW_ALERT_DIALOG(adw_alert_dialog_new(
        projection_failed ? _("Hidden Messages Need an Update") : _("Account Could Not Be Hidden"),
        projection_failed ? _("The hide entry was saved, but history could not be updated. Open "
                              "Hidden Accounts and Content in Preferences to retry.")
        : error           ? error->message
                          : _("Account storage is not available")));
    adw_alert_dialog_add_response(failure, "close", _("_Close"));
    adw_alert_dialog_set_close_response(failure, "close");
    adw_dialog_present(ADW_DIALOG(failure), parent);
    g_clear_error(&error);
  }
  clear_dialog(self);
}
static void
activate(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  (void)action;
  Hide *self = data;
  const gchar *account, *target;
  g_variant_get(parameter, "(&s&s)", &account, &target);
  if (g_strcmp0(account, gh_social_lists_get_account(self->lists)) || !g_strcmp0(account, target)
      || strlen(target) != 64)
    return;
  for (guint i = 0; i < 64; i++)
    if (!g_ascii_isxdigit(target[i]))
      return;
  clear_dialog(self);
  self->account = g_strdup(account);
  self->target = g_ascii_strdown(target, -1);
  self->generation = gh_social_lists_get_generation(self->lists);
  self->dialog = ADW_ALERT_DIALOG(g_object_ref_sink(adw_alert_dialog_new(
      _("Hide Account?"), _("New messages from this account will not be saved. Existing history is "
                            "retained and can be shown again in Hidden Accounts and Content. This "
                            "does not block the conversation or accept a message request."))));
  adw_alert_dialog_add_responses(self->dialog, "cancel", _("_Cancel"), "hide", _("_Hide Account"),
                                 NULL);
  adw_alert_dialog_set_default_response(self->dialog, "cancel");
  adw_alert_dialog_set_close_response(self->dialog, "cancel");
  adw_alert_dialog_set_response_appearance(self->dialog, "hide", ADW_RESPONSE_DESTRUCTIVE);
  g_signal_connect(self->dialog, "response", G_CALLBACK(response), self);
  GtkWindow *window = gtk_application_get_active_window(self->app);
  adw_dialog_present(ADW_DIALOG(self->dialog), window ? GTK_WIDGET(window) : NULL);
}
static void
hide_free(gpointer data)
{
  Hide *self = data;
  clear_dialog(self);
  g_signal_handlers_disconnect_by_data(self->lists, self);
  g_action_map_remove_action(G_ACTION_MAP(self->app), "hide-account");
  g_object_unref(self->action);
  g_object_unref(self->lists);
  g_free(self);
}
void
gh_hide_account_uninstall(GtkApplication *app)
{
  g_object_set_data(G_OBJECT(app), HIDE_DATA, NULL);
}
void
gh_hide_account_install(GtkApplication *app, GhSocialLists *lists)
{
  gh_hide_account_uninstall(app);
  Hide *self = g_new0(Hide, 1);
  self->app = app;
  self->lists = g_object_ref(lists);
  self->action = g_simple_action_new("hide-account", G_VARIANT_TYPE("(ss)"));
  g_signal_connect(self->action, "activate", G_CALLBACK(activate), self);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(self->action));
  g_signal_connect(lists, "changed", G_CALLBACK(changed), self);
  g_object_set_data_full(G_OBJECT(app), HIDE_DATA, self, hide_free);
  changed(lists, self);
}
