#pragma once
#include <adwaita.h>
G_BEGIN_DECLS
#define TYPE_RELAY_ROW (relay_row_get_type())
G_DECLARE_FINAL_TYPE(RelayRow, relay_row, RELAY, ROW, AdwActionRow)
GtkWidget *relay_row_new(void);
G_END_DECLS
