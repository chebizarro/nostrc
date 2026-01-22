/* sheet-relay-config.h - Relay configuration dialog */
#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define TYPE_SHEET_RELAY_CONFIG (sheet_relay_config_get_type())
G_DECLARE_FINAL_TYPE(SheetRelayConfig, sheet_relay_config, SHEET, RELAY_CONFIG, AdwDialog)

/* Callback invoked when relay list is saved */
typedef void (*SheetRelayConfigSaveCb)(const gchar *event_json, gpointer user_data);

/* Create a new relay config dialog */
SheetRelayConfig *sheet_relay_config_new(void);

/* Set callback for publish action */
void sheet_relay_config_set_on_publish(SheetRelayConfig *self,
                                       SheetRelayConfigSaveCb cb,
                                       gpointer user_data);

G_END_DECLS
