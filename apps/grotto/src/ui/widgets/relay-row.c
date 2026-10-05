#include "relay-row.h"
#include "../app-resources.h"

struct _RelayRow { AdwActionRow parent_instance; };
G_DEFINE_TYPE(RelayRow, relay_row, ADW_TYPE_ACTION_ROW)

static void relay_row_class_init(RelayRowClass *klass){
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/widgets/relay-row.ui");
}
static void relay_row_init(RelayRow *self){ gtk_widget_init_template(GTK_WIDGET(self)); }

GtkWidget *relay_row_new(void){ return g_object_new(TYPE_RELAY_ROW, NULL); }
