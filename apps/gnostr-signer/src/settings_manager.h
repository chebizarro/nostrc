/* settings_manager.h - GSettings wrapper for gnostr-signer
 *
 * Provides a type-safe API for accessing GSettings with proper defaults
 * and change notifications.
 */
#ifndef APPS_GNOSTR_SIGNER_SETTINGS_MANAGER_H
#define APPS_GNOSTR_SIGNER_SETTINGS_MANAGER_H

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define GNOSTR_SIGNER_SCHEMA_ID "org.gnostr.Signer"

typedef struct _SettingsManager SettingsManager;

/* Theme options */
typedef enum {
  SETTINGS_THEME_SYSTEM,
  SETTINGS_THEME_LIGHT,
  SETTINGS_THEME_DARK,
  SETTINGS_THEME_HIGH_CONTRAST
} SettingsTheme;

/* High contrast color scheme variants */
typedef enum {
  SETTINGS_HC_DEFAULT,        /* Black on White */
  SETTINGS_HC_INVERTED,       /* White on Black */
  SETTINGS_HC_YELLOW_ON_BLACK /* Yellow on Black */
} SettingsHighContrastVariant;

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

SettingsHighContrastVariant settings_manager_get_high_contrast_variant(SettingsManager *sm);
void settings_manager_set_high_contrast_variant(SettingsManager *sm, SettingsHighContrastVariant variant);

gboolean settings_manager_get_force_high_contrast(SettingsManager *sm);
void settings_manager_set_force_high_contrast(SettingsManager *sm, gboolean force);

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

/* Hardware Keystore Settings */

/**
 * HardwareKeystoreMode:
 * @HW_KS_MODE_DISABLED: Hardware keystore is disabled
 * @HW_KS_MODE_HARDWARE: Use hardware only (fail if unavailable)
 * @HW_KS_MODE_FALLBACK: Allow software fallback
 * @HW_KS_MODE_AUTO: Automatically choose best available
 */
typedef enum {
  HW_KS_MODE_DISABLED = 0,
  HW_KS_MODE_HARDWARE = 1,
  HW_KS_MODE_FALLBACK = 2,
  HW_KS_MODE_AUTO = 3
} HardwareKeystoreMode;

/**
 * settings_manager_get_hardware_keystore_enabled:
 * @sm: A #SettingsManager
 *
 * Gets whether hardware keystore is enabled.
 *
 * Returns: %TRUE if hardware keystore is enabled
 */
gboolean settings_manager_get_hardware_keystore_enabled(SettingsManager *sm);

/**
 * settings_manager_set_hardware_keystore_enabled:
 * @sm: A #SettingsManager
 * @enabled: Whether to enable hardware keystore
 *
 * Enables or disables hardware keystore.
 */
void settings_manager_set_hardware_keystore_enabled(SettingsManager *sm, gboolean enabled);

/**
 * settings_manager_get_hardware_keystore_mode:
 * @sm: A #SettingsManager
 *
 * Gets the hardware keystore mode.
 *
 * Returns: The current #HardwareKeystoreMode
 */
HardwareKeystoreMode settings_manager_get_hardware_keystore_mode(SettingsManager *sm);

/**
 * settings_manager_set_hardware_keystore_mode:
 * @sm: A #SettingsManager
 * @mode: The mode to set
 *
 * Sets the hardware keystore mode.
 */
void settings_manager_set_hardware_keystore_mode(SettingsManager *sm, HardwareKeystoreMode mode);

/**
 * settings_manager_get_hardware_keystore_fallback:
 * @sm: A #SettingsManager
 *
 * Gets whether software fallback is allowed for hardware keystore.
 *
 * Returns: %TRUE if fallback is allowed
 */
gboolean settings_manager_get_hardware_keystore_fallback(SettingsManager *sm);

/**
 * settings_manager_set_hardware_keystore_fallback:
 * @sm: A #SettingsManager
 * @fallback: Whether to allow software fallback
 *
 * Sets whether software fallback is allowed for hardware keystore.
 */
void settings_manager_set_hardware_keystore_fallback(SettingsManager *sm, gboolean fallback);

/**
 * settings_manager_get_hardware_keystore_identities:
 * @sm: A #SettingsManager
 *
 * Gets the list of identity npubs that use hardware-backed keys.
 *
 * Returns: (transfer full): Array of npub strings. Free with g_strfreev().
 */
gchar **settings_manager_get_hardware_keystore_identities(SettingsManager *sm);

/**
 * settings_manager_set_hardware_keystore_identities:
 * @sm: A #SettingsManager
 * @npubs: Array of npub strings
 *
 * Sets the list of identity npubs that use hardware-backed keys.
 */
void settings_manager_set_hardware_keystore_identities(SettingsManager *sm, const gchar *const *npubs);

/**
 * settings_manager_add_hardware_keystore_identity:
 * @sm: A #SettingsManager
 * @npub: The npub to add
 *
 * Adds an identity to the hardware keystore list.
 *
 * Returns: %TRUE if added, %FALSE if already present
 */
gboolean settings_manager_add_hardware_keystore_identity(SettingsManager *sm, const gchar *npub);

/**
 * settings_manager_remove_hardware_keystore_identity:
 * @sm: A #SettingsManager
 * @npub: The npub to remove
 *
 * Removes an identity from the hardware keystore list.
 *
 * Returns: %TRUE if removed, %FALSE if not found
 */
gboolean settings_manager_remove_hardware_keystore_identity(SettingsManager *sm, const gchar *npub);

/**
 * settings_manager_is_hardware_keystore_identity:
 * @sm: A #SettingsManager
 * @npub: The npub to check
 *
 * Checks if an identity uses hardware-backed keys.
 *
 * Returns: %TRUE if the identity uses hardware keystore
 */
gboolean settings_manager_is_hardware_keystore_identity(SettingsManager *sm, const gchar *npub);

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

/**
 * settings_manager_preload_startup_settings:
 * @sm: A #SettingsManager
 *
 * Preload commonly-used startup settings into cache to reduce
 * D-Bus round trips during application startup. This batches
 * multiple GSettings reads into a single operation.
 *
 * Call this once early in the startup sequence, ideally right
 * after creating the SettingsManager.
 */
void settings_manager_preload_startup_settings(SettingsManager *sm);

/* ============================================================================
 * Internationalization Settings
 * ============================================================================ */

/**
 * settings_manager_get_language:
 * @sm: A #SettingsManager
 *
 * Gets the user's preferred language code.
 *
 * Returns: (transfer full): Language code (e.g., "ja", "es") or NULL for system default.
 *          Free with g_free().
 */
gchar *settings_manager_get_language(SettingsManager *sm);

/**
 * settings_manager_set_language:
 * @sm: A #SettingsManager
 * @lang: Language code (e.g., "ja", "es") or NULL for system default
 *
 * Sets the user's preferred language.
 */
void settings_manager_set_language(SettingsManager *sm, const gchar *lang);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_SETTINGS_MANAGER_H */
