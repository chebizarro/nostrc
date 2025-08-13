#include "sheet-create-bunker.h"
#include "../app-resources.h"

struct _SheetCreateBunker { AdwDialog parent_instance; GtkButton *btn_cancel; GtkButton *btn_create; GtkEntry *entry_name; GtkEntry *entry_relay; };
G_DEFINE_TYPE(SheetCreateBunker, sheet_create_bunker, ADW_TYPE_DIALOG)

static void sheet_create_bunker_class_init(SheetCreateBunkerClass *klass){
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/sheets/sheet-create-bunker.ui");
  gtk_widget_class_bind_template_child(wc, SheetCreateBunker, btn_cancel);
  gtk_widget_class_bind_template_child(wc, SheetCreateBunker, btn_create);
  gtk_widget_class_bind_template_child(wc, SheetCreateBunker, entry_name);
  gtk_widget_class_bind_template_child(wc, SheetCreateBunker, entry_relay);
}
static void on_cancel(GtkButton *b, gpointer user_data){ (void)b; SheetCreateBunker *self = user_data; if (self) adw_dialog_close(ADW_DIALOG(self)); }
static void on_entry_activate(GtkEntry *e, gpointer user_data){ (void)e; SheetCreateBunker *self = user_data; if (self && self->btn_create) gtk_widget_activate(GTK_WIDGET(self->btn_create)); }
static void sheet_create_bunker_init(SheetCreateBunker *self){
  gtk_widget_init_template(GTK_WIDGET(self));
  if (self->btn_cancel) g_signal_connect(self->btn_cancel, "clicked", G_CALLBACK(on_cancel), self);
  if (self->entry_name) {
    g_signal_connect(self->entry_name, "activate", G_CALLBACK(on_entry_activate), self);
    gtk_widget_grab_focus(GTK_WIDGET(self->entry_name));
  }
  if (self->entry_relay) g_signal_connect(self->entry_relay, "activate", G_CALLBACK(on_entry_activate), self);
}

SheetCreateBunker *sheet_create_bunker_new(void){ return g_object_new(TYPE_SHEET_CREATE_BUNKER, NULL); }
