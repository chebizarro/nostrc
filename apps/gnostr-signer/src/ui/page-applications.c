/* Applications page controller: wires actions to open sheets */
#include "page-applications.h"
#include "app-resources.h"
#include "sheets/sheet-add-application.h"
#include "sheets/sheet-create-bunker.h"

struct _PageApplications {
  AdwPreferencesPage parent_instance;
  GtkButton *btn_add_app;
  GtkButton *btn_add_bunker;
};

G_DEFINE_TYPE(PageApplications, page_applications, ADW_TYPE_PREFERENCES_PAGE)

static GtkWindow *get_parent_window(GtkWidget *w){ GtkRoot *r = gtk_widget_get_root(w); return r ? GTK_WINDOW(r) : NULL; }

static void on_add_app(GtkButton *b, gpointer user_data){
  (void)b; PageApplications *self = user_data; if (!self) return;
  SheetAddApplication *dlg = sheet_add_application_new();
  adw_dialog_present(ADW_DIALOG(dlg), GTK_WIDGET(get_parent_window(GTK_WIDGET(self))));
}
static void on_add_bunker(GtkButton *b, gpointer user_data){
  (void)b; PageApplications *self = user_data; if (!self) return;
  SheetCreateBunker *dlg = sheet_create_bunker_new();
  adw_dialog_present(ADW_DIALOG(dlg), GTK_WIDGET(get_parent_window(GTK_WIDGET(self))));
}

static void page_applications_class_init(PageApplicationsClass *klass) {
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/page-applications.ui");
  gtk_widget_class_bind_template_child(wc, PageApplications, btn_add_app);
  gtk_widget_class_bind_template_child(wc, PageApplications, btn_add_bunker);
}

static void page_applications_init(PageApplications *self) {
  gtk_widget_init_template(GTK_WIDGET(self));
  g_signal_connect(self->btn_add_app, "clicked", G_CALLBACK(on_add_app), self);
  g_signal_connect(self->btn_add_bunker, "clicked", G_CALLBACK(on_add_bunker), self);
}

PageApplications *page_applications_new(void) {
  return g_object_new(TYPE_PAGE_APPLICATIONS, NULL);
}
