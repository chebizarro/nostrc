#include "gh-window.h"

struct _GhWindow {
  AdwApplicationWindow parent_instance;
  AdwToastOverlay *toasts;
  AdwNavigationSplitView *split;
  GhSidebarPage *sidebar;
  GhContentPage *content;
};

G_DEFINE_FINAL_TYPE(GhWindow, gh_window, ADW_TYPE_APPLICATION_WINDOW)

static void
gh_window_dispose(GObject *object)
{
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_WINDOW);
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
