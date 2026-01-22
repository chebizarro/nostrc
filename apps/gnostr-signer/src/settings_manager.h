/* settings_manager.h - GSettings wrapper for gnostr-signer
 *
 * Provides a type-safe API for accessing GSettings with proper defaults
 * and change notifications.
 */
#pragma once

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define GNOSTR_SIGNER_SCHEMA_ID "org.gnostr.Signer"

typedef struct _SettingsManager SettingsManager;

/* Theme options */
typedef enum {
  SETTINGS_THEME_SYSTEM,
  SETTINGS_THEME_LIGHT,
  SETTINGS_THEME_DARK
} SettingsTheme;

/* Create a new settings manager */
SettingsManager *settings_manager_new(void);

/* Free the settings manager */
void settings_manager_free(SettingsManager *sm);

/* Get the underlying GSettings object */
GSettings *settings_manager_get_gsettings(SettingsManager *sm);

/* Identity Settings */
const gchar *settings_manager_get_default_identity(SettingsManager *sm);
void settings_manager_set_default_identity(SettingsManager *sm, const gchar *npub);

gchar *settings_manager_get_identity_label(SettingsManager *sm, const gchar *npub);
void settings_manager_set_identity_label(SettingsManager *sm, const gchar *npub, const gchar *label);

/* UI Settings */
SettingsTheme settings_manager_get_theme(SettingsManager *sm);
void settings_manager_set_theme(SettingsManager *sm, SettingsTheme theme);

void settings_manager_get_window_size(SettingsManager *sm, gint *width, gint *height);
void settings_manager_set_window_size(SettingsManager *sm, gint width, gint height);

gboolean settings_manager_get_window_maximized(SettingsManager *sm);
void settings_manager_set_window_maximized(SettingsManager *sm, gboolean maximized);

/* Security Settings */
gint settings_manager_get_lock_timeout(SettingsManager *sm);
void settings_manager_set_lock_timeout(SettingsManager *sm, gint seconds);

gboolean settings_manager_get_remember_approvals(SettingsManager *sm);
void settings_manager_set_remember_approvals(SettingsManager *sm, gboolean remember);

gint settings_manager_get_approval_ttl_hours(SettingsManager *sm);
void settings_manager_set_approval_ttl_hours(SettingsManager *sm, gint hours);

gchar **settings_manager_get_confirmation_kinds(SettingsManager *sm);
void settings_manager_set_confirmation_kinds(SettingsManager *sm, const gchar *const *kinds);

/* Network Settings */
const gchar *settings_manager_get_tor_proxy(SettingsManager *sm);
void settings_manager_set_tor_proxy(SettingsManager *sm, const gchar *proxy_uri);

gboolean settings_manager_get_use_tor(SettingsManager *sm);
void settings_manager_set_use_tor(SettingsManager *sm, gboolean use_tor);

gchar **settings_manager_get_bootstrap_relays(SettingsManager *sm);
void settings_manager_set_bootstrap_relays(SettingsManager *sm, const gchar *const *relays);

/* Bunker Settings */
gboolean settings_manager_get_bunker_enabled(SettingsManager *sm);
void settings_manager_set_bunker_enabled(SettingsManager *sm, gboolean enabled);

gchar **settings_manager_get_bunker_relays(SettingsManager *sm);
void settings_manager_set_bunker_relays(SettingsManager *sm, const gchar *const *relays);

gchar **settings_manager_get_bunker_allowed_pubkeys(SettingsManager *sm);
void settings_manager_set_bunker_allowed_pubkeys(SettingsManager *sm, const gchar *const *pubkeys);

gchar **settings_manager_get_bunker_allowed_methods(SettingsManager *sm);
void settings_manager_set_bunker_allowed_methods(SettingsManager *sm, const gchar *const *methods);

gchar **settings_manager_get_bunker_auto_approve_kinds(SettingsManager *sm);
void settings_manager_set_bunker_auto_approve_kinds(SettingsManager *sm, const gchar *const *kinds);

/* Logging Settings */
gboolean settings_manager_get_log_requests(SettingsManager *sm);
void settings_manager_set_log_requests(SettingsManager *sm, gboolean log);

gint settings_manager_get_log_retention_days(SettingsManager *sm);
void settings_manager_set_log_retention_days(SettingsManager *sm, gint days);

/* Startup Settings */
gboolean settings_manager_get_autostart(SettingsManager *sm);
void settings_manager_set_autostart(SettingsManager *sm, gboolean autostart);

gboolean settings_manager_get_start_minimized(SettingsManager *sm);
void settings_manager_set_start_minimized(SettingsManager *sm, gboolean minimized);

/* Account Settings */
gchar **settings_manager_get_account_order(SettingsManager *sm);
void settings_manager_set_account_order(SettingsManager *sm, const gchar *const *npubs);

/* Change notification callback type */
typedef void (*SettingsChangedCb)(const gchar *key, gpointer user_data);

/* Connect to change notifications */
gulong settings_manager_connect_changed(SettingsManager *sm,
                                        const gchar *key,
                                        SettingsChangedCb cb,
                                        gpointer user_data);

/* Disconnect from change notifications */
void settings_manager_disconnect_changed(SettingsManager *sm, gulong handler_id);

/* Get singleton instance */
SettingsManager *settings_manager_get_default(void);

G_END_DECLS
