#define G_LOG_DOMAIN "gnostr-main-window-actions"

#include "gnostr-main-window-private.h"

void
gnostr_main_window_on_show_about_activated_internal(GSimpleAction *action,
                                                    GVariant *param,
                                                    gpointer user_data)
{
  (void)action;
  (void)param;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return;

  GtkBuilder *builder = gtk_builder_new_from_resource("/org/gnostr/ui/ui/dialogs/gnostr-about-dialog.ui");
  if (!builder) {
    gnostr_main_window_show_toast(GTK_WIDGET(self), "About dialog UI missing");
    return;
  }

  GtkWindow *win = GTK_WINDOW(gtk_builder_get_object(builder, "about_window"));
  if (!win) {
    g_object_unref(builder);
    gnostr_main_window_show_toast(GTK_WIDGET(self), "About window missing");
    return;
  }

  gtk_window_set_transient_for(win, GTK_WINDOW(self));
  gtk_window_set_modal(win, TRUE);
  g_object_set_data_full(G_OBJECT(win), "builder", builder, g_object_unref);
  gtk_window_present(win);
}

void
gnostr_main_window_on_show_preferences_activated_internal(GSimpleAction *action,
                                                          GVariant *param,
                                                          gpointer user_data)
{
  (void)action;
  (void)param;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return;

  gnostr_main_window_on_settings_clicked_internal(NULL, self);
}

void
gnostr_main_window_install_actions_internal(GnostrMainWindow *self)
{
  g_return_if_fail(GNOSTR_IS_MAIN_WINDOW(self));

  GSimpleAction *about_action = g_simple_action_new("show-about", NULL);
  g_signal_connect(about_action, "activate",
                   G_CALLBACK(gnostr_main_window_on_show_about_activated_internal), self);
  g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(about_action));
  g_object_unref(about_action);

  GSimpleAction *prefs_action = g_simple_action_new("show-preferences", NULL);
  g_signal_connect(prefs_action, "activate",
                   G_CALLBACK(gnostr_main_window_on_show_preferences_activated_internal), self);
  g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(prefs_action));
  g_object_unref(prefs_action);
}
