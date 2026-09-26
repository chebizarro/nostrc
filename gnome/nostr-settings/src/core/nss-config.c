/* nss-config.c — see nss-config.h.
 * SPDX-License-Identifier: MIT
 */
#include "nss-config.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <nostr/nip19/nip19.h>
#include <stdlib.h>
#include <string.h>

G_DEFINE_QUARK(nss-config-error-quark, nss_config_error)

gboolean
nss_config_write_atomic(const gchar *path, const gchar *contents, GError **error)
{
  g_autofree gchar *dir = g_path_get_dirname(path);
  if (g_mkdir_with_parents(dir, 0700) != 0) {
    int e = errno;
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(e),
                "%s: %s", dir, g_strerror(e));
    return FALSE;
  }
  /* Keep an existing file's mode (GLib versions differ on whether a
   * consistent replace does); new files are private. */
  GStatBuf st;
  int mode = g_stat(path, &st) == 0 ? (int)(st.st_mode & 0777) : 0600;
  return g_file_set_contents_full(path, contents, -1,
                                  G_FILE_SET_CONTENTS_CONSISTENT, mode, error);
}

/* ── pubkeys ───────────────────────────────────────────────────────────── */

gboolean
nss_parse_pubkey(const gchar *text, guint8 out[32], GError **error)
{
  if (text == NULL || *text == '\0')
    goto bad;
  if (g_str_has_prefix(text, "npub1")) {
    if (nostr_nip19_decode_npub(text, out) == 0)
      return TRUE;
    goto bad;
  }
  if (strlen(text) == 64) {
    for (int i = 0; i < 32; i++) {
      int hi = g_ascii_xdigit_value(text[2 * i]);
      int lo = g_ascii_xdigit_value(text[2 * i + 1]);
      if (hi < 0 || lo < 0)
        goto bad;
      out[i] = (guint8)(hi << 4 | lo);
    }
    return TRUE;
  }
bad:
  g_set_error(error, NSS_CONFIG_ERROR, NSS_CONFIG_ERROR_INVALID,
              "“%s” is not an npub or a 64-character hex public key",
              text ? text : "");
  return FALSE;
}

gchar *
nss_pubkey_to_hex(const guint8 pk[32])
{
  GString *s = g_string_sized_new(64);
  for (int i = 0; i < 32; i++)
    g_string_append_printf(s, "%02x", pk[i]);
  return g_string_free(s, FALSE);
}

gchar *
nss_pubkey_to_npub(const guint8 pk[32])
{
  char *b = NULL;
  if (nostr_nip19_encode_npub(pk, &b) != 0 || b == NULL)
    return NULL;
  gchar *out = g_strdup(b);
  free(b);
  return out;
}

/* ── session-relay.conf retention ──────────────────────────────────────── */

typedef struct {
  const gchar *key;
  gsize        offset;
  gboolean     is_bool;
} RetKey;

static const RetKey RET_KEYS[] = {
  { "retention_enabled",            G_STRUCT_OFFSET(NssRetention, enabled),            TRUE  },
  { "retention_cache_max_mb",       G_STRUCT_OFFSET(NssRetention, cache_max_mb),       FALSE },
  { "retention_high_watermark_pct", G_STRUCT_OFFSET(NssRetention, high_watermark_pct), FALSE },
  { "retention_low_watermark_pct",  G_STRUCT_OFFSET(NssRetention, low_watermark_pct),  FALSE },
  { "retention_min_age_days",       G_STRUCT_OFFSET(NssRetention, min_age_days),       FALSE },
  { "retention_note_ttl_days",      G_STRUCT_OFFSET(NssRetention, note_ttl_days),      FALSE },
  { "retention_reaction_ttl_days",  G_STRUCT_OFFSET(NssRetention, reaction_ttl_days),  FALSE },
  { "retention_interval_mins",      G_STRUCT_OFFSET(NssRetention, interval_mins),      FALSE },
};

#define RET_FIELD(r, k) G_STRUCT_MEMBER(gint, (r), (k)->offset)

void
nss_retention_defaults(NssRetention *r)
{
  r->enabled = TRUE;
  r->cache_max_mb = 1024;
  r->high_watermark_pct = 90;
  r->low_watermark_pct = 75;
  r->min_age_days = 2;
  r->note_ttl_days = 90;
  r->reaction_ttl_days = 14;
  r->interval_mins = 60;
}

gboolean
nss_retention_validate(const NssRetention *r, GError **error)
{
  const gchar *why = NULL;
  if (r->cache_max_mb != 0 && r->cache_max_mb < NSS_RETENTION_MIN_CACHE_MB)
    why = "the storage limit must be 0 (no limit) or at least 64 MB";
  else if (r->high_watermark_pct < 2 || r->high_watermark_pct > 99)
    why = "the start threshold must be 2–99 %";
  else if (r->low_watermark_pct < 1 || r->low_watermark_pct >= r->high_watermark_pct)
    why = "the stop threshold must be at least 1 % and below the start threshold";
  else if (r->min_age_days < 0 || r->note_ttl_days < 0 || r->reaction_ttl_days < 0)
    why = "ages must not be negative";
  else if (r->note_ttl_days != 0 && r->note_ttl_days < r->min_age_days)
    why = "notes cannot expire before the minimum age";
  else if (r->reaction_ttl_days != 0 && r->reaction_ttl_days < r->min_age_days)
    why = "reactions cannot expire before the minimum age";
  else if (r->interval_mins < 1 || r->interval_mins > 7 * 24 * 60)
    why = "the check interval must be 1 minute to 7 days";
  if (why != NULL) {
    g_set_error_literal(error, NSS_CONFIG_ERROR, NSS_CONFIG_ERROR_INVALID, why);
    return FALSE;
  }
  return TRUE;
}

gchar *
nss_relay_conf_path(void)
{
  return g_build_filename(g_get_user_config_dir(), "nostr", "session-relay.conf", NULL);
}

/* Split "key = value" the way relayd_config_load() does (trim, optional
 * double quotes). Returns FALSE for comments / blank / non key=value lines. */
static gboolean
split_kv(const gchar *line, gchar **key, gchar **val)
{
  g_autofree gchar *l = g_strstrip(g_strdup(line));
  if (*l == '\0' || *l == '#' || *l == ';')
    return FALSE;
  gchar *eq = strchr(l, '=');
  if (eq == NULL)
    return FALSE;
  *eq = '\0';
  *key = g_strstrip(g_strdup(l));
  gchar *v = g_strstrip(g_strdup(eq + 1));
  gsize n = strlen(v);
  if (n >= 2 && v[0] == '"' && v[n - 1] == '"') {
    memmove(v, v + 1, n - 2);
    v[n - 2] = '\0';
  }
  *val = v;
  return TRUE;
}

static const RetKey *
ret_key(const gchar *key)
{
  for (gsize i = 0; i < G_N_ELEMENTS(RET_KEYS); i++)
    if (g_str_equal(RET_KEYS[i].key, key))
      return &RET_KEYS[i];
  return NULL;
}

gboolean
nss_relay_conf_load(const gchar *path, NssRetention *out, GError **error)
{
  nss_retention_defaults(out);
  g_autofree gchar *data = NULL;
  GError *local = NULL;
  if (!g_file_get_contents(path, &data, NULL, &local)) {
    if (g_error_matches(local, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
      g_clear_error(&local);
      return TRUE;
    }
    g_propagate_error(error, local);
    return FALSE;
  }
  g_auto(GStrv) lines = g_strsplit(data, "\n", -1);
  for (guint i = 0; lines[i]; i++) {
    g_autofree gchar *k = NULL, *v = NULL;
    if (!split_kv(lines[i], &k, &v))
      continue;
    const RetKey *rk = ret_key(k);
    if (rk == NULL)
      continue;
    gchar *end = NULL;
    errno = 0;
    gint64 n = g_ascii_strtoll(v, &end, 10);
    if (end == v || *end != '\0' || errno != 0 || n < G_MININT || n > G_MAXINT ||
        (rk->is_bool && n != 0 && n != 1)) {
      g_set_error(error, NSS_CONFIG_ERROR, NSS_CONFIG_ERROR_PARSE,
                  "%s:%u: bad value for %s: “%s”", path, i + 1, k, v);
      return FALSE;
    }
    RET_FIELD(out, rk) = (gint)n;
  }
  return TRUE;
}

gboolean
nss_relay_conf_save(const gchar *path, const NssRetention *r, GError **error)
{
  if (!nss_retention_validate(r, error))
    return FALSE;
  g_autofree gchar *data = NULL;
  GError *local = NULL;
  if (!g_file_get_contents(path, &data, NULL, &local)) {
    if (!g_error_matches(local, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
      g_propagate_error(error, local);
      return FALSE;
    }
    g_clear_error(&local);
  }
  gboolean written[G_N_ELEMENTS(RET_KEYS)] = { FALSE };
  GString *outs = g_string_new(NULL);
  if (data != NULL && *data != '\0') {
    g_auto(GStrv) lines = g_strsplit(data, "\n", -1);
    guint n = g_strv_length(lines);
    /* A trailing "\n" yields a final empty element; do not double it. */
    if (n > 0 && *lines[n - 1] == '\0')
      n--;
    for (guint i = 0; i < n; i++) {
      g_autofree gchar *k = NULL, *v = NULL;
      const RetKey *rk = split_kv(lines[i], &k, &v) ? ret_key(k) : NULL;
      if (rk == NULL) {
        g_string_append_printf(outs, "%s\n", lines[i]);
        continue;
      }
      gsize idx = (gsize)(rk - RET_KEYS);
      if (written[idx])
        continue; /* drop duplicates; the last one would win in relayd anyway */
      written[idx] = TRUE;
      g_string_append_printf(outs, "%s = %d\n", rk->key, RET_FIELD(r, rk));
    }
  }
  gboolean header = FALSE;
  for (gsize i = 0; i < G_N_ELEMENTS(RET_KEYS); i++) {
    if (written[i])
      continue;
    if (!header) {
      if (outs->len > 0 && outs->str[outs->len - 1] != '\n')
        g_string_append_c(outs, '\n');
      g_string_append(outs,
        "# Storage retention (Nostr Settings). Flat keys: session-relay.conf\n"
        "# has no [sections]. Enforced once RetentionSupported is true\n"
        "# (nostrc-prqu.17); see docs/designs/nostrdb-retention-eviction-policy.md.\n");
      header = TRUE;
    }
    g_string_append_printf(outs, "%s = %d\n", RET_KEYS[i].key, RET_FIELD(r, &RET_KEYS[i]));
  }
  g_autofree gchar *s = g_string_free(outs, FALSE);
  return nss_config_write_atomic(path, s, error);
}

/* ── GKeyFile helpers ──────────────────────────────────────────────────── */

static GKeyFile *
keyfile_load(const gchar *path, GError **error)
{
  GKeyFile *kf = g_key_file_new();
  GError *local = NULL;
  if (!g_key_file_load_from_file(kf, path,
                                 G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS,
                                 &local)) {
    if (g_error_matches(local, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
      g_clear_error(&local);
      return kf;
    }
    g_key_file_unref(kf);
    g_set_error(error, NSS_CONFIG_ERROR, NSS_CONFIG_ERROR_PARSE, "%s: %s", path,
                local->message);
    g_error_free(local);
    return NULL;
  }
  return kf;
}

static gboolean
keyfile_save(GKeyFile *kf, const gchar *path, GError **error)
{
  g_autofree gchar *data = g_key_file_to_data(kf, NULL, error);
  return data != NULL && nss_config_write_atomic(path, data, error);
}

static gboolean
kf_bool(GKeyFile *kf, const gchar *path, const gchar *group, const gchar *key,
        gboolean *out, GError **error)
{
  if (!g_key_file_has_key(kf, group, key, NULL))
    return TRUE;
  GError *local = NULL;
  gboolean v = g_key_file_get_boolean(kf, group, key, &local);
  if (local != NULL) {
    g_set_error(error, NSS_CONFIG_ERROR, NSS_CONFIG_ERROR_PARSE,
                "%s: %s must be true or false", path, key);
    g_error_free(local);
    return FALSE;
  }
  *out = v;
  return TRUE;
}

/* "a;b", "a, b" → strv (the tools accept both separators). */
static gchar **
kf_list(GKeyFile *kf, const gchar *group, const gchar *key)
{
  g_autofree gchar *raw = g_key_file_get_string(kf, group, key, NULL);
  g_autoptr(GStrvBuilder) b = g_strv_builder_new();
  if (raw != NULL) {
    g_auto(GStrv) parts = g_strsplit_set(raw, ";, \t", -1);
    for (guint i = 0; parts[i]; i++)
      if (*parts[i])
        g_strv_builder_add(b, parts[i]);
  }
  return g_strv_builder_end(b);
}

static void
kf_set_list(GKeyFile *kf, const gchar *group, const gchar *key,
            const gchar *const *items)
{
  g_autofree gchar *joined = g_strjoinv(";", (gchar **)items);
  g_key_file_set_string(kf, group, key, joined);
}

/* ── nostr-notify ──────────────────────────────────────────────────────── */

#define NOTIFY_GROUP "notify"

gboolean
nss_notify_presentation_supported(void)
{
  return FALSE; /* nostrc-prqu.16 */
}

gchar *
nss_notify_conf_path(void)
{
  return g_build_filename(g_get_user_config_dir(), "nostr-notify", "nostr-notify.conf", NULL);
}

void
nss_notify_conf_init(NssNotifyConf *c)
{
  memset(c, 0, sizeof *c);
  c->upstream = NSS_NOTIFY_UPSTREAM_DIRECT;
  c->home_relays = g_new0(gchar *, 1);
  c->notify_groups = TRUE;
  c->notify_dms = TRUE;
  c->group_preview = TRUE;
  c->sound = FALSE;
}

void
nss_notify_conf_clear(NssNotifyConf *c)
{
  g_clear_pointer(&c->home_relays, g_strfreev);
}

gboolean
nss_notify_conf_load(const gchar *path, NssNotifyConf *out, GError **error)
{
  nss_notify_conf_init(out);
  g_autoptr(GKeyFile) kf = keyfile_load(path, error);
  if (kf == NULL)
    return FALSE;
  g_autofree gchar *mode = g_key_file_get_string(kf, NOTIFY_GROUP, "upstream_mode", NULL);
  if (mode != NULL) {
    g_strstrip(mode);
    if (g_str_equal(mode, "session_relay"))
      out->upstream = NSS_NOTIFY_UPSTREAM_SESSION_RELAY;
    else if (!g_str_equal(mode, "direct") && *mode != '\0') {
      g_set_error(error, NSS_CONFIG_ERROR, NSS_CONFIG_ERROR_PARSE,
                  "%s: unknown upstream_mode “%s”", path, mode);
      return FALSE;
    }
  }
  g_strfreev(out->home_relays);
  out->home_relays = kf_list(kf, NOTIFY_GROUP, "home_relays");
  return kf_bool(kf, path, NOTIFY_GROUP, "notify_groups", &out->notify_groups, error) &&
         kf_bool(kf, path, NOTIFY_GROUP, "notify_dms", &out->notify_dms, error) &&
         kf_bool(kf, path, NOTIFY_GROUP, "group_preview", &out->group_preview, error) &&
         kf_bool(kf, path, NOTIFY_GROUP, "sound", &out->sound, error);
}

static void
set_reserved(GKeyFile *kf, const gchar *key, gboolean v, gboolean dflt)
{
  if (v != dflt || g_key_file_has_key(kf, NOTIFY_GROUP, key, NULL))
    g_key_file_set_boolean(kf, NOTIFY_GROUP, key, v);
}

gboolean
nss_notify_conf_save(const gchar *path, const NssNotifyConf *c, GError **error)
{
  for (guint i = 0; c->home_relays && c->home_relays[i]; i++)
    if (!g_str_has_prefix(c->home_relays[i], "wss://") &&
        !g_str_has_prefix(c->home_relays[i], "ws://")) {
      g_set_error(error, NSS_CONFIG_ERROR, NSS_CONFIG_ERROR_INVALID,
                  "“%s” is not a ws:// or wss:// relay URL", c->home_relays[i]);
      return FALSE;
    }
  g_autoptr(GKeyFile) kf = keyfile_load(path, error);
  if (kf == NULL)
    return FALSE;
  g_key_file_set_string(kf, NOTIFY_GROUP, "upstream_mode",
                        c->upstream == NSS_NOTIFY_UPSTREAM_SESSION_RELAY ? "session_relay"
                                                                         : "direct");
  kf_set_list(kf, NOTIFY_GROUP, "home_relays", (const gchar *const *)c->home_relays);
  set_reserved(kf, "notify_groups", c->notify_groups, TRUE);
  set_reserved(kf, "notify_dms", c->notify_dms, TRUE);
  set_reserved(kf, "group_preview", c->group_preview, TRUE);
  set_reserved(kf, "sound", c->sound, FALSE);
  return keyfile_save(kf, path, error);
}

/* ── nostr-seal ────────────────────────────────────────────────────────── */

#define SEAL_GROUP "seal"

gchar *
nss_seal_conf_path(void)
{
  const gchar *env = g_getenv("NOSTR_SEAL_CONFIG");
  if (env != NULL && *env != '\0')
    return g_strdup(env);
  return g_build_filename(g_get_user_config_dir(), "nostr", "seal.conf", NULL);
}

void
nss_seal_conf_init(NssSealConf *c)
{
  memset(c, 0, sizeof *c);
  c->default_recipients = g_new0(gchar *, 1);
}

void
nss_seal_conf_clear(NssSealConf *c)
{
  g_clear_pointer(&c->default_recipients, g_strfreev);
}

gboolean
nss_seal_conf_load(const gchar *path, NssSealConf *out, GError **error)
{
  nss_seal_conf_init(out);
  g_autoptr(GKeyFile) kf = keyfile_load(path, error);
  if (kf == NULL)
    return FALSE;
  g_strfreev(out->default_recipients);
  out->default_recipients = kf_list(kf, SEAL_GROUP, "default_recipients");
  if (!kf_bool(kf, path, SEAL_GROUP, "include_self", &out->include_self, error))
    return FALSE;
  if (g_key_file_has_key(kf, SEAL_GROUP, "work_factor", NULL)) {
    GError *local = NULL;
    gint w = g_key_file_get_integer(kf, SEAL_GROUP, "work_factor", &local);
    if (local != NULL || w < NSS_SEAL_WORK_MIN || w > NSS_SEAL_WORK_MAX) {
      g_clear_error(&local);
      g_set_error(error, NSS_CONFIG_ERROR, NSS_CONFIG_ERROR_PARSE,
                  "%s: work_factor must be %d..%d", path, NSS_SEAL_WORK_MIN,
                  NSS_SEAL_WORK_MAX);
      return FALSE;
    }
    out->work_factor = w;
  }
  return TRUE;
}

gboolean
nss_seal_conf_save(const gchar *path, const NssSealConf *c, GError **error)
{
  for (guint i = 0; c->default_recipients && c->default_recipients[i]; i++) {
    guint8 pk[32];
    if (!nss_parse_pubkey(c->default_recipients[i], pk, error))
      return FALSE;
  }
  if (c->work_factor != 0 &&
      (c->work_factor < NSS_SEAL_WORK_MIN || c->work_factor > NSS_SEAL_WORK_MAX)) {
    g_set_error(error, NSS_CONFIG_ERROR, NSS_CONFIG_ERROR_INVALID,
                "the scrypt cost must be %d..%d", NSS_SEAL_WORK_MIN, NSS_SEAL_WORK_MAX);
    return FALSE;
  }
  g_autoptr(GKeyFile) kf = keyfile_load(path, error);
  if (kf == NULL)
    return FALSE;
  if (c->default_recipients && c->default_recipients[0])
    kf_set_list(kf, SEAL_GROUP, "default_recipients",
                (const gchar *const *)c->default_recipients);
  else
    (void)g_key_file_remove_key(kf, SEAL_GROUP, "default_recipients", NULL);
  g_key_file_set_boolean(kf, SEAL_GROUP, "include_self", c->include_self);
  if (c->work_factor != 0)
    g_key_file_set_integer(kf, SEAL_GROUP, "work_factor", c->work_factor);
  else
    (void)g_key_file_remove_key(kf, SEAL_GROUP, "work_factor", NULL);
  return keyfile_save(kf, path, error);
}

/* ── nostr-share ───────────────────────────────────────────────────────── */

#define SHARE_GROUP "nostr-share"

gchar *
nss_share_conf_path(void)
{
  const gchar *env = g_getenv("NOSTR_SHARE_CONFIG");
  if (env != NULL && *env != '\0')
    return g_strdup(env);
  return g_build_filename(g_get_user_config_dir(), "nostr-share", "nostr-share.conf", NULL);
}

gboolean
nss_share_conf_load(const gchar *path, NssShareConf *out, GError **error)
{
  out->text_kind = 1;
  out->keep_metadata = FALSE;
  g_autoptr(GKeyFile) kf = keyfile_load(path, error);
  if (kf == NULL)
    return FALSE;
  if (g_key_file_has_key(kf, SHARE_GROUP, "default_text_kind", NULL)) {
    gint k = g_key_file_get_integer(kf, SHARE_GROUP, "default_text_kind", NULL);
    if (k != 1 && k != 30023) {
      g_set_error(error, NSS_CONFIG_ERROR, NSS_CONFIG_ERROR_PARSE,
                  "%s: default_text_kind must be 1 or 30023", path);
      return FALSE;
    }
    out->text_kind = k;
  }
  return kf_bool(kf, path, SHARE_GROUP, "keep_metadata", &out->keep_metadata, error);
}

gboolean
nss_share_conf_save(const gchar *path, const NssShareConf *c, GError **error)
{
  if (c->text_kind != 1 && c->text_kind != 30023) {
    g_set_error(error, NSS_CONFIG_ERROR, NSS_CONFIG_ERROR_INVALID,
                "the default kind for text must be 1 or 30023");
    return FALSE;
  }
  g_autoptr(GKeyFile) kf = keyfile_load(path, error);
  if (kf == NULL)
    return FALSE;
  g_key_file_set_integer(kf, SHARE_GROUP, "default_text_kind", c->text_kind);
  g_key_file_set_boolean(kf, SHARE_GROUP, "keep_metadata", c->keep_metadata);
  return keyfile_save(kf, path, error);
}
