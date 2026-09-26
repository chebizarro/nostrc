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
  config->upstream_mode = ND_UPSTREAM_MODE_DEFAULT;
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

  return TRUE;
}
