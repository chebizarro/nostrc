#include "sheet-account-backup.h"
#include "../app-resources.h"

struct _SheetAccountBackup { AdwDialog parent_instance; GtkButton *btn_back; };
G_DEFINE_TYPE(SheetAccountBackup, sheet_account_backup, ADW_TYPE_DIALOG)

static void sheet_account_backup_class_init(SheetAccountBackupClass *klass){
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/sheets/sheet-account-backup.ui");
  gtk_widget_class_bind_template_child(wc, SheetAccountBackup, btn_back);
}
static void on_back(GtkButton *b, gpointer user_data){ (void)b; SheetAccountBackup *self = user_data; if (self) adw_dialog_close(ADW_DIALOG(self)); }
static void sheet_account_backup_init(SheetAccountBackup *self){
  gtk_widget_init_template(GTK_WIDGET(self));
  if (self->btn_back) g_signal_connect(self->btn_back, "clicked", G_CALLBACK(on_back), self);
}

SheetAccountBackup *sheet_account_backup_new(void){ return g_object_new(TYPE_SHEET_ACCOUNT_BACKUP, NULL); }
