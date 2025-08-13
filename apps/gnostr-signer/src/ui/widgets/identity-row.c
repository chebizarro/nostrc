#include "identity-row.h"
#include "../app-resources.h"

struct _IdentityRow { AdwActionRow parent_instance; };
G_DEFINE_TYPE(IdentityRow, identity_row, ADW_TYPE_ACTION_ROW)

static void identity_row_class_init(IdentityRowClass *klass){
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/widgets/identity-row.ui");
}
static void identity_row_init(IdentityRow *self){ gtk_widget_init_template(GTK_WIDGET(self)); }

GtkWidget *identity_row_new(void){ return g_object_new(TYPE_IDENTITY_ROW, NULL); }
