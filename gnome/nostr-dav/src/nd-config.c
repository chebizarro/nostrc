/* nd-config.c - nostr-dav daemon configuration
 *
 * SPDX-License-Identifier: MIT
 */

#include "nd-config.h"

G_DEFINE_QUARK(nd-config-error-quark, nd_config_error)

static const struct {
  NdUpstreamMode mode;
  const gchar   *name;
} upstream_modes[] = {
  { ND_UPSTREAM_MODE_SESSION_RELAY_ONLY,      "session_relay_only" },
  { ND_UPSTREAM_MODE_SESSION_RELAY_OR_DIRECT, "session_relay_or_direct" },
  { ND_UPSTREAM_MODE_DIRECT_ONLY,             "direct_only" },
};

const gchar *
nd_upstream_mode_to_string(NdUpstreamMode mode)
{
  for (gsize i = 0; i < G_N_ELEMENTS(upstream_modes); i++)
    if (upstream_modes[i].mode == mode)
      return upstream_modes[i].name;
  g_return_val_if_reached("unknown");
}

gboolean
nd_upstream_mode_from_string(const gchar *str, NdUpstreamMode *out_mode)
{
  g_return_val_if_fail(out_mode != NULL, FALSE);
  if (str == NULL)
    return FALSE;
  for (gsize i = 0; i < G_N_ELEMENTS(upstream_modes); i++) {
    if (g_str_equal(str, upstream_modes[i].name)) {
      *out_mode = upstream_modes[i].mode;
      return TRUE;
    }
  }
  return FALSE;
}

gchar *
nd_config_default_path(void)
{
  return g_build_filename(g_get_user_config_dir(), "nostr-dav",
                          "nostr-dav.conf", NULL);
}

void
nd_config_init_defaults(NdConfig *config)
{
  g_return_if_fail(config != NULL);
  config->upstream_mode  = ND_UPSTREAM_MODE_DEFAULT;
  config->publish_quorum = ND_PUBLISH_QUORUM_DEFAULT;
  config->account_pubkey = NULL;
  config->home_relays    = NULL;
  config->enable_publish = FALSE;
}

void
nd_config_clear(NdConfig *config)
{
  if (config == NULL)
    return;
  g_clear_pointer(&config->account_pubkey, g_free);
  g_clear_pointer(&config->home_relays, g_strfreev);
  config->upstream_mode  = ND_UPSTREAM_MODE_DEFAULT;
  config->publish_quorum = ND_PUBLISH_QUORUM_DEFAULT;
  config->enable_publish = FALSE;
}

gboolean
nd_publish_quorum_parse(const gchar     *str,
                        NdPublishQuorum *out_quorum,
                        GError         **error)
{
  g_return_val_if_fail(out_quorum != NULL, FALSE);
  if (str == NULL) {
    g_set_error(error, ND_CONFIG_ERROR, ND_CONFIG_ERROR_INVALID_VALUE,
                "nostr_dav_publish_quorum: missing value");
    return FALSE;
  }

  if (g_ascii_strcasecmp(str, "all") == 0) {
    out_quorum->all   = TRUE;
    out_quorum->count = 0;
    return TRUE;
  }

  gchar *end = NULL;
  guint64 v = g_ascii_strtoull(str, &end, 10);
  if (end == str || *end != '\0' || v == 0 || v > G_MAXUINT) {
    g_set_error(error, ND_CONFIG_ERROR, ND_CONFIG_ERROR_INVALID_VALUE,
                "nostr_dav_publish_quorum: invalid value '%s' "
                "(expected 'all' or a positive integer)", str);
    return FALSE;
  }

  out_quorum->all   = FALSE;
  out_quorum->count = (guint)v;
  return TRUE;
}

gboolean
nd_config_load(const gchar *path, NdConfig *config, GError **error)
{
  g_return_val_if_fail(path != NULL, FALSE);
  g_return_val_if_fail(config != NULL, FALSE);

  nd_config_init_defaults(config);

  g_autoptr(GKeyFile) kf = g_key_file_new();
  GError *local_err = NULL;
  if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &local_err)) {
    if (g_error_matches(local_err, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
      g_clear_error(&local_err);
      return TRUE;
    }
    g_propagate_prefixed_error(error, local_err, "%s: ", path);
    return FALSE;
  }

  g_autofree gchar *mode =
    g_key_file_get_string(kf, ND_CONFIG_GROUP, "nostr_dav_upstream_mode", NULL);
  if (mode != NULL) {
    g_strstrip(mode);
    if (!nd_upstream_mode_from_string(mode, &config->upstream_mode)) {
      g_set_error(error, ND_CONFIG_ERROR, ND_CONFIG_ERROR_INVALID_VALUE,
                  "%s: invalid nostr_dav_upstream_mode '%s' (expected "
                  "session_relay_only, session_relay_or_direct, or direct_only)",
                  path, mode);
      return FALSE;
    }
  }

  g_autofree gchar *quorum =
    g_key_file_get_string(kf, ND_CONFIG_GROUP, "nostr_dav_publish_quorum", NULL);
  if (quorum != NULL) {
    g_strstrip(quorum);
    GError *qerr = NULL;
    if (!nd_publish_quorum_parse(quorum, &config->publish_quorum, &qerr)) {
      g_propagate_prefixed_error(error, qerr, "%s: ", path);
      return FALSE;
    }
  }

  g_autofree gchar *pubkey =
    g_key_file_get_string(kf, ND_CONFIG_GROUP, "account_pubkey", NULL);
  if (pubkey != NULL) {
    g_strstrip(pubkey);
    if (*pubkey != '\0') {
      g_free(config->account_pubkey);
      config->account_pubkey = g_strdup(pubkey);
    }
  }

  GError *bool_err = NULL;
  gboolean enable = g_key_file_get_boolean(kf, ND_CONFIG_GROUP,
                                           "enable_publish", &bool_err);
  if (bool_err == NULL) {
    config->enable_publish = enable;
  } else if (!g_error_matches(bool_err, G_KEY_FILE_ERROR,
                              G_KEY_FILE_ERROR_KEY_NOT_FOUND)) {
    g_propagate_prefixed_error(error, bool_err, "%s: enable_publish: ", path);
    return FALSE;
  } else {
    g_clear_error(&bool_err);
  }

  GStrv relays =
    g_key_file_get_string_list(kf, ND_CONFIG_GROUP, "home_relays",
                               NULL, NULL);
  if (relays != NULL) {
    /* Trim whitespace and drop empty entries so a trailing comma or
     * indented list stays parseable. */
    guint out = 0;
    for (guint i = 0; relays[i] != NULL; i++) {
      g_strstrip(relays[i]);
      if (*relays[i] == '\0') {
        g_free(relays[i]);
        continue;
      }
      relays[out++] = relays[i];
    }
    relays[out] = NULL;
    if (out == 0) {
      g_strfreev(relays);
      relays = NULL;
    }
    g_strfreev(config->home_relays);
    config->home_relays = relays;
  }

  return TRUE;
}
