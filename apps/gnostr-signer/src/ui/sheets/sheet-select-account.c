#include "sheet-select-account.h"
#include "../app-resources.h"

struct _SheetSelectAccount { AdwDialog parent_instance; GtkButton *btn_cancel; GtkListView *list_identities; };
G_DEFINE_TYPE(SheetSelectAccount, sheet_select_account, ADW_TYPE_DIALOG)

static void sheet_select_account_class_init(SheetSelectAccountClass *klass){
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/sheets/sheet-select-account.ui");
  gtk_widget_class_bind_template_child(wc, SheetSelectAccount, btn_cancel);
  gtk_widget_class_bind_template_child(wc, SheetSelectAccount, list_identities);
}
static void on_cancel(GtkButton *b, gpointer user_data){ (void)b; SheetSelectAccount *self = user_data; if (self) adw_dialog_close(ADW_DIALOG(self)); }
static void sheet_select_account_init(SheetSelectAccount *self){
  gtk_widget_init_template(GTK_WIDGET(self));
  if (self->btn_cancel) g_signal_connect(self->btn_cancel, "clicked", G_CALLBACK(on_cancel), self);
  if (self->list_identities) gtk_widget_grab_focus(GTK_WIDGET(self->list_identities));
}

SheetSelectAccount *sheet_select_account_new(void){ return g_object_new(TYPE_SHEET_SELECT_ACCOUNT, NULL); }
