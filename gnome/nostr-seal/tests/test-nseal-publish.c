/* nostr-seal --publish (nostrc-hby8): kind-1063 shape + CLI dry run. */
#include "nostr-seal.h"
#include "nseal-publish.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>

#define PK_A "3bf0c63fcb93463407af97a5e5ee64fa883d107ef9e558472c4eb9aaaefa459d"
#define PK_B "82341f882b6eabcd2ba7f1ef90aad961cf074af15b9ef44a09f9d2a8fbfbe6a2"

static gboolean has_tag(JsonArray *tags, const char *k, const char *v) {
  for (guint i = 0; i < json_array_get_length(tags); i++) {
    JsonArray *t = json_array_get_array_element(tags, i);
    if (json_array_get_length(t) >= 2 && g_str_equal(json_array_get_string_element(t, 0), k) &&
        (!v || g_str_equal(json_array_get_string_element(t, 1), v)))
      return TRUE;
  }
  return FALSE;
}

static JsonObject *parse_obj(const char *json, JsonParser **p) {
  *p = json_parser_new();
  g_assert_true(json_parser_load_from_data(*p, json, -1, NULL));
  return json_node_get_object(json_parser_get_root(*p));
}

static void test_event_shape(void) {
  const char *rcpts[] = {PK_A, PK_B, PK_A, NULL};
  g_autofree char *sha = g_strnfill(64, 'a');
  g_autofree char *json = nseal_publish_event_json(PK_A, 1700000000, "https://b.example/x", sha,
                                                   4242, rcpts, FALSE);
  g_assert_nonnull(json);
  g_autoptr(JsonParser) p = NULL;
  JsonObject *o = parse_obj(json, &p);
  g_assert_cmpint(json_object_get_int_member(o, "kind"), ==, 1063);
  g_assert_cmpstr(json_object_get_string_member(o, "content"), ==, "");
  JsonArray *tags = json_object_get_array_member(o, "tags");
  g_assert_true(has_tag(tags, "url", "https://b.example/x"));
  g_assert_true(has_tag(tags, "m", NSEAL_MIME_TYPE));
  g_assert_true(has_tag(tags, "x", sha));
  g_assert_true(has_tag(tags, "ox", sha));
  g_assert_true(has_tag(tags, "size", "4242"));
  g_assert_true(has_tag(tags, "alt", NULL));
  g_assert_true(has_tag(tags, "p", PK_A));
  g_assert_true(has_tag(tags, "p", PK_B));
  guint p_tags = 0;
  for (guint i = 0; i < json_array_get_length(tags); i++)
    if (g_str_equal(json_array_get_string_element(json_array_get_array_element(tags, i), 0), "p"))
      p_tags++;
  g_assert_cmpuint(p_tags, ==, 2); /* deduplicated */
  g_assert_false(has_tag(tags, "name", NULL)); /* no file name leaks */
}

/* Seal for two recipients, then `publish --dry-run` with no signer and no
 * session relay: prints the 1063 event, uploads nothing. */
static void test_cli_dry_run(void) {
  g_autofree char *dir = g_dir_make_tmp("nseal-pub-XXXXXX", NULL);
  g_autofree char *in = g_build_filename(dir, "secret.txt", NULL);
  g_autofree char *out = g_build_filename(dir, "secret.txt.nsealed", NULL);
  g_autofree char *share_conf = g_build_filename(dir, "nostr-share.conf", NULL);
  g_autofree char *seal_conf = g_build_filename(dir, "seal.conf", NULL);
  g_assert_true(g_file_set_contents(in, "hello sealed world\n", -1, NULL));
  g_assert_true(g_file_set_contents(share_conf,
                                    "[nostr-share]\nhome_relays=wss://relay.example\n"
                                    "blossom_servers=https://blossom.example\n", -1, NULL));
  g_assert_true(g_file_set_contents(seal_conf, "[seal]\n", -1, NULL));

  g_auto(GStrv) env = g_get_environ();
  env = g_environ_setenv(env, "NOSTR_SHARE_CONFIG", share_conf, TRUE);
  env = g_environ_setenv(env, "NOSTR_SEAL_CONFIG", seal_conf, TRUE);
  env = g_environ_setenv(env, "XDG_RUNTIME_DIR", dir, TRUE); /* no relay.sock */
  env = g_environ_setenv(env, "DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent", TRUE);

  const char *seal_argv[] = {NSEAL_CLI_PATH, "encrypt", "--to", PK_A, "--to", PK_B, in, NULL};
  int status = 0;
  g_autofree char *so = NULL, *se = NULL;
  g_assert_true(g_spawn_sync(NULL, (char **)seal_argv, env, G_SPAWN_DEFAULT, NULL, NULL, &so,
                             &se, &status, NULL));
  g_assert_true(g_spawn_check_wait_status(status, NULL));

  const char *pub_argv[] = {NSEAL_CLI_PATH, "publish", "--dry-run", out, NULL};
  g_autofree char *po = NULL, *pe = NULL;
  g_assert_true(g_spawn_sync(NULL, (char **)pub_argv, env, G_SPAWN_DEFAULT, NULL, NULL, &po,
                             &pe, &status, NULL));
  if (!g_spawn_check_wait_status(status, NULL)) g_printerr("stderr: %s\n", pe);
  g_assert_true(g_spawn_check_wait_status(status, NULL));
  g_assert_nonnull(strstr(pe, "upstream_mode: direct_only"));
  g_assert_nonnull(strstr(pe, "targets: wss://relay.example"));
  g_assert_nonnull(strstr(pe, "dry run: nothing uploaded"));

  g_autoptr(JsonParser) p = NULL;
  JsonObject *o = parse_obj(po, &p);
  g_assert_cmpint(json_object_get_int_member(o, "kind"), ==, 1063);
  JsonArray *tags = json_object_get_array_member(o, "tags");
  g_assert_true(has_tag(tags, "p", PK_A));
  g_assert_true(has_tag(tags, "p", PK_B));
  g_assert_true(has_tag(tags, "m", NSEAL_MIME_TYPE));
  /* The url is the first server's BUD-01 address of the sealed bytes. */
  g_autofree char *data = NULL;
  gsize len = 0;
  g_assert_true(g_file_get_contents(out, &data, &len, NULL));
  g_autofree char *sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)data, len);
  g_assert_true(has_tag(tags, "x", sha));
  g_autofree char *want_url = g_strconcat("https://blossom.example/", sha, NULL);
  gboolean url_ok = FALSE;
  for (guint i = 0; i < json_array_get_length(tags); i++) {
    JsonArray *t = json_array_get_array_element(tags, i);
    if (g_str_equal(json_array_get_string_element(t, 0), "url"))
      url_ok = g_str_has_prefix(json_array_get_string_element(t, 1), want_url);
  }
  g_assert_true(url_ok);
  g_assert_null(strstr(po, "secret.txt")); /* the file name is not published */

  g_remove(in); g_remove(out); g_remove(share_conf); g_remove(seal_conf); g_rmdir(dir);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nseal/publish/event-shape", test_event_shape);
  g_test_add_func("/nseal/publish/cli-dry-run", test_cli_dry_run);
  return g_test_run();
}
