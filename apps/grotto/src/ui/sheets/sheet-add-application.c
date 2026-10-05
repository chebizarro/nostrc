#include "sheet-add-application.h"
#include "../app-resources.h"

struct _SheetAddApplication { AdwDialog parent_instance; GtkButton *btn_cancel; GtkButton *btn_paste; };
G_DEFINE_TYPE(SheetAddApplication, sheet_add_application, ADW_TYPE_DIALOG)

static void sheet_add_application_class_init(SheetAddApplicationClass *klass){
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/sheets/sheet-add-application.ui");
  gtk_widget_class_bind_template_child(wc, SheetAddApplication, btn_cancel);
  gtk_widget_class_bind_template_child(wc, SheetAddApplication, btn_paste);
}
static void on_cancel(GtkButton *b, gpointer user_data){ (void)b; SheetAddApplication *self = user_data; if (self) adw_dialog_close(ADW_DIALOG(self)); }
static void sheet_add_application_init(SheetAddApplication *self){
  gtk_widget_init_template(GTK_WIDGET(self));
  if (self->btn_cancel) g_signal_connect(self->btn_cancel, "clicked", G_CALLBACK(on_cancel), self);
  if (self->btn_paste) gtk_widget_grab_focus(GTK_WIDGET(self->btn_paste));
}

SheetAddApplication *sheet_add_application_new(void){ return g_object_new(TYPE_SHEET_ADD_APPLICATION, NULL); }
