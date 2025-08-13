#include <glib.h>
#include <glib/gstdio.h>
#include "util/relays.h"

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
  g_assert_true(gnostr_is_valid_relay_url("wss://relay.damus.io"));
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
  g_ptr_array_add(arr, g_strdup("wss://relay.damus.io"));
  g_ptr_array_add(arr, g_strdup("wss://nos.lol"));
  gnostr_save_relays_from(arr);
  g_ptr_array_free(arr, TRUE);
  /* Load */
  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  gnostr_load_relays_into(out);
  g_assert_cmpuint(out->len, ==, 2);
  g_assert_cmpstr((const char*)out->pdata[0], ==, "wss://relay.damus.io");
  g_assert_cmpstr((const char*)out->pdata[1], ==, "wss://nos.lol");
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

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/relays/valid_url", test_valid_url);
  g_test_add_func("/relays/save_load_roundtrip", test_save_load_roundtrip);
  g_test_add_func("/relays/save_empty_list", test_save_empty_list);
  return g_test_run();
}
