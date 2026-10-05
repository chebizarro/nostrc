#include "app-row.h"
#include "../app-resources.h"

struct _AppRow { AdwActionRow parent_instance; };
G_DEFINE_TYPE(AppRow, app_row, ADW_TYPE_ACTION_ROW)

static void app_row_class_init(AppRowClass *klass){
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/widgets/app-row.ui");
}
static void app_row_init(AppRow *self){ gtk_widget_init_template(GTK_WIDGET(self)); }

GtkWidget *app_row_new(void){ return g_object_new(TYPE_APP_ROW, NULL); }
