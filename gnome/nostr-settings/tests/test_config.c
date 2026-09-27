/* test_config — every backing store the Settings pages edit: load
 * defaults, round-trip, preservation of foreign content, validation, file
 * mode, and (when built together) that the owning tool's parser accepts
 * what Settings wrote. SPDX-License-Identifier: MIT */
#include "nss-config.h"

#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef NSS_TEST_SEAL_PARSER
#include "nseal-config.h"
#endif
#ifdef NSS_TEST_SHARE_PARSER
#include "ns-config.h"
#endif
#ifdef NSS_TEST_RELAYD_PARSER
#include "relayd_config.h"
#endif

#define NPUB "npub10elfcs4fr0l0r8af98jlmgdh9c8tcxjvz9qkw038js35mp4dma8qzvjptg"
#define HEX  "7e7e9c42a91bfef19fa929e5fda1b72e0ebc1a4c1141673e2794234d86addf4e"

static gchar *tmp;

static gchar *
path_in(const gchar *rel)
{
  return g_build_filename(tmp, rel, NULL);
}

static gchar *
slurp(const gchar *p)
{
  gchar *s = NULL;
  g_assert_true(g_file_get_contents(p, &s, NULL, NULL));
  return s;
}

static void
assert_private(const gchar *p)
{
  struct stat st;
  g_assert_cmpint(g_stat(p, &st), ==, 0);
  g_assert_cmpint(st.st_mode & 0777, ==, 0600);
  g_autofree gchar *dir = g_path_get_dirname(p);
  g_assert_cmpint(g_stat(dir, &st), ==, 0);
}

/* ── session-relay.conf ── */

static void
test_relay_defaults_and_roundtrip(void)
{
  g_autofree gchar *p = path_in("nostr/session-relay.conf");
  NssRetention r;
  g_assert_true(nss_relay_conf_load(p, &r, NULL));   /* missing → defaults */
  g_assert_true(r.enabled);
  g_assert_cmpint(r.cache_max_mb, ==, 1024);
  g_assert_cmpint(r.high_watermark_pct, ==, 90);
  g_assert_cmpint(r.low_watermark_pct, ==, 75);

  r.cache_max_mb = 2048;
  r.low_watermark_pct = 60;
  r.enabled = FALSE;
  GError *e = NULL;
  g_assert_true(nss_relay_conf_save(p, &r, &e));
  g_assert_no_error(e);
  assert_private(p);
  NssRetention back;
  g_assert_true(nss_relay_conf_load(p, &back, &e));
  g_assert_no_error(e);
  g_assert_cmpmem(&r, sizeof r, &back, sizeof back);
  g_autofree gchar *s = slurp(p);
  g_auto(GStrv) lines = g_strsplit(s, "\n", -1);   /* never a section header */
  for (guint i = 0; lines[i]; i++)
    g_assert_false(g_str_has_prefix(g_strchug(lines[i]), "["));
  g_assert_nonnull(strstr(s, "retention_enabled = 0\n"));
  g_assert_nonnull(strstr(s, "retention_cache_max_mb = 2048\n"));
}

static void
test_relay_preserves_foreign_lines(void)
{
  g_autofree gchar *p = path_in("nostr/preserve.conf");
  const gchar *orig =
    "# my relay\n"
    "supported_nips = [1, 11, 42]\n"
    "retention_cache_max_mb = 512\n"
    "name = \"laptop relay\"\n"
    "; old comment style\n"
    "retention_cache_max_mb = 999\n"          /* duplicate: collapsed */
    "max_subs = 20";                          /* no trailing newline */
  g_assert_true(g_file_set_contents(p, orig, -1, NULL));
  NssRetention r;
  g_assert_true(nss_relay_conf_load(p, &r, NULL));
  g_assert_cmpint(r.cache_max_mb, ==, 999);   /* last wins, like relayd */
  r.cache_max_mb = 4096;
  g_assert_true(nss_relay_conf_save(p, &r, NULL));
  g_autofree gchar *s = slurp(p);
  g_assert_true(g_str_has_prefix(s,
    "# my relay\nsupported_nips = [1, 11, 42]\nretention_cache_max_mb = 4096\n"
    "name = \"laptop relay\"\n; old comment style\nmax_subs = 20\n"));
  /* Missing keys appended once, under one header. */
  g_assert_nonnull(strstr(s, "retention_interval_mins = 60\n"));
  g_assert_null(strstr(s, "999"));
  const gchar *first = strstr(s, "# Storage retention");
  g_assert_nonnull(first);
  g_assert_null(strstr(first + 1, "# Storage retention"));
  /* Idempotent. */
  g_assert_true(nss_relay_conf_save(p, &r, NULL));
  g_autofree gchar *s2 = slurp(p);
  g_assert_cmpstr(s, ==, s2);

#ifdef NSS_TEST_RELAYD_PARSER
  /* The relay itself still loads the file. */
  RelaydConfig rc;
  g_assert_cmpint(relayd_config_load(p, &rc), ==, 0);
  g_assert_cmpint(rc.supported_nips_count, ==, 3);
  g_assert_cmpint(rc.max_subs, ==, 20);
  g_assert_cmpstr(rc.name, ==, "laptop relay");
#endif
}

static void
test_relay_validation(void)
{
  g_autofree gchar *p = path_in("nostr/invalid.conf");
  NssRetention r;
  nss_retention_defaults(&r);
  GError *e = NULL;
  g_assert_true(nss_retention_validate(&r, &e));
  r.low_watermark_pct = r.high_watermark_pct;
  g_assert_false(nss_relay_conf_save(p, &r, &e));
  g_assert_error(e, NSS_CONFIG_ERROR, NSS_CONFIG_ERROR_INVALID);
  g_clear_error(&e);
  g_assert_false(g_file_test(p, G_FILE_TEST_EXISTS)); /* nothing written */
  nss_retention_defaults(&r);
  r.cache_max_mb = 10;
  g_assert_false(nss_retention_validate(&r, NULL));
  r.cache_max_mb = 0;
  g_assert_true(nss_retention_validate(&r, NULL));
  r.note_ttl_days = 1;          /* below min_age_days (2) */
  g_assert_false(nss_retention_validate(&r, NULL));
  nss_retention_defaults(&r);
  r.interval_mins = 0;
  g_assert_false(nss_retention_validate(&r, NULL));

  g_assert_true(g_file_set_contents(p, "retention_enabled = 7\n", -1, NULL));
  g_assert_false(nss_relay_conf_load(p, &r, &e));
  g_assert_error(e, NSS_CONFIG_ERROR, NSS_CONFIG_ERROR_PARSE);
  g_clear_error(&e);
  g_assert_true(g_file_set_contents(p, "retention_cache_max_mb = lots\n", -1, NULL));
  g_assert_false(nss_relay_conf_load(p, &r, NULL));
}

/* ── nostr-notify ── */

static void
test_notify_roundtrip(void)
{
  g_autofree gchar *p = path_in("nostr-notify/nostr-notify.conf");
  NssNotifyConf c;
  g_assert_true(nss_notify_conf_load(p, &c, NULL));
  g_assert_cmpint(c.upstream, ==, NSS_NOTIFY_UPSTREAM_DIRECT);
  g_assert_cmpuint(g_strv_length(c.home_relays), ==, 0);
  g_assert_true(c.notify_groups && c.notify_dms && c.group_preview && !c.sound);
  nss_notify_conf_clear(&c);

  g_assert_cmpint(g_mkdir_with_parents(path_in("nostr-notify"), 0700), ==, 0);
  g_assert_true(g_file_set_contents(p,
    "# header comment\n[notify]\n# mode comment\nupstream_mode = direct\n"
    "home_relays = wss://relay.damus.io;wss://nostr.wine\n"
    "[other]\nkeep = me\n", -1, NULL));
  g_assert_true(nss_notify_conf_load(p, &c, NULL));
  g_assert_cmpuint(g_strv_length(c.home_relays), ==, 2);
  g_assert_cmpstr(c.home_relays[1], ==, "wss://nostr.wine");
  c.upstream = NSS_NOTIFY_UPSTREAM_SESSION_RELAY;
  g_strfreev(c.home_relays);
  c.home_relays = g_strsplit("wss://a.example;wss://b.example", ";", -1);
  g_assert_cmpint(g_chmod(p, 0640), ==, 0);
  GError *e = NULL;
  g_assert_true(nss_notify_conf_save(p, &c, &e));
  g_assert_no_error(e);
  struct stat pst;                       /* an existing file keeps its mode */
  g_assert_cmpint(g_stat(p, &pst), ==, 0);
  g_assert_cmpint(pst.st_mode & 0777, ==, 0640);
  nss_notify_conf_clear(&c);

  g_autofree gchar *s = slurp(p);
  g_assert_nonnull(strstr(s, "# header comment"));
  g_assert_nonnull(strstr(s, "# mode comment"));
  g_assert_nonnull(strstr(s, "[other]"));
  g_assert_nonnull(strstr(s, "keep"));
  /* Reserved keys at their defaults are not added. */
  g_assert_null(strstr(s, "notify_groups"));
  g_assert_null(strstr(s, "sound"));

  /* The daemon reads home_relays with g_key_file_get_string_list (';'). */
  g_autoptr(GKeyFile) kf = g_key_file_new();
  g_assert_true(g_key_file_load_from_file(kf, p, G_KEY_FILE_NONE, NULL));
  g_autofree gchar *mode = g_key_file_get_string(kf, "notify", "upstream_mode", NULL);
  g_assert_cmpstr(mode, ==, "session_relay");
  gsize n = 0;
  g_auto(GStrv) list = g_key_file_get_string_list(kf, "notify", "home_relays", &n, NULL);
  g_assert_cmpuint(n, ==, 2);
  g_assert_cmpstr(list[0], ==, "wss://a.example");

  g_assert_true(nss_notify_conf_load(p, &c, NULL));
  g_assert_cmpint(c.upstream, ==, NSS_NOTIFY_UPSTREAM_SESSION_RELAY);
  c.sound = TRUE;                       /* non-default reserved key: written */
  g_assert_true(nss_notify_conf_save(p, &c, NULL));
  nss_notify_conf_clear(&c);
  g_assert_true(nss_notify_conf_load(p, &c, NULL));
  g_assert_true(c.sound);
  g_strfreev(c.home_relays);
  c.home_relays = g_strsplit("https://not-a-relay", ";", -1);
  g_assert_false(nss_notify_conf_save(p, &c, &e));
  g_assert_error(e, NSS_CONFIG_ERROR, NSS_CONFIG_ERROR_INVALID);
  g_clear_error(&e);
  nss_notify_conf_clear(&c);

  g_assert_true(g_file_set_contents(p, "[notify]\nupstream_mode = carrier_pigeon\n", -1, NULL));
  g_assert_false(nss_notify_conf_load(p, &c, &e));
  g_assert_error(e, NSS_CONFIG_ERROR, NSS_CONFIG_ERROR_PARSE);
  g_clear_error(&e);
  nss_notify_conf_clear(&c);
}

/* ── nostr-seal ── */

static void
test_seal_roundtrip(void)
{
  g_autofree gchar *p = path_in("nostr/seal.conf");
  g_setenv("NOSTR_SEAL_CONFIG", p, TRUE);
  g_autofree gchar *resolved = nss_seal_conf_path();
  g_assert_cmpstr(resolved, ==, p);
  NssSealConf c;
  g_assert_true(nss_seal_conf_load(p, &c, NULL));
  g_assert_cmpuint(g_strv_length(c.default_recipients), ==, 0);
  g_strfreev(c.default_recipients);
  c.default_recipients = g_strsplit(NPUB ";" HEX, ";", -1);
  c.include_self = TRUE;
  c.work_factor = 18;
  GError *e = NULL;
  g_assert_true(nss_seal_conf_save(p, &c, &e));
  g_assert_no_error(e);
  assert_private(p);
  nss_seal_conf_clear(&c);
  g_assert_true(nss_seal_conf_load(p, &c, NULL));
  g_assert_cmpuint(g_strv_length(c.default_recipients), ==, 2);
  g_assert_cmpstr(c.default_recipients[1], ==, HEX);
  g_assert_true(c.include_self);
  g_assert_cmpint(c.work_factor, ==, 18);

#ifdef NSS_TEST_SEAL_PARSER
  g_autoptr(NsealConfig) sc = nseal_config_load(&e);
  g_assert_no_error(e);
  g_assert_true(sc->loaded);
  g_assert_cmpuint(g_strv_length(sc->default_recipients), ==, 2);
  g_assert_true(sc->include_self);
  g_assert_cmpint(sc->work_factor, ==, 18);
#endif

  /* Clearing removes keys instead of writing empties. */
  g_strfreev(c.default_recipients);
  c.default_recipients = g_new0(gchar *, 1);
  c.work_factor = 0;
  g_assert_true(nss_seal_conf_save(p, &c, NULL));
  g_autofree gchar *s = slurp(p);
  g_assert_null(strstr(s, "default_recipients"));
  g_assert_null(strstr(s, "work_factor"));

  /* Invalid input never reaches the file. */
  g_strfreev(c.default_recipients);
  c.default_recipients = g_strsplit("npub1broken", ";", -1);
  g_assert_false(nss_seal_conf_save(p, &c, &e));
  g_assert_error(e, NSS_CONFIG_ERROR, NSS_CONFIG_ERROR_INVALID);
  g_clear_error(&e);
  g_strfreev(c.default_recipients);
  c.default_recipients = g_new0(gchar *, 1);
  c.work_factor = 25;
  g_assert_false(nss_seal_conf_save(p, &c, NULL));
  nss_seal_conf_clear(&c);
  g_unsetenv("NOSTR_SEAL_CONFIG");
}

/* ── nostr-share ── */

static void
test_share_roundtrip(void)
{
  g_autofree gchar *p = path_in("nostr-share/nostr-share.conf");
  g_setenv("NOSTR_SHARE_CONFIG", p, TRUE);
  g_assert_cmpint(g_mkdir_with_parents(path_in("nostr-share"), 0700), ==, 0);
  g_assert_true(g_file_set_contents(p,
    "[nostr-share]\n# keep\nhome_relays=wss://r.example\nmax_upload_mib=50\n", -1, NULL));
  NssShareConf c;
  g_assert_true(nss_share_conf_load(p, &c, NULL));
  g_assert_cmpint(c.text_kind, ==, 1);
  g_assert_false(c.keep_metadata);
  c.text_kind = 30023;
  c.keep_metadata = TRUE;
  GError *e = NULL;
  g_assert_true(nss_share_conf_save(p, &c, &e));
  g_assert_no_error(e);
  NssShareConf b;
  g_assert_true(nss_share_conf_load(p, &b, NULL));
  g_assert_cmpint(b.text_kind, ==, 30023);
  g_assert_true(b.keep_metadata);
  g_autofree gchar *s = slurp(p);
  g_assert_nonnull(strstr(s, "# keep"));
  g_assert_nonnull(strstr(s, "max_upload_mib=50"));

#ifdef NSS_TEST_SHARE_PARSER
  NsConfig *sc = ns_config_load(&e);
  g_assert_no_error(e);
  g_assert_cmpint(sc->text_kind, ==, 30023);
  g_assert_true(sc->keep_metadata);
  g_assert_cmpstr(sc->home_relays[0], ==, "wss://r.example");
  ns_config_free(sc);
#endif

  c.text_kind = 1063;
  g_assert_false(nss_share_conf_save(p, &c, &e));
  g_clear_error(&e);
  g_assert_true(g_file_set_contents(p, "[nostr-share]\ndefault_text_kind=7\n", -1, NULL));
  g_assert_false(nss_share_conf_load(p, &c, &e));
  g_clear_error(&e);
  g_unsetenv("NOSTR_SHARE_CONFIG");
}

static void
test_pubkeys(void)
{
  guint8 a[32], b[32];
  g_assert_true(nss_parse_pubkey(NPUB, a, NULL));
  g_assert_true(nss_parse_pubkey(HEX, b, NULL));
  g_assert_cmpmem(a, 32, b, 32);
  g_autofree gchar *hex = nss_pubkey_to_hex(a);
  g_assert_cmpstr(hex, ==, HEX);
  g_autofree gchar *npub = nss_pubkey_to_npub(a);
  g_assert_cmpstr(npub, ==, NPUB);
  g_assert_false(nss_parse_pubkey("npub1x", a, NULL));
  g_assert_false(nss_parse_pubkey("zz7e9c42a91bfef19fa929e5fda1b72e0ebc1a4c1141673e2794234d86addf4e", a, NULL));
  g_assert_false(nss_parse_pubkey("", a, NULL));
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  tmp = g_dir_make_tmp("nss-config-XXXXXX", NULL);
  g_test_add_func("/nostr-settings/config/relay/roundtrip", test_relay_defaults_and_roundtrip);
  g_test_add_func("/nostr-settings/config/relay/preserve", test_relay_preserves_foreign_lines);
  g_test_add_func("/nostr-settings/config/relay/validation", test_relay_validation);
  g_test_add_func("/nostr-settings/config/notify", test_notify_roundtrip);
  g_test_add_func("/nostr-settings/config/seal", test_seal_roundtrip);
  g_test_add_func("/nostr-settings/config/share", test_share_roundtrip);
  g_test_add_func("/nostr-settings/config/pubkeys", test_pubkeys);
  int rc = g_test_run();
  g_autofree gchar *cmd = g_strdup_printf("rm -rf '%s'", tmp);
  if (system(cmd) != 0 && rc == 0)
    rc = 1;
  return rc;
}
