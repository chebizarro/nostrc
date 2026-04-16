#define G_LOG_DOMAIN "gnostr-main-window-actions"

#include "gnostr-main-window-private.h"

#include "gnostr-session-view.h"
#include "gnostr-filter-switcher.h"

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

/* win.activate-filter-set(int32): activates the filter set at the given
 * 1-based slot via the filter-switcher widget. Hooked to
 * <Primary>1…<Primary>9 accelerators in main_app.c so users can quickly
 * jump between their favourite filter sets.
 * nostrc-yg8j.5 */
static void
on_activate_filter_set_action(GSimpleAction *action,
                               GVariant *param,
                               gpointer user_data)
{
  (void)action;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !param)
    return;
  if (!g_variant_is_of_type(param, G_VARIANT_TYPE_INT32))
    return;
  if (!self->session_view)
    return;

  gint32 position = g_variant_get_int32(param);
  if (position < 1)
    return;

  GtkWidget *switcher = gnostr_session_view_get_filter_switcher(self->session_view);
  if (!switcher || !GNOSTR_IS_FILTER_SWITCHER(switcher))
    return;

  gnostr_filter_switcher_activate_position(GNOSTR_FILTER_SWITCHER(switcher),
                                            (guint)position);
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

  /* Parameterised filter-set activation. The i parameter is a 1-based
   * slot; per-slot accelerators are registered on the application in
   * main_app.c using action target detail strings ("win.activate-filter-set(1)"
   * etc.). nostrc-yg8j.5 */
  GSimpleAction *activate_fs = g_simple_action_new("activate-filter-set",
                                                    G_VARIANT_TYPE_INT32);
  g_signal_connect(activate_fs, "activate",
                   G_CALLBACK(on_activate_filter_set_action), self);
  g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(activate_fs));
  g_object_unref(activate_fs);
}
