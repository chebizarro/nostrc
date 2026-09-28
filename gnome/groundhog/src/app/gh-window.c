#include "gh-window.h"

struct _GhWindow {
  AdwApplicationWindow parent_instance;
  AdwToastOverlay *toasts;
  AdwNavigationSplitView *split;
  GhSidebarPage *sidebar;
  GhContentPage *content;
  GhStatus *status;
  /* Whether the next selection change should open the conversation while
   * collapsed: set by a pointer or touch press on the list and by the
   * previous/next actions, cleared by any key press in the list and once
   * used. Arrow keys only move the selection; Enter opens (activate). */
  gboolean open_on_select;
};

G_DEFINE_FINAL_TYPE(GhWindow, gh_window, ADW_TYPE_APPLICATION_WINDOW)

/* The one table of application accelerators; gh-shortcuts-window.blp lists
 * the same keys (tests/app/test_shell_layout.c checks both directions). */
static const struct {
  const gchar *action;
  const gchar *accels[3];
} shell_accels[] = {
  { "win.search", { "<Control>f", NULL } },
  { "win.previous-conversation", { "<Alt>Up", "<Control>Page_Up", NULL } },
  { "win.next-conversation", { "<Alt>Down", "<Control>Page_Down", NULL } },
  { "win.new-message", { "<Control>n", NULL } },
  { "win.show-help-overlay", { "<Control>question", NULL } },
  { "window.close", { "<Control>w", NULL } },
  { "app.quit", { "<Control>q", NULL } },
};

static void
on_search(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  GhWindow *self = data;
  (void)action;
  (void)parameter;
  /* The search lives in the sidebar; bring it on screen first. */
  if (adw_navigation_split_view_get_collapsed(self->split))
    adw_navigation_split_view_set_show_content(self->split, FALSE);
  gh_sidebar_page_start_search(self->sidebar);
}

static void
select_relative(GhWindow *self, gint delta)
{
  self->open_on_select = TRUE;
  gh_sidebar_page_select_relative(self->sidebar, delta);
  self->open_on_select = FALSE;
}

static void
on_previous(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  (void)action;
  (void)parameter;
  select_relative(GH_WINDOW(data), -1);
}

static void
on_next(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  (void)action;
  (void)parameter;
  select_relative(GH_WINDOW(data), 1);
}

static const GActionEntry window_actions[] = {
  { "search", on_search, NULL, NULL, NULL, { 0 } },
  { "previous-conversation", on_previous, NULL, NULL, NULL, { 0 } },
  { "next-conversation", on_next, NULL, NULL, NULL, { 0 } },
  /* Disabled below until starting a conversation exists (charter G18). */
  { "new-message", NULL, NULL, NULL, NULL, { 0 } },
};

/* The content page follows the selection (charter §7.12), except that while
 * collapsed a keyboard user moving through the list with the arrow keys is
 * not pushed into each conversation: only a press, the previous/next actions
 * or activation (on_activate) open one there. */
static void
on_selected(GhWindow *self)
{
  gboolean selected = gh_sidebar_page_get_selected(self->sidebar) != NULL;
  gboolean open = self->open_on_select;
  self->open_on_select = FALSE;
  if (!selected)
    adw_navigation_split_view_set_show_content(self->split, FALSE);
  else if (!adw_navigation_split_view_get_collapsed(self->split) || open)
    adw_navigation_split_view_set_show_content(self->split, TRUE);
}

/* Collapsing with a conversation selected keeps it on screen. */
static void
on_collapsed(GhWindow *self)
{
  adw_navigation_split_view_set_show_content(self->split,
                                             gh_sidebar_page_get_selected(self->sidebar) != NULL);
}

static void
on_list_pressed(GhWindow *self)
{
  self->open_on_select = TRUE;
}

static gboolean
on_list_key(GhWindow *self)
{
  self->open_on_select = FALSE;
  return GDK_EVENT_PROPAGATE;
}

/* Back to the list while collapsed: clear the selection so that choosing the
 * same conversation again is a change that reopens it. */
static void
on_show_content(GhWindow *self)
{
  if (adw_navigation_split_view_get_collapsed(self->split) &&
      !adw_navigation_split_view_get_show_content(self->split))
    gh_sidebar_page_unselect(self->sidebar);
}

/* Enter or a double click on a row: open it and move into the messages. */
static void
on_activate(GhWindow *self, guint position)
{
  (void)position;
  if (!gh_sidebar_page_get_selected(self->sidebar))
    return;
  adw_navigation_split_view_set_show_content(self->split, TRUE);
  gtk_widget_grab_focus(GTK_WIDGET(gh_content_page_get_message_list(self->content)));
}

static void
gh_window_dispose(GObject *object)
{
  GhWindow *self = GH_WINDOW(object);
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_WINDOW);
  g_clear_object(&self->status);
  G_OBJECT_CLASS(gh_window_parent_class)->dispose(object);
}

static void
gh_window_class_init(GhWindowClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  G_OBJECT_CLASS(klass)->dispose = gh_window_dispose;
  /* The template instantiates both page types by name. */
  g_type_ensure(GH_TYPE_SIDEBAR_PAGE);
  g_type_ensure(GH_TYPE_CONTENT_PAGE);
  gtk_widget_class_set_template_from_resource(widget_class,
                                              "/org/nostr/Groundhog/ui/gh-window.ui");
  gtk_widget_class_bind_template_child(widget_class, GhWindow, toasts);
  gtk_widget_class_bind_template_child(widget_class, GhWindow, split);
  gtk_widget_class_bind_template_child(widget_class, GhWindow, sidebar);
  gtk_widget_class_bind_template_child(widget_class, GhWindow, content);
}

static void
gh_window_init(GhWindow *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));

  self->status = gh_status_new();
  gh_sidebar_page_set_status(self->sidebar, self->status);
  gh_sidebar_page_set_key_capture_widget(self->sidebar, GTK_WIDGET(self));

  g_action_map_add_action_entries(G_ACTION_MAP(self), window_actions,
                                  G_N_ELEMENTS(window_actions), self);
  g_simple_action_set_enabled(
    G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(self), "new-message")), FALSE);

  g_autoptr(GtkBuilder) builder =
    gtk_builder_new_from_resource("/org/nostr/Groundhog/ui/gh-shortcuts-window.ui");
  gtk_application_window_set_help_overlay(
    GTK_APPLICATION_WINDOW(self),
    GTK_SHORTCUTS_WINDOW(gtk_builder_get_object(builder, "help_overlay")));

  g_signal_connect_object(self->sidebar, "notify::selected", G_CALLBACK(on_selected), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(self->split, "notify::show-content", G_CALLBACK(on_show_content),
                          self, G_CONNECT_SWAPPED);
  g_signal_connect_object(self->split, "notify::collapsed", G_CALLBACK(on_collapsed), self,
                          G_CONNECT_SWAPPED);
  GtkWidget *list = GTK_WIDGET(gh_sidebar_page_get_list(self->sidebar));
  g_signal_connect_object(list, "activate", G_CALLBACK(on_activate), self, G_CONNECT_SWAPPED);
  /* Capture phase: seen before the row selects (on release) or the list
   * moves its cursor. Neither claims the event. */
  GtkGesture *press = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(press), 0);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(press), GTK_PHASE_CAPTURE);
  g_signal_connect_object(press, "pressed", G_CALLBACK(on_list_pressed), self,
                          G_CONNECT_SWAPPED);
  gtk_widget_add_controller(list, GTK_EVENT_CONTROLLER(press));
  GtkEventController *keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect_object(keys, "key-pressed", G_CALLBACK(on_list_key), self,
                          G_CONNECT_SWAPPED);
  gtk_widget_add_controller(list, keys);
}

GhWindow *
gh_window_new(GtkApplication *app)
{
  return g_object_new(GH_TYPE_WINDOW, "application", app, NULL);
}

AdwToastOverlay *
gh_window_get_toasts(GhWindow *self)
{
  g_return_val_if_fail(GH_IS_WINDOW(self), NULL);
  return self->toasts;
}

AdwNavigationSplitView *
gh_window_get_split(GhWindow *self)
{
  g_return_val_if_fail(GH_IS_WINDOW(self), NULL);
  return self->split;
}

GhSidebarPage *
gh_window_get_sidebar(GhWindow *self)
{
  g_return_val_if_fail(GH_IS_WINDOW(self), NULL);
  return self->sidebar;
}

GhContentPage *
gh_window_get_content(GhWindow *self)
{
  g_return_val_if_fail(GH_IS_WINDOW(self), NULL);
  return self->content;
}

GhStatus *
gh_window_get_status(GhWindow *self)
{
  g_return_val_if_fail(GH_IS_WINDOW(self), NULL);
  return self->status;
}

gboolean
gh_window_get_content_visible(GhWindow *self)
{
  g_return_val_if_fail(GH_IS_WINDOW(self), FALSE);
  return !adw_navigation_split_view_get_collapsed(self->split) ||
         adw_navigation_split_view_get_show_content(self->split);
}

static void
on_quit(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  (void)action;
  (void)parameter;
  g_application_quit(G_APPLICATION(data));
}

void
gh_window_setup_application(GtkApplication *app)
{
  g_return_if_fail(GTK_IS_APPLICATION(app));
  const GActionEntry app_actions[] = {
    { "quit", on_quit, NULL, NULL, NULL, { 0 } },
  };
  g_action_map_add_action_entries(G_ACTION_MAP(app), app_actions, G_N_ELEMENTS(app_actions),
                                  app);
  for (guint i = 0; i < G_N_ELEMENTS(shell_accels); i++)
    gtk_application_set_accels_for_action(app, shell_accels[i].action,
                                          shell_accels[i].accels);
}
