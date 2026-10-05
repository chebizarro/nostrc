#include "sheet-orbot-setup.h"
#include "../app-resources.h"

struct _SheetOrbotSetup { AdwDialog parent_instance; GtkButton *btn_cancel; GtkButton *btn_save; GtkEntry *entry_proxy; };
G_DEFINE_TYPE(SheetOrbotSetup, sheet_orbot_setup, ADW_TYPE_DIALOG)

static void sheet_orbot_setup_class_init(SheetOrbotSetupClass *klass){
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/sheets/sheet-orbot-setup.ui");
  gtk_widget_class_bind_template_child(wc, SheetOrbotSetup, btn_cancel);
  gtk_widget_class_bind_template_child(wc, SheetOrbotSetup, btn_save);
  gtk_widget_class_bind_template_child(wc, SheetOrbotSetup, entry_proxy);
}
static void on_cancel(GtkButton *b, gpointer user_data){ (void)b; SheetOrbotSetup *self = user_data; if (self) adw_dialog_close(ADW_DIALOG(self)); }
static void on_entry_activate(GtkEntry *e, gpointer user_data){ (void)e; SheetOrbotSetup *self = user_data; if (self && self->btn_save) gtk_widget_activate(GTK_WIDGET(self->btn_save)); }
static void sheet_orbot_setup_init(SheetOrbotSetup *self){
  gtk_widget_init_template(GTK_WIDGET(self));
  if (self->btn_cancel) g_signal_connect(self->btn_cancel, "clicked", G_CALLBACK(on_cancel), self);
  if (self->entry_proxy) {
    g_signal_connect(self->entry_proxy, "activate", G_CALLBACK(on_entry_activate), self);
    gtk_widget_grab_focus(GTK_WIDGET(self->entry_proxy));
  }
}

SheetOrbotSetup *sheet_orbot_setup_new(void){ return g_object_new(TYPE_SHEET_ORBOT_SETUP, NULL); }
