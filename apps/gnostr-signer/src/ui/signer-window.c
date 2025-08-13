#include "signer-window.h"
#include "app-resources.h"
#include "page-permissions.h"
#include "page-applications.h"
#include "page-settings.h"

struct _SignerWindow {
  AdwApplicationWindow parent_instance;
  /* Template children */
  AdwToolbarView *toolbar_view;
  AdwHeaderBar *header_bar;
  AdwViewSwitcherTitle *switcher_title;
  AdwNavigationSplitView *split_view;
  GtkMenuButton *menu_btn;
  GtkListBox *sidebar;
  AdwViewStack *stack;
  /* pages */
  GtkWidget *page_permissions;
  GtkWidget *page_applications;
  GtkWidget *page_settings;
};

G_DEFINE_TYPE(SignerWindow, signer_window, ADW_TYPE_APPLICATION_WINDOW)

static void on_sidebar_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
  (void)box;
  SignerWindow *self = user_data;
  if (!self || !self->stack || !row) return;
  int idx = gtk_list_box_row_get_index(row);
  const char *names[] = { "permissions", "applications", "settings" };
  if (idx >= 0 && idx < 3) adw_view_stack_set_visible_child_name(self->stack, names[idx]);
}

static void signer_window_class_init(SignerWindowClass *klass) {
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  /* Ensure custom page types are registered before template instantiation */
  g_type_ensure(TYPE_PAGE_PERMISSIONS);
  g_type_ensure(TYPE_PAGE_APPLICATIONS);
  g_type_ensure(TYPE_PAGE_SETTINGS);
  gtk_widget_class_set_template_from_resource(widget_class, APP_RESOURCE_PATH "/ui/signer-window.ui");
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, toolbar_view);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, header_bar);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, switcher_title);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, split_view);
   gtk_widget_class_bind_template_child(widget_class, SignerWindow, menu_btn);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, sidebar);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, stack);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, page_permissions);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, page_applications);
  gtk_widget_class_bind_template_child(widget_class, SignerWindow, page_settings);
}

static void signer_window_init(SignerWindow *self) {
  gtk_widget_init_template(GTK_WIDGET(self));
  /* App menu */
  if (self->menu_btn) {
    GMenu *menu = g_menu_new();
    g_menu_append(menu, "Preferences", "app.preferences");
    g_menu_append(menu, "About GNostr Signer", "app.about");
    g_menu_append(menu, "Quit", "app.quit");
    gtk_menu_button_set_menu_model(self->menu_btn, G_MENU_MODEL(menu));
    g_object_unref(menu);
  }
  g_signal_connect(self->sidebar, "row-activated", G_CALLBACK(on_sidebar_row_activated), self);
  /* Select first row and show default page */
  GtkListBoxRow *first = gtk_list_box_get_row_at_index(self->sidebar, 0);
  if (first) gtk_list_box_select_row(self->sidebar, first);
  if (self->stack) adw_view_stack_set_visible_child_name(self->stack, "permissions");
}

SignerWindow *signer_window_new(AdwApplication *app) {
  g_return_val_if_fail(ADW_IS_APPLICATION(app), NULL);
  return g_object_new(TYPE_SIGNER_WINDOW, "application", app, NULL);
}

void signer_window_show_page(SignerWindow *self, const char *name) {
  g_return_if_fail(self != NULL && name != NULL);
  if (self->stack) adw_view_stack_set_visible_child_name(self->stack, name);
}
