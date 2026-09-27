/* ns-config.c - nostr-share configuration
 *
 * SPDX-License-Identifier: MIT
 */
#include "ns-config.h"
#include "ns-kind.h"

#include <string.h>

#define NS_CONFIG_GROUP              "nostr-share"
#define NS_DEFAULT_MAX_UPLOAD_MIB    100
#define NS_DEFAULT_OK_WAIT_SEC       15
#define NS_DEFAULT_QUERY_TIMEOUT_MS  6000

static const struct { NsUpstreamMode mode; const gchar *name; } MODES[] = {
  { NS_UPSTREAM_SESSION_RELAY_AND_DIRECT, "session_relay_and_direct" },
  { NS_UPSTREAM_SESSION_RELAY_OR_DIRECT,  "session_relay_or_direct" },
  { NS_UPSTREAM_SESSION_RELAY_ONLY,       "session_relay_only" },
  { NS_UPSTREAM_DIRECT_ONLY,              "direct_only" },
};

const gchar *
ns_upstream_mode_name(NsUpstreamMode mode)
{
  for (gsize i = 0; i < G_N_ELEMENTS(MODES); i++)
    if (MODES[i].mode == mode)
      return MODES[i].name;
  return "unknown";
}

gboolean
ns_upstream_mode_parse(const gchar *s, NsUpstreamMode *out)
{
  for (gsize i = 0; i < G_N_ELEMENTS(MODES); i++)
    if (g_strcmp0(MODES[i].name, s) == 0) {
      *out = MODES[i].mode;
      return TRUE;
    }
  return FALSE;
}

/* Accept both "a;b" (GKeyFile list) and "a,b" / whitespace. */
static gchar **
read_list(GKeyFile *kf, const gchar *key)
{
  g_autofree gchar *raw = g_key_file_get_string(kf, NS_CONFIG_GROUP, key, NULL);
  g_autoptr(GStrvBuilder) b = g_strv_builder_new();
  if (raw != NULL) {
    g_auto(GStrv) parts = g_strsplit_set(raw, ";, \t", -1);
    for (guint i = 0; parts[i] != NULL; i++)
      if (*parts[i] != '\0')
        g_strv_builder_add(b, parts[i]);
  }
  return g_strv_builder_end(b);
}

NsConfig *
ns_config_load(GError **error)
{
  NsConfig *cfg = g_new0(NsConfig, 1);
  cfg->upstream         = NS_UPSTREAM_SESSION_RELAY_OR_DIRECT;   /* nostrc-t24q */
  cfg->max_upload_bytes = (guint64)NS_DEFAULT_MAX_UPLOAD_MIB * 1024u * 1024u;
  cfg->ok_wait_sec      = NS_DEFAULT_OK_WAIT_SEC;
  cfg->query_timeout_ms = NS_DEFAULT_QUERY_TIMEOUT_MS;

  const gchar *env = g_getenv("NOSTR_SHARE_CONFIG");
  cfg->config_path = env != NULL && *env != '\0'
    ? g_strdup(env)
    : g_build_filename(g_get_user_config_dir(), "nostr-share",
                       "nostr-share.conf", NULL);

  g_autoptr(GKeyFile) kf = g_key_file_new();
  GError *local = NULL;
  if (!g_key_file_load_from_file(kf, cfg->config_path, G_KEY_FILE_NONE, &local)) {
    if (g_error_matches(local, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
      g_clear_error(&local);
      cfg->home_relays = g_new0(gchar *, 1);
      cfg->blossom_servers = g_new0(gchar *, 1);
      return cfg;
    }
    g_propagate_prefixed_error(error, local, "%s: ", cfg->config_path);
    ns_config_free(cfg);
    return NULL;
  }

  cfg->home_relays     = read_list(kf, "home_relays");
  cfg->blossom_servers = read_list(kf, "blossom_servers");

  g_autofree gchar *mode = g_key_file_get_string(kf, NS_CONFIG_GROUP,
                                                 "upstream_mode", NULL);
  if (mode != NULL && !ns_upstream_mode_parse(g_strstrip(mode), &cfg->upstream)) {
    g_set_error(error, NS_ERROR, NS_ERROR_BAD_INPUT,
                "%s: unknown upstream_mode '%s'", cfg->config_path, mode);
    ns_config_free(cfg);
    return NULL;
  }

  if (g_key_file_has_key(kf, NS_CONFIG_GROUP, "max_upload_mib", NULL)) {
    gint mib = g_key_file_get_integer(kf, NS_CONFIG_GROUP, "max_upload_mib", NULL);
    if (mib > 0)
      cfg->max_upload_bytes = (guint64)mib * 1024u * 1024u;
  }
  if (g_key_file_has_key(kf, NS_CONFIG_GROUP, "ok_wait_sec", NULL)) {
    gint s = g_key_file_get_integer(kf, NS_CONFIG_GROUP, "ok_wait_sec", NULL);
    if (s > 0)
      cfg->ok_wait_sec = (guint)s;
  }
  if (g_key_file_has_key(kf, NS_CONFIG_GROUP, "default_text_kind", NULL)) {
    gint k = g_key_file_get_integer(kf, NS_CONFIG_GROUP, "default_text_kind", NULL);
    if (k != NS_KIND_NOTE && k != NS_KIND_ARTICLE) {
      g_set_error(error, NS_ERROR, NS_ERROR_BAD_INPUT,
                  "%s: default_text_kind must be %d or %d", cfg->config_path,
                  NS_KIND_NOTE, NS_KIND_ARTICLE);
      ns_config_free(cfg);
      return NULL;
    }
    cfg->text_kind = k;
  }
  if (g_key_file_has_key(kf, NS_CONFIG_GROUP, "keep_metadata", NULL)) {
    GError *b = NULL;
    cfg->keep_metadata = g_key_file_get_boolean(kf, NS_CONFIG_GROUP, "keep_metadata", &b);
    if (b != NULL) {
      g_set_error(error, NS_ERROR, NS_ERROR_BAD_INPUT,
                  "%s: keep_metadata must be true or false", cfg->config_path);
      g_error_free(b);
      ns_config_free(cfg);
      return NULL;
    }
  }
  g_autofree gchar *dav = g_key_file_get_string(kf, NS_CONFIG_GROUP, "dav_url", NULL);
  if (dav != NULL && *g_strstrip(dav) != '\0')
    cfg->dav_url = g_steal_pointer(&dav);
  return cfg;
}

void
ns_config_free(NsConfig *cfg)
{
  if (cfg == NULL)
    return;
  g_strfreev(cfg->home_relays);
  g_strfreev(cfg->blossom_servers);
  g_free(cfg->dav_url);
  g_free(cfg->config_path);
  g_free(cfg);
}
