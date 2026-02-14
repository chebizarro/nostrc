#ifndef APPS_GNOSTR_SIGNER_UI_WIDGETS_APP_ROW_H
#define APPS_GNOSTR_SIGNER_UI_WIDGETS_APP_ROW_H
#include <adwaita.h>
G_BEGIN_DECLS
#define TYPE_APP_ROW (app_row_get_type())
G_DECLARE_FINAL_TYPE(AppRow, app_row, APP, ROW, AdwActionRow)
GtkWidget *app_row_new(void);
G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_UI_WIDGETS_APP_ROW_H */
