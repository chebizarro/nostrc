#include "page-permissions.h"
#include "app-resources.h"
#include "../startup-timing.h"
#include "../signer_grants.h"

struct _PagePermissions {
  AdwPreferencesPage parent_instance;
  GtkListBox *list_grants;
  GtkButton *btn_refresh_grants;
};

G_DEFINE_TYPE(PagePermissions, page_permissions, ADW_TYPE_PREFERENCES_PAGE)

static void page_permissions_class_init(PagePermissionsClass *klass) {
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/page-permissions.ui");
  gtk_widget_class_bind_template_child(wc, PagePermissions, list_grants);
  gtk_widget_class_bind_template_child(wc, PagePermissions, btn_refresh_grants);
}

/* The daemon's grants (nostrc-yjky), read each time the page is shown. */
static void refresh_grants(PagePermissions *self) {
  signer_grants_list_box_refresh(self->list_grants);
}

static void page_permissions_init(PagePermissions *self) {
  gint64 init_start = startup_timing_measure_start();
  gtk_widget_init_template(GTK_WIDGET(self));
  g_signal_connect_swapped(self, "map", G_CALLBACK(refresh_grants), self);
  g_signal_connect_swapped(self->btn_refresh_grants, "clicked", G_CALLBACK(refresh_grants), self);
  startup_timing_measure_end(init_start, "page-permissions-init", 30);
}

PagePermissions *page_permissions_new(void) {
  return g_object_new(TYPE_PAGE_PERMISSIONS, NULL);
}
