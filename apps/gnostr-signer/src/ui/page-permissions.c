#include "page-permissions.h"
#include "app-resources.h"

struct _PagePermissions {
  AdwPreferencesPage parent_instance;
};

G_DEFINE_TYPE(PagePermissions, page_permissions, ADW_TYPE_PREFERENCES_PAGE)

static void page_permissions_class_init(PagePermissionsClass *klass) {
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/page-permissions.ui");
}

static void page_permissions_init(PagePermissions *self) {
  gtk_widget_init_template(GTK_WIDGET(self));
}

PagePermissions *page_permissions_new(void) {
  return g_object_new(TYPE_PAGE_PERMISSIONS, NULL);
}
