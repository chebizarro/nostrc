/* gh-test-escape.h — pressing Escape in a test window. Header-only.
 *
 * GTK 4 has no public way to synthesize a key event. Escape reaches the
 * focused widget first and bubbles to its ancestors; this walks the same
 * chain and runs the first Escape shortcut found in a widget's shortcut
 * controllers (its class bindings, such as a panel's Escape, and its own
 * controllers) that handles it. It fails the test when nothing does. */
#ifndef GH_TEST_ESCAPE_H
#define GH_TEST_ESCAPE_H

#include <gtk/gtk.h>

static G_GNUC_UNUSED gboolean
gh_test_escape_on(GtkWidget *widget)
{
  g_autoptr(GtkShortcutTrigger) escape = gtk_keyval_trigger_new(GDK_KEY_Escape, 0);
  g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(widget);
  for (guint i = 0; i < g_list_model_get_n_items(controllers); i++) {
    g_autoptr(GObject) controller = g_list_model_get_item(controllers, i);
    if (!GTK_IS_SHORTCUT_CONTROLLER(controller))
      continue;
    GListModel *shortcuts = G_LIST_MODEL(controller);
    for (guint j = 0; j < g_list_model_get_n_items(shortcuts); j++) {
      g_autoptr(GtkShortcut) shortcut = g_list_model_get_item(shortcuts, j);
      GtkShortcutTrigger *trigger = gtk_shortcut_get_trigger(shortcut);
      GtkShortcutAction *action = gtk_shortcut_get_action(shortcut);
      if (trigger && action && gtk_shortcut_trigger_equal(trigger, escape) &&
          gtk_shortcut_action_activate(action, 0, widget, gtk_shortcut_get_arguments(shortcut)))
        return TRUE;
    }
  }
  return FALSE;
}

/* Presses Escape where the keyboard focus of window is. */
static G_GNUC_UNUSED void
gh_test_press_escape(GtkWindow *window)
{
  for (GtkWidget *w = gtk_window_get_focus(window); w; w = gtk_widget_get_parent(w))
    if (gh_test_escape_on(w))
      return;
  g_assert_not_reached();
}

#endif
