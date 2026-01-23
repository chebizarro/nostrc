/* sheet-relay-config.h - Relay configuration dialog
 *
 * Supports per-identity relay lists (nostrc-5ju).
 */
#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define TYPE_SHEET_RELAY_CONFIG (sheet_relay_config_get_type())
G_DECLARE_FINAL_TYPE(SheetRelayConfig, sheet_relay_config, SHEET, RELAY_CONFIG, AdwDialog)

/* Callback invoked when relay list is saved.
 * @event_json: NIP-65 event JSON for publishing
 * @identity: npub of the identity (NULL for global)
 * @user_data: user data passed to the callback
 */
typedef void (*SheetRelayConfigSaveCb)(const gchar *event_json,
                                       const gchar *identity,
                                       gpointer user_data);

/* Create a new relay config dialog (global relays) */
SheetRelayConfig *sheet_relay_config_new(void);

/* Create a new relay config dialog for a specific identity (npub).
 * If identity is NULL, shows global relay configuration.
 * The dialog title will include the identity's display name if available.
 */
SheetRelayConfig *sheet_relay_config_new_for_identity(const gchar *identity);

/* Set callback for publish action */
void sheet_relay_config_set_on_publish(SheetRelayConfig *self,
                                       SheetRelayConfigSaveCb cb,
                                       gpointer user_data);

/* Get the identity associated with this dialog (NULL for global) */
const gchar *sheet_relay_config_get_identity(SheetRelayConfig *self);

G_END_DECLS
