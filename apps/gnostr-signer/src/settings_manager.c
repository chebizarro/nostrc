/* settings_manager.c - GSettings wrapper implementation */
#include "settings_manager.h"
#include <string.h>

struct _SettingsManager {
  GSettings *settings;
  gchar *default_identity_cache;
  gchar *tor_proxy_cache;
};

/* Singleton instance */
static SettingsManager *default_instance = NULL;

SettingsManager *settings_manager_new(void) {
  SettingsManager *sm = g_new0(SettingsManager, 1);
  sm->settings = g_settings_new(GNOSTR_SIGNER_SCHEMA_ID);
  return sm;
}

void settings_manager_free(SettingsManager *sm) {
  if (!sm) return;
  g_clear_object(&sm->settings);
  g_free(sm->default_identity_cache);
  g_free(sm->tor_proxy_cache);
  g_free(sm);
}

GSettings *settings_manager_get_gsettings(SettingsManager *sm) {
  return sm ? sm->settings : NULL;
}

SettingsManager *settings_manager_get_default(void) {
  if (!default_instance) {
    default_instance = settings_manager_new();
  }
  return default_instance;
}

/* Identity Settings */
const gchar *settings_manager_get_default_identity(SettingsManager *sm) {
  if (!sm) return NULL;
  g_free(sm->default_identity_cache);
  sm->default_identity_cache = g_settings_get_string(sm->settings, "default-identity");
  return sm->default_identity_cache;
}

void settings_manager_set_default_identity(SettingsManager *sm, const gchar *npub) {
  if (!sm) return;
  g_settings_set_string(sm->settings, "default-identity", npub ? npub : "");
}

gchar *settings_manager_get_identity_label(SettingsManager *sm, const gchar *npub) {
  if (!sm || !npub) return NULL;

  GVariant *labels = g_settings_get_value(sm->settings, "identity-labels");
  gchar *label = NULL;

  GVariantIter iter;
  const gchar *key;
  const gchar *value;

  g_variant_iter_init(&iter, labels);
  while (g_variant_iter_next(&iter, "{&s&s}", &key, &value)) {
    if (g_strcmp0(key, npub) == 0) {
      label = g_strdup(value);
      break;
    }
  }

  g_variant_unref(labels);
  return label;
}

void settings_manager_set_identity_label(SettingsManager *sm, const gchar *npub, const gchar *label) {
  if (!sm || !npub) return;

  GVariant *old_labels = g_settings_get_value(sm->settings, "identity-labels");
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("a{ss}"));

  GVariantIter iter;
  const gchar *key;
  const gchar *value;
  gboolean found = FALSE;

  g_variant_iter_init(&iter, old_labels);
  while (g_variant_iter_next(&iter, "{&s&s}", &key, &value)) {
    if (g_strcmp0(key, npub) == 0) {
      if (label && *label) {
        g_variant_builder_add(&builder, "{ss}", key, label);
      }
      /* else: skip to remove */
      found = TRUE;
    } else {
      g_variant_builder_add(&builder, "{ss}", key, value);
    }
  }

  if (!found && label && *label) {
    g_variant_builder_add(&builder, "{ss}", npub, label);
  }

  GVariant *new_labels = g_variant_builder_end(&builder);
  g_settings_set_value(sm->settings, "identity-labels", new_labels);
  g_variant_unref(old_labels);
}

/* UI Settings */
SettingsTheme settings_manager_get_theme(SettingsManager *sm) {
  if (!sm) return SETTINGS_THEME_SYSTEM;

  gchar *theme = g_settings_get_string(sm->settings, "theme");
  SettingsTheme result = SETTINGS_THEME_SYSTEM;

  if (g_strcmp0(theme, "light") == 0) {
    result = SETTINGS_THEME_LIGHT;
  } else if (g_strcmp0(theme, "dark") == 0) {
    result = SETTINGS_THEME_DARK;
  } else if (g_strcmp0(theme, "high-contrast") == 0) {
    result = SETTINGS_THEME_HIGH_CONTRAST;
  }

  g_free(theme);
  return result;
}

void settings_manager_set_theme(SettingsManager *sm, SettingsTheme theme) {
  if (!sm) return;

  const gchar *value;
  switch (theme) {
    case SETTINGS_THEME_LIGHT: value = "light"; break;
    case SETTINGS_THEME_DARK: value = "dark"; break;
    case SETTINGS_THEME_HIGH_CONTRAST: value = "high-contrast"; break;
    default: value = "system"; break;
  }

  g_settings_set_string(sm->settings, "theme", value);
}

SettingsHighContrastVariant settings_manager_get_high_contrast_variant(SettingsManager *sm) {
  if (!sm) return SETTINGS_HC_DEFAULT;

  gchar *variant = g_settings_get_string(sm->settings, "high-contrast-variant");
  SettingsHighContrastVariant result = SETTINGS_HC_DEFAULT;

  if (g_strcmp0(variant, "inverted") == 0) {
    result = SETTINGS_HC_INVERTED;
  } else if (g_strcmp0(variant, "yellow-on-black") == 0) {
    result = SETTINGS_HC_YELLOW_ON_BLACK;
  }

  g_free(variant);
  return result;
}

void settings_manager_set_high_contrast_variant(SettingsManager *sm, SettingsHighContrastVariant variant) {
  if (!sm) return;

  const gchar *value;
  switch (variant) {
    case SETTINGS_HC_INVERTED: value = "inverted"; break;
    case SETTINGS_HC_YELLOW_ON_BLACK: value = "yellow-on-black"; break;
    default: value = "default"; break;
  }

  g_settings_set_string(sm->settings, "high-contrast-variant", value);
}

void settings_manager_get_window_size(SettingsManager *sm, gint *width, gint *height) {
  if (!sm) return;
  if (width) *width = g_settings_get_int(sm->settings, "window-width");
  if (height) *height = g_settings_get_int(sm->settings, "window-height");
}

void settings_manager_set_window_size(SettingsManager *sm, gint width, gint height) {
  if (!sm) return;
  g_settings_set_int(sm->settings, "window-width", width);
  g_settings_set_int(sm->settings, "window-height", height);
}

gboolean settings_manager_get_window_maximized(SettingsManager *sm) {
  return sm ? g_settings_get_boolean(sm->settings, "window-maximized") : FALSE;
}

void settings_manager_set_window_maximized(SettingsManager *sm, gboolean maximized) {
  if (!sm) return;
  g_settings_set_boolean(sm->settings, "window-maximized", maximized);
}

/* Security Settings */
gint settings_manager_get_lock_timeout(SettingsManager *sm) {
  return sm ? g_settings_get_int(sm->settings, "lock-timeout-sec") : 300;
}

void settings_manager_set_lock_timeout(SettingsManager *sm, gint seconds) {
  if (!sm) return;
  g_settings_set_int(sm->settings, "lock-timeout-sec", seconds);
}

gboolean settings_manager_get_remember_approvals(SettingsManager *sm) {
  return sm ? g_settings_get_boolean(sm->settings, "remember-approvals") : FALSE;
}

void settings_manager_set_remember_approvals(SettingsManager *sm, gboolean remember) {
  if (!sm) return;
  g_settings_set_boolean(sm->settings, "remember-approvals", remember);
}

gint settings_manager_get_approval_ttl_hours(SettingsManager *sm) {
  return sm ? g_settings_get_int(sm->settings, "approval-ttl-hours") : 24;
}

void settings_manager_set_approval_ttl_hours(SettingsManager *sm, gint hours) {
  if (!sm) return;
  g_settings_set_int(sm->settings, "approval-ttl-hours", hours);
}

gchar **settings_manager_get_confirmation_kinds(SettingsManager *sm) {
  return sm ? g_settings_get_strv(sm->settings, "require-confirmation-kinds") : NULL;
}

void settings_manager_set_confirmation_kinds(SettingsManager *sm, const gchar *const *kinds) {
  if (!sm) return;
  g_settings_set_strv(sm->settings, "require-confirmation-kinds", kinds);
}

/* Network Settings */
const gchar *settings_manager_get_tor_proxy(SettingsManager *sm) {
  if (!sm) return NULL;
  g_free(sm->tor_proxy_cache);
  sm->tor_proxy_cache = g_settings_get_string(sm->settings, "tor-socks");
  return sm->tor_proxy_cache;
}

void settings_manager_set_tor_proxy(SettingsManager *sm, const gchar *proxy_uri) {
  if (!sm) return;
  g_settings_set_string(sm->settings, "tor-socks", proxy_uri ? proxy_uri : "");
}

gboolean settings_manager_get_use_tor(SettingsManager *sm) {
  return sm ? g_settings_get_boolean(sm->settings, "use-tor-for-relays") : FALSE;
}

void settings_manager_set_use_tor(SettingsManager *sm, gboolean use_tor) {
  if (!sm) return;
  g_settings_set_boolean(sm->settings, "use-tor-for-relays", use_tor);
}

gchar **settings_manager_get_bootstrap_relays(SettingsManager *sm) {
  return sm ? g_settings_get_strv(sm->settings, "bootstrap-relays") : NULL;
}

void settings_manager_set_bootstrap_relays(SettingsManager *sm, const gchar *const *relays) {
  if (!sm) return;
  g_settings_set_strv(sm->settings, "bootstrap-relays", relays);
}

/* Bunker Settings */
gboolean settings_manager_get_bunker_enabled(SettingsManager *sm) {
  return sm ? g_settings_get_boolean(sm->settings, "bunker-enabled") : FALSE;
}

void settings_manager_set_bunker_enabled(SettingsManager *sm, gboolean enabled) {
  if (!sm) return;
  g_settings_set_boolean(sm->settings, "bunker-enabled", enabled);
}

gchar **settings_manager_get_bunker_relays(SettingsManager *sm) {
  return sm ? g_settings_get_strv(sm->settings, "bunker-relays") : NULL;
}

void settings_manager_set_bunker_relays(SettingsManager *sm, const gchar *const *relays) {
  if (!sm) return;
  g_settings_set_strv(sm->settings, "bunker-relays", relays);
}

gchar **settings_manager_get_bunker_allowed_pubkeys(SettingsManager *sm) {
  return sm ? g_settings_get_strv(sm->settings, "bunker-allowed-pubkeys") : NULL;
}

void settings_manager_set_bunker_allowed_pubkeys(SettingsManager *sm, const gchar *const *pubkeys) {
  if (!sm) return;
  g_settings_set_strv(sm->settings, "bunker-allowed-pubkeys", pubkeys);
}

gchar **settings_manager_get_bunker_allowed_methods(SettingsManager *sm) {
  return sm ? g_settings_get_strv(sm->settings, "bunker-allowed-methods") : NULL;
}

void settings_manager_set_bunker_allowed_methods(SettingsManager *sm, const gchar *const *methods) {
  if (!sm) return;
  g_settings_set_strv(sm->settings, "bunker-allowed-methods", methods);
}

gchar **settings_manager_get_bunker_auto_approve_kinds(SettingsManager *sm) {
  return sm ? g_settings_get_strv(sm->settings, "bunker-auto-approve-kinds") : NULL;
}

void settings_manager_set_bunker_auto_approve_kinds(SettingsManager *sm, const gchar *const *kinds) {
  if (!sm) return;
  g_settings_set_strv(sm->settings, "bunker-auto-approve-kinds", kinds);
}

/* Logging Settings */
gboolean settings_manager_get_log_requests(SettingsManager *sm) {
  return sm ? g_settings_get_boolean(sm->settings, "log-signing-requests") : TRUE;
}

void settings_manager_set_log_requests(SettingsManager *sm, gboolean log) {
  if (!sm) return;
  g_settings_set_boolean(sm->settings, "log-signing-requests", log);
}

gint settings_manager_get_log_retention_days(SettingsManager *sm) {
  return sm ? g_settings_get_int(sm->settings, "log-retention-days") : 30;
}

void settings_manager_set_log_retention_days(SettingsManager *sm, gint days) {
  if (!sm) return;
  g_settings_set_int(sm->settings, "log-retention-days", days);
}

/* Startup Settings */
gboolean settings_manager_get_autostart(SettingsManager *sm) {
  return sm ? g_settings_get_boolean(sm->settings, "autostart") : FALSE;
}

void settings_manager_set_autostart(SettingsManager *sm, gboolean autostart) {
  if (!sm) return;
  g_settings_set_boolean(sm->settings, "autostart", autostart);
}

gboolean settings_manager_get_start_minimized(SettingsManager *sm) {
  return sm ? g_settings_get_boolean(sm->settings, "start-minimized") : FALSE;
}

void settings_manager_set_start_minimized(SettingsManager *sm, gboolean minimized) {
  if (!sm) return;
  g_settings_set_boolean(sm->settings, "start-minimized", minimized);
}

/* Account Settings */
gchar **settings_manager_get_account_order(SettingsManager *sm) {
  return sm ? g_settings_get_strv(sm->settings, "account-order") : NULL;
}

void settings_manager_set_account_order(SettingsManager *sm, const gchar *const *npubs) {
  if (!sm) return;
  g_settings_set_strv(sm->settings, "account-order", npubs);
}

/* Change notifications */
typedef struct {
  SettingsChangedCb cb;
  gpointer user_data;
} ChangedCallbackData;

static void on_settings_changed(GSettings *settings, const gchar *key, gpointer user_data) {
  (void)settings;
  ChangedCallbackData *data = user_data;
  if (data && data->cb) {
    data->cb(key, data->user_data);
  }
}

static void changed_callback_data_free(gpointer data) {
  g_free(data);
}

gulong settings_manager_connect_changed(SettingsManager *sm,
                                        const gchar *key,
                                        SettingsChangedCb cb,
                                        gpointer user_data) {
  if (!sm || !cb) return 0;

  ChangedCallbackData *data = g_new0(ChangedCallbackData, 1);
  data->cb = cb;
  data->user_data = user_data;

  gchar *signal_name;
  if (key) {
    signal_name = g_strdup_printf("changed::%s", key);
  } else {
    signal_name = g_strdup("changed");
  }

  gulong id = g_signal_connect_data(sm->settings, signal_name,
                                    G_CALLBACK(on_settings_changed), data,
                                    (GClosureNotify)changed_callback_data_free, 0);

  g_free(signal_name);
  return id;
}

void settings_manager_disconnect_changed(SettingsManager *sm, gulong handler_id) {
  if (!sm || handler_id == 0) return;
  g_signal_handler_disconnect(sm->settings, handler_id);
}
