#include "page-permissions.h"
#include "../signer_grants.h"
#include "../startup-timing.h"
#include "app-resources.h"

struct _PagePermissions {
  AdwPreferencesPage parent_instance;
  GtkListBox *list_grants;
  GtkButton *btn_refresh_grants;
  AdwPreferencesGroup *group_grants, *group_policy, *group_conn, *pending_group;
};

G_DEFINE_TYPE(PagePermissions, page_permissions, ADW_TYPE_PREFERENCES_PAGE)

static void page_permissions_class_init(PagePermissionsClass *klass) {
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/page-permissions.ui");
  gtk_widget_class_bind_template_child(wc, PagePermissions, list_grants);
  gtk_widget_class_bind_template_child(wc, PagePermissions, btn_refresh_grants);
  gtk_widget_class_bind_template_child(wc, PagePermissions, group_grants);
  gtk_widget_class_bind_template_child(wc, PagePermissions, group_policy);
  gtk_widget_class_bind_template_child(wc, PagePermissions, group_conn);
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

void page_permissions_set_pending_group(PagePermissions *self, GtkWidget *group) {
  g_return_if_fail(ADW_IS_PREFERENCES_GROUP(group));
  if (self->pending_group)
    return;
  /* Keep the existing default page intact, with the reusable inbox first. */
  AdwPreferencesGroup *sections[] = {self->group_grants, self->group_policy, self->group_conn};
  for (guint i = 0; i < G_N_ELEMENTS(sections); i++) {
    g_object_ref(sections[i]);
    adw_preferences_page_remove(ADW_PREFERENCES_PAGE(self), sections[i]);
  }
  self->pending_group = ADW_PREFERENCES_GROUP(group);
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(self), self->pending_group);
  for (guint i = 0; i < G_N_ELEMENTS(sections); i++) {
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(self), sections[i]);
    g_object_unref(sections[i]);
  }
}
PagePermissions *page_permissions_new(void) {
  return g_object_new(TYPE_PAGE_PERMISSIONS, NULL);
}
