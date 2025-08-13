#pragma once

#include <glib.h>

/* Return config file path; respects GNOSTR_CONFIG_PATH if set. Caller frees. */
gchar *gnostr_config_path(void);

/* Basic validation for Nostr relay URLs: must be ws:// or wss:// and have a host. */
gboolean gnostr_is_valid_relay_url(const char *url);

/* Normalize relay URL (trim spaces, lowercase scheme/host, remove trailing slash). Returns newly allocated string or NULL if invalid. */
gchar *gnostr_normalize_relay_url(const char *url);

/* Load relay URLs from config into provided array of gchar* (owned by caller). */
void gnostr_load_relays_into(GPtrArray *out);

/* Save relay URLs from provided array to config; replaces the list. */
void gnostr_save_relays_from(GPtrArray *arr);
