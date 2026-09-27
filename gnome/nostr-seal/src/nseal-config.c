/* nseal-config.c — see nseal-config.h.
 * SPDX-License-Identifier: MIT
 */
#include "nseal-config.h"
#include "nostr-seal.h"
#include "nseal-private.h"

gchar *nseal_config_default_path(void) {
  return g_build_filename(g_get_user_config_dir(), "nostr", "seal.conf", NULL);
}

NsealConfig *nseal_config_load(GError **error) {
  g_autoptr(NsealConfig) cfg = g_new0(NsealConfig, 1);
  const gchar *env = g_getenv("NOSTR_SEAL_CONFIG");
  cfg->path = (env && *env) ? g_strdup(env) : nseal_config_default_path();
  cfg->default_recipients = g_new0(gchar *, 1);
  cfg->upstream = NSEAL_UPSTREAM_DIRECT_ONLY;

  g_autoptr(GKeyFile) kf = g_key_file_new();
  GError *local = NULL;
  if (!g_key_file_load_from_file(kf, cfg->path, G_KEY_FILE_NONE, &local)) {
    if (g_error_matches(local, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
      g_clear_error(&local);
      return g_steal_pointer(&cfg);
    }
    g_propagate_prefixed_error(error, local, "%s: ", cfg->path);
    return NULL;
  }
  cfg->loaded = TRUE;

  g_autofree gchar *raw = g_key_file_get_string(kf, NSEAL_CONFIG_GROUP,
                                                "default_recipients", NULL);
  if (raw) {
    g_autoptr(GStrvBuilder) b = g_strv_builder_new();
    g_auto(GStrv) parts = g_strsplit_set(raw, ";, \t", -1);
    for (guint i = 0; parts[i]; i++) {
      if (!*parts[i]) continue;
      uint8_t pk[32];
      GError *e = NULL;
      if (!nseal_parse_pubkey(parts[i], pk, &e)) {
        g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_ARG,
                    "%s: default_recipients: %s", cfg->path, e->message);
        g_error_free(e);
        return NULL;
      }
      g_strv_builder_add(b, parts[i]);
    }
    g_strfreev(cfg->default_recipients);
    cfg->default_recipients = g_strv_builder_end(b);
  }

  if (g_key_file_has_key(kf, NSEAL_CONFIG_GROUP, "include_self", NULL)) {
    cfg->include_self = g_key_file_get_boolean(kf, NSEAL_CONFIG_GROUP,
                                               "include_self", &local);
    if (local) {
      g_propagate_prefixed_error(error, local, "%s: include_self: ", cfg->path);
      return NULL;
    }
  }
  if (g_key_file_has_key(kf, NSEAL_CONFIG_GROUP, "work_factor", NULL)) {
    gint w = g_key_file_get_integer(kf, NSEAL_CONFIG_GROUP, "work_factor", &local);
    if (local || w < NSEAL_LOG_N_MIN || w > NSEAL_LOG_N_MAX) {
      g_clear_error(&local);
      g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_ARG,
                  "%s: work_factor must be %d..%d", cfg->path, NSEAL_LOG_N_MIN,
                  NSEAL_LOG_N_MAX);
      return NULL;
    }
    cfg->work_factor = w;
  }
  g_autofree gchar *mode = g_key_file_get_string(kf, NSEAL_CONFIG_GROUP, "upstream_mode", NULL);
  if (mode) {
    g_strstrip(mode);
    if (g_str_equal(mode, "direct_only")) cfg->upstream = NSEAL_UPSTREAM_DIRECT_ONLY;
    else if (g_str_equal(mode, "session_relay_or_direct"))
      cfg->upstream = NSEAL_UPSTREAM_SESSION_RELAY_OR_DIRECT;
    else if (g_str_equal(mode, "session_relay_only"))
      cfg->upstream = NSEAL_UPSTREAM_SESSION_RELAY_ONLY;
    else {
      g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_ARG,
                  "%s: upstream_mode must be direct_only, session_relay_or_direct or "
                  "session_relay_only", cfg->path);
      return NULL;
    }
  }
  return g_steal_pointer(&cfg);
}

void nseal_config_free(NsealConfig *cfg) {
  if (!cfg) return;
  g_free(cfg->path);
  g_strfreev(cfg->default_recipients);
  g_free(cfg);
}
