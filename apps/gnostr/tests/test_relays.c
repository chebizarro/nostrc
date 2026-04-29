#include <glib.h>
#include <glib/gstdio.h>
#include <nostr-gobject-1.0/gnostr-relays.h>

static gchar *make_temp_config_path(void) {
  const gchar *tmp = g_get_tmp_dir();
  gchar *uuid = g_uuid_string_random();
  gchar *dir = g_build_filename(tmp, "gnostr-test", uuid, NULL);
  g_free(uuid);
  g_mkdir_with_parents(dir, 0700);
  gchar *cfg = g_build_filename(dir, "config.ini", NULL);
  g_free(dir);
  return cfg;
}

static void test_valid_url(void) {
  g_assert_true(gnostr_is_valid_relay_url("wss://nos.lol"));
  g_assert_true(gnostr_is_valid_relay_url("ws://localhost:8080"));
  g_assert_false(gnostr_is_valid_relay_url(NULL));
  g_assert_false(gnostr_is_valid_relay_url(""));
  g_assert_false(gnostr_is_valid_relay_url("http://example.com"));
  g_assert_false(gnostr_is_valid_relay_url("wss:///nohost"));
}

static void test_save_load_roundtrip(void) {
  gchar *cfg = make_temp_config_path();
  g_setenv("GNOSTR_CONFIG_PATH", cfg, TRUE);
  /* Save */
  GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(arr, g_strdup("wss://nos.lol"));
  g_ptr_array_add(arr, g_strdup("wss://relay.primal.net"));
  gnostr_save_relays_from(arr);
  g_ptr_array_free(arr, TRUE);
  /* Load */
  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  gnostr_load_relays_into(out);
  g_assert_cmpuint(out->len, ==, 2);
  g_assert_cmpstr((const char*)out->pdata[0], ==, "wss://nos.lol");
  g_assert_cmpstr((const char*)out->pdata[1], ==, "wss://relay.primal.net");
  g_ptr_array_free(out, TRUE);
  /* Cleanup */
  g_unlink(cfg);
  gchar *parent = g_path_get_dirname(cfg);
  g_rmdir(parent);
  g_free(parent);
  g_free(cfg);
}

static void test_save_empty_list(void) {
  gchar *cfg = make_temp_config_path();
  g_setenv("GNOSTR_CONFIG_PATH", cfg, TRUE);
  GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
  gnostr_save_relays_from(arr);
  g_ptr_array_free(arr, TRUE);
  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  gnostr_load_relays_into(out);
  g_assert_cmpuint(out->len, ==, 0);
  g_ptr_array_free(out, TRUE);
  /* Cleanup */
  g_unlink(cfg);
  gchar *parent = g_path_get_dirname(cfg);
  g_rmdir(parent);
  g_free(parent);
  g_free(cfg);
}

/* Test DM relay save/load (NIP-17) */
static void test_dm_relays_save_load(void) {
  gchar *cfg = make_temp_config_path();
  g_setenv("GNOSTR_CONFIG_PATH", cfg, TRUE);

  /* Save DM relays */
  GPtrArray *dm_arr = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(dm_arr, g_strdup("wss://dm.relay1.com"));
  g_ptr_array_add(dm_arr, g_strdup("wss://dm.relay2.com"));
  gnostr_save_dm_relays_from(dm_arr);
  g_ptr_array_free(dm_arr, TRUE);

  /* Load DM relays */
  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  gnostr_load_dm_relays_into(out);
  g_assert_cmpuint(out->len, ==, 2);
  g_assert_cmpstr((const char*)out->pdata[0], ==, "wss://dm.relay1.com");
  g_assert_cmpstr((const char*)out->pdata[1], ==, "wss://dm.relay2.com");
  g_ptr_array_free(out, TRUE);

  /* Cleanup */
  g_unlink(cfg);
  gchar *parent = g_path_get_dirname(cfg);
  g_rmdir(parent);
  g_free(parent);
  g_free(cfg);
}

/* Test gnostr_get_dm_relays fallback behavior */
static void test_dm_relays_fallback(void) {
  gchar *cfg = make_temp_config_path();
  g_setenv("GNOSTR_CONFIG_PATH", cfg, TRUE);

  /* Ensure no DM relays are set, but general relays are */
  GPtrArray *dm_empty = g_ptr_array_new_with_free_func(g_free);
  gnostr_save_dm_relays_from(dm_empty);
  g_ptr_array_free(dm_empty, TRUE);

  GPtrArray *gen_arr = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(gen_arr, g_strdup("wss://general.relay.com"));
  gnostr_save_relays_from(gen_arr);
  g_ptr_array_free(gen_arr, TRUE);

  /* gnostr_get_dm_relays should fall back to general relays */
  GPtrArray *result = gnostr_get_dm_relays();
  g_assert_cmpuint(result->len, >=, 1);
  /* Should contain the general relay */
  gboolean found = FALSE;
  for (guint i = 0; i < result->len; i++) {
    if (g_strcmp0(g_ptr_array_index(result, i), "wss://general.relay.com") == 0) {
      found = TRUE;
      break;
    }
  }
  g_assert_true(found);
  g_ptr_array_unref(result);

  /* Cleanup */
  g_unlink(cfg);
  gchar *parent = g_path_get_dirname(cfg);
  g_rmdir(parent);
  g_free(parent);
  g_free(cfg);
}

static void test_profile_fetch_relays_include_indexers(void) {
  gchar *cfg = make_temp_config_path();
  g_setenv("GNOSTR_CONFIG_PATH", cfg, TRUE);

  GPtrArray *gen_arr = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(gen_arr, g_strdup("wss://general.relay.com"));
  gnostr_save_relays_from(gen_arr);
  g_ptr_array_free(gen_arr, TRUE);

  GPtrArray *result = gnostr_get_profile_fetch_relay_urls(NULL);
  g_assert_nonnull(result);

  gboolean found_general = FALSE;
  gboolean found_purplepages = FALSE;
  gboolean found_band = FALSE;
  for (guint i = 0; i < result->len; i++) {
    const char *url = g_ptr_array_index(result, i);
    if (g_strcmp0(url, "wss://general.relay.com") == 0)
      found_general = TRUE;
    if (g_strcmp0(url, "wss://purplepag.es") == 0)
      found_purplepages = TRUE;
    if (g_strcmp0(url, "wss://relay.nostr.band") == 0)
      found_band = TRUE;
  }
  g_assert_true(found_general);
  g_assert_true(found_purplepages);
  g_assert_true(found_band);
  g_ptr_array_unref(result);

  g_unlink(cfg);
  gchar *parent = g_path_get_dirname(cfg);
  g_rmdir(parent);
  g_free(parent);
  g_free(cfg);
}

static void test_profile_fetch_relays_prefer_nip65_reads(void) {
  GPtrArray *nip65_relays = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_nip65_relay_free);

  GnostrNip65Relay *read_relay = g_new0(GnostrNip65Relay, 1);
  read_relay->url = g_strdup("wss://profile.read.example");
  read_relay->type = GNOSTR_RELAY_READ;
  g_ptr_array_add(nip65_relays, read_relay);

  GnostrNip65Relay *write_relay = g_new0(GnostrNip65Relay, 1);
  write_relay->url = g_strdup("wss://profile.write.example");
  write_relay->type = GNOSTR_RELAY_WRITE;
  g_ptr_array_add(nip65_relays, write_relay);

  GPtrArray *result = gnostr_get_profile_fetch_relay_urls(nip65_relays);
  g_assert_nonnull(result);

  gboolean found_read = FALSE;
  gboolean found_write = FALSE;
  for (guint i = 0; i < result->len; i++) {
    const char *url = g_ptr_array_index(result, i);
    if (g_strcmp0(url, "wss://profile.read.example") == 0)
      found_read = TRUE;
    if (g_strcmp0(url, "wss://profile.write.example") == 0)
      found_write = TRUE;
  }
  g_assert_true(found_read);
  g_assert_false(found_write);

  g_ptr_array_unref(result);
  g_ptr_array_unref(nip65_relays);
}

/* Test NIP-17 DM relay event parsing (kind 10050) */
static void test_nip17_parse_dm_relays(void) {
  /* Sample kind 10050 event JSON */
  const char *event_json =
    "{"
    "\"kind\": 10050,"
    "\"created_at\": 1700000000,"
    "\"pubkey\": \"abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234abcd1234\","
    "\"tags\": ["
    "  [\"relay\", \"wss://inbox1.example.com\"],"
    "  [\"relay\", \"wss://inbox2.example.com\"],"
    "  [\"other\", \"ignored\"]"
    "],"
    "\"content\": \"\","
    "\"sig\": \"dummy\""
    "}";

  gint64 created_at = 0;
  GPtrArray *relays = gnostr_nip17_parse_dm_relays_event(event_json, &created_at);

  g_assert_nonnull(relays);
  g_assert_cmpint(created_at, ==, 1700000000);
  g_assert_cmpuint(relays->len, ==, 2);
  g_assert_cmpstr((const char*)g_ptr_array_index(relays, 0), ==, "wss://inbox1.example.com");
  g_assert_cmpstr((const char*)g_ptr_array_index(relays, 1), ==, "wss://inbox2.example.com");

  g_ptr_array_unref(relays);
}

/* Test parsing wrong kind (should return NULL) */
static void test_nip17_parse_wrong_kind(void) {
  const char *event_json =
    "{"
    "\"kind\": 10002,"
    "\"tags\": [[\"r\", \"wss://relay.example.com\"]]"
    "}";

  GPtrArray *relays = gnostr_nip17_parse_dm_relays_event(event_json, NULL);
  g_assert_null(relays);
}

/* F39 NIP-65 edge case: Publish uses only write-capable relays */
static void test_nip65_publish_uses_only_write_relays(void) {
  GPtrArray *nip65_relays = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_nip65_relay_free);

  /* Set up 3-relay NIP-65 list: read-only, write-only, read+write */
  GnostrNip65Relay *read_only = g_new0(GnostrNip65Relay, 1);
  read_only->url = g_strdup("wss://read-only.example");
  read_only->type = GNOSTR_RELAY_READ;
  g_ptr_array_add(nip65_relays, read_only);

  GnostrNip65Relay *write_only = g_new0(GnostrNip65Relay, 1);
  write_only->url = g_strdup("wss://write-only.example");
  write_only->type = GNOSTR_RELAY_WRITE;
  g_ptr_array_add(nip65_relays, write_only);

  GnostrNip65Relay *readwrite = g_new0(GnostrNip65Relay, 1);
  readwrite->url = g_strdup("wss://readwrite.example");
  readwrite->type = GNOSTR_RELAY_READWRITE;
  g_ptr_array_add(nip65_relays, readwrite);

  /* Get write relays for publishing - should only include write-only and readwrite */
  GPtrArray *write_relays = gnostr_nip65_get_write_relays(nip65_relays);
  g_assert_nonnull(write_relays);
  g_assert_cmpuint(write_relays->len, ==, 2);

  /* Verify read-only relay is excluded */
  gboolean found_read_only = FALSE;
  gboolean found_write_only = FALSE;
  gboolean found_readwrite = FALSE;

  for (guint i = 0; i < write_relays->len; i++) {
    const char *url = g_ptr_array_index(write_relays, i);
    if (g_strcmp0(url, "wss://read-only.example") == 0)
      found_read_only = TRUE;
    if (g_strcmp0(url, "wss://write-only.example") == 0)
      found_write_only = TRUE;
    if (g_strcmp0(url, "wss://readwrite.example") == 0)
      found_readwrite = TRUE;
  }

  g_assert_false(found_read_only);  /* Must NOT include read-only relay */
  g_assert_true(found_write_only);  /* Must include write-only relay */
  g_assert_true(found_readwrite);   /* Must include readwrite relay */

  g_ptr_array_unref(write_relays);
  g_ptr_array_unref(nip65_relays);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/relays/valid_url", test_valid_url);
  g_test_add_func("/relays/save_load_roundtrip", test_save_load_roundtrip);
  g_test_add_func("/relays/save_empty_list", test_save_empty_list);
  g_test_add_func("/relays/dm_relays_save_load", test_dm_relays_save_load);
  g_test_add_func("/relays/dm_relays_fallback", test_dm_relays_fallback);
  g_test_add_func("/relays/profile_fetch_relays_include_indexers", test_profile_fetch_relays_include_indexers);
  g_test_add_func("/relays/profile_fetch_relays_prefer_nip65_reads", test_profile_fetch_relays_prefer_nip65_reads);
  g_test_add_func("/relays/nip17_parse_dm_relays", test_nip17_parse_dm_relays);
  g_test_add_func("/relays/nip17_parse_wrong_kind", test_nip17_parse_wrong_kind);
  g_test_add_func("/relays/nip65_publish_uses_only_write_relays", test_nip65_publish_uses_only_write_relays);
  return g_test_run();
}
