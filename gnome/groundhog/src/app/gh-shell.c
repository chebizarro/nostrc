#include "gh-shell.h"

struct _GhSidebarPage {
  AdwNavigationPage parent_instance;
  AdwHeaderBar *header;
  AdwWindowTitle *window_title;
  GtkStack *stack;
};

G_DEFINE_FINAL_TYPE(GhSidebarPage, gh_sidebar_page, ADW_TYPE_NAVIGATION_PAGE)

static void
gh_sidebar_page_dispose(GObject *object)
{
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_SIDEBAR_PAGE);
  G_OBJECT_CLASS(gh_sidebar_page_parent_class)->dispose(object);
}

static void
gh_sidebar_page_class_init(GhSidebarPageClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  G_OBJECT_CLASS(klass)->dispose = gh_sidebar_page_dispose;
  gtk_widget_class_set_template_from_resource(widget_class,
                                              "/org/nostr/Groundhog/ui/gh-sidebar-page.ui");
  gtk_widget_class_bind_template_child(widget_class, GhSidebarPage, header);
  gtk_widget_class_bind_template_child(widget_class, GhSidebarPage, window_title);
  gtk_widget_class_bind_template_child(widget_class, GhSidebarPage, stack);
}

static void
gh_sidebar_page_init(GhSidebarPage *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
}

AdwHeaderBar *
gh_sidebar_page_get_header(GhSidebarPage *self)
{
  g_return_val_if_fail(GH_IS_SIDEBAR_PAGE(self), NULL);
  return self->header;
}

AdwWindowTitle *
gh_sidebar_page_get_window_title(GhSidebarPage *self)
{
  g_return_val_if_fail(GH_IS_SIDEBAR_PAGE(self), NULL);
  return self->window_title;
}

GtkStack *
gh_sidebar_page_get_stack(GhSidebarPage *self)
{
  g_return_val_if_fail(GH_IS_SIDEBAR_PAGE(self), NULL);
  return self->stack;
}

void
gh_sidebar_page_show_onboarding(GhSidebarPage *self)
{
  g_return_if_fail(GH_IS_SIDEBAR_PAGE(self));
  g_return_if_fail(gtk_stack_get_child_by_name(self->stack, "onboarding") == NULL);

  g_autoptr(GtkBuilder) builder =
    gtk_builder_new_from_resource("/org/nostr/Groundhog/ui/gh-onboarding-page.ui");
  gtk_stack_add_named(self->stack, GTK_WIDGET(gtk_builder_get_object(builder, "onboarding")),
                      "onboarding");
  gtk_stack_set_visible_child_name(self->stack, "onboarding");
}

struct _GhContentPage {
  AdwNavigationPage parent_instance;
  AdwBanner *banner;
};

G_DEFINE_FINAL_TYPE(GhContentPage, gh_content_page, ADW_TYPE_NAVIGATION_PAGE)

static void
gh_content_page_dispose(GObject *object)
{
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_CONTENT_PAGE);
  G_OBJECT_CLASS(gh_content_page_parent_class)->dispose(object);
}

static void
gh_content_page_class_init(GhContentPageClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  G_OBJECT_CLASS(klass)->dispose = gh_content_page_dispose;
  gtk_widget_class_set_template_from_resource(widget_class,
                                              "/org/nostr/Groundhog/ui/gh-content-page.ui");
  gtk_widget_class_bind_template_child(widget_class, GhContentPage, banner);
}

static void
gh_content_page_init(GhContentPage *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
}

AdwBanner *
gh_content_page_get_banner(GhContentPage *self)
{
  g_return_val_if_fail(GH_IS_CONTENT_PAGE(self), NULL);
  return self->banner;
}
