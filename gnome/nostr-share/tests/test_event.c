/* test_event.c - tag builders, URL → r handling, --to, event JSON
 *
 * SPDX-License-Identifier: MIT
 */
#include "ns-event.h"
#include "ns-kind.h"

#include <nostr/nip19/nip19.h>
#include <glib.h>
#include <stdlib.h>
#include <string.h>

static gchar *
tags_json(JsonArray *tags)
{
  JsonNode *n = json_node_new(JSON_NODE_ARRAY);
  json_node_set_array(n, tags);
  gchar *s = json_to_string(n, FALSE);
  json_node_unref(n);
  return s;
}

static void
test_extract_urls(void)
{
  g_autoptr(GPtrArray) u = ns_extract_urls(
    "Read https://example.com/a?b=1. Also (see https://en.wikipedia.org/wiki/Foo_(bar)) "
    "and http://x.org/, dup https://example.com/a?b=1 and "
    "nostr:nevent1qqs0000 and nostr:npub1abc plus xhttps://no.pe "
    "and <https://angle.example/path>");
  g_assert_cmpuint(u->len, ==, 4);
  g_assert_cmpstr(g_ptr_array_index(u, 0), ==, "https://example.com/a?b=1");
  g_assert_cmpstr(g_ptr_array_index(u, 1), ==, "https://en.wikipedia.org/wiki/Foo_(bar)");
  g_assert_cmpstr(g_ptr_array_index(u, 2), ==, "http://x.org/");
  g_assert_cmpstr(g_ptr_array_index(u, 3), ==, "https://angle.example/path");

  g_autoptr(GPtrArray) none = ns_extract_urls("no links, just nostr:note1xyz and https://");
  g_assert_cmpuint(none->len, ==, 0);
}

static void
test_r_tags(void)
{
  g_autoptr(JsonArray) tags = json_array_new();
  ns_tags_add(tags, "r", "https://a.example", NULL);
  ns_tags_add_urls(tags, "https://a.example and https://b.example! nostr:npub1zzz");
  g_autofree gchar *j = tags_json(tags);
  g_assert_cmpstr(j, ==, "[[\"r\",\"https://a.example\"],[\"r\",\"https://b.example\"]]");
}

static void
test_imeta_and_1063(void)
{
  NsBlobMeta m = { 0 };
  m.url = g_strdup("https://blossom.example/abc.jpg");
  m.mime = g_strdup("image/jpeg");
  g_strlcpy(m.sha256, "ab" "cdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789",
            sizeof(m.sha256));
  m.size = 12345;
  m.width = 800;
  m.height = 600;
  m.alt = g_strdup("a cat");

  g_autoptr(JsonArray) tags = json_array_new();
  json_array_add_array_element(tags, ns_imeta_tag_new(&m));
  g_autofree gchar *j = tags_json(tags);
  g_assert_cmpstr(j, ==,
    "[[\"imeta\",\"url https://blossom.example/abc.jpg\",\"m image/jpeg\","
    "\"x abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789\","
    "\"size 12345\",\"dim 800x600\",\"alt a cat\"]]");

  g_autoptr(JsonArray) fm = json_array_new();
  ns_tags_add_file_metadata(fm, &m);
  g_autofree gchar *j2 = tags_json(fm);
  g_assert_cmpstr(j2, ==,
    "[[\"url\",\"https://blossom.example/abc.jpg\"],[\"m\",\"image/jpeg\"],"
    "[\"x\",\"abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789\"],"
    "[\"ox\",\"abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789\"],"
    "[\"size\",\"12345\"],[\"dim\",\"800x600\"],[\"alt\",\"a cat\"]]");

  /* Unknown dimensions / no alt: those entries are omitted. */
  m.width = 0;
  g_clear_pointer(&m.alt, g_free);
  g_autoptr(JsonArray) t3 = json_array_new();
  json_array_add_array_element(t3, ns_imeta_tag_new(&m));
  g_autofree gchar *j3 = tags_json(t3);
  g_assert_null(strstr(j3, "dim "));
  g_assert_null(strstr(j3, "alt "));
  ns_blob_meta_clear(&m);
}

static void
test_blossom_helpers(void)
{
  g_autofree gchar *u1 = ns_blossom_blob_url("https://b.example/", "ff", "image/png");
  g_assert_cmpstr(u1, ==, "https://b.example/ff.png");
  g_autofree gchar *u2 = ns_blossom_blob_url("https://b.example", "ff", "application/x-unknown");
  g_assert_cmpstr(u2, ==, "https://b.example/ff");

  const gchar *ev =
    "{\"kind\":10063,\"tags\":[[\"server\",\"https://one.example/\"],"
    "[\"server\",\"http://insecure.example\"],[\"server\",\"https://two.example\"],"
    "[\"server\",\"https://one.example\"],[\"relay\",\"wss://r\"],[\"server\"]],"
    "\"content\":\"\"}";
  g_auto(GStrv) s = ns_blossom_servers_from_event(ev);
  g_assert_cmpuint(g_strv_length(s), ==, 2);
  g_assert_cmpstr(s[0], ==, "https://one.example");
  g_assert_cmpstr(s[1], ==, "https://two.example");

  g_auto(GStrv) wrong = ns_blossom_servers_from_event(
    "{\"kind\":10002,\"tags\":[[\"server\",\"https://x.example\"]]}");
  g_assert_cmpuint(g_strv_length(wrong), ==, 0);
}

static void
test_article_helpers(void)
{
  g_autofree gchar *s1 = ns_slugify("Hello, Wörld: A  Test!");
  g_assert_cmpstr(s1, ==, "hello-world-a-test");
  g_autofree gchar *s2 = ns_slugify("!!!");
  g_assert_true(g_str_has_prefix(s2, "note-"));

  g_autofree gchar *t1 = ns_markdown_title("intro\n\n```\n# not this\n```\n#  Real Title ##\n");
  g_assert_cmpstr(t1, ==, "Real Title");
  g_autofree gchar *t2 = ns_markdown_title("---\ntitle: \"From Front\"\n---\n# H1\n");
  g_assert_cmpstr(t2, ==, "From Front");
  g_autofree gchar *t3 = ns_markdown_title("#hashtag line\nno heading");
  g_assert_null(t3);

  g_autoptr(JsonArray) tags = json_array_new();
  ns_tags_add_article(tags, "my-post", "My Post", 1700000000);
  g_autofree gchar *j = tags_json(tags);
  g_assert_cmpstr(j, ==,
    "[[\"d\",\"my-post\"],[\"title\",\"My Post\"],[\"published_at\",\"1700000000\"]]");
}

static void
test_recipient(void)
{
  uint8_t pk[32];
  for (int i = 0; i < 32; i++) pk[i] = (uint8_t)i;
  char *npub = NULL;
  g_assert_cmpint(nostr_nip19_encode_npub(pk, &npub), ==, 0);
  const gchar *hex = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";

  NsRecipient r;
  GError *err = NULL;
  g_assert_true(ns_recipient_parse(npub, &r, &err));
  g_assert_cmpint(r.type, ==, NS_RECIPIENT_MENTION);
  g_assert_cmpstr(r.pubkey_hex, ==, hex);
  ns_recipient_clear(&r);

  g_autofree gchar *pref = g_strdup_printf("nostr:%s", npub);
  g_assert_true(ns_recipient_parse(pref, &r, NULL));
  g_assert_cmpstr(r.npub, ==, npub);
  ns_recipient_clear(&r);

  g_assert_true(ns_recipient_parse(hex, &r, NULL));
  g_assert_cmpstr(r.npub, ==, npub);
  g_autoptr(JsonArray) tags = json_array_new();
  ns_tags_add_recipient(tags, &r);
  ns_tags_add_recipient(tags, &r);   /* idempotent */
  g_assert_cmpuint(json_array_get_length(tags), ==, 1);
  ns_recipient_clear(&r);

  g_assert_true(ns_recipient_parse("groups.example.com'abc_123", &r, NULL));
  g_assert_cmpint(r.type, ==, NS_RECIPIENT_GROUP);
  g_assert_cmpstr(r.group_id, ==, "abc_123");
  g_assert_cmpstr(r.relay_url, ==, "wss://groups.example.com");
  g_autoptr(JsonArray) gt = json_array_new();
  ns_tags_add_recipient(gt, &r);
  g_autofree gchar *gj = tags_json(gt);
  g_assert_cmpstr(gj, ==, "[[\"h\",\"abc_123\"]]");
  ns_recipient_clear(&r);

  g_assert_true(ns_recipient_parse("wss://g.example/'x", &r, NULL));
  g_assert_cmpstr(r.relay_url, ==, "wss://g.example");
  ns_recipient_clear(&r);

  g_assert_true(ns_recipient_parse(NULL, &r, NULL));
  g_assert_cmpint(r.type, ==, NS_RECIPIENT_NONE);

  const gchar *bad[] = { "npub1invalid", "bob", "host/path'id", "host'bad id", "user@h'x" };
  for (gsize i = 0; i < G_N_ELEMENTS(bad); i++) {
    g_assert_false(ns_recipient_parse(bad[i], &r, &err));
    g_assert_error(err, NS_ERROR, NS_ERROR_BAD_INPUT);
    g_clear_error(&err);
  }
  free(npub);
}

static void
test_unsigned_json(void)
{
  g_autoptr(JsonArray) tags = json_array_new();
  ns_tags_add(tags, "r", "https://a.example", NULL);
  g_autofree gchar *j = ns_event_unsigned_json(1, 1700000000, NULL, tags,
                                               "hi \"there\"", FALSE);
  g_assert_cmpstr(j, ==,
    "{\"created_at\":1700000000,\"kind\":1,\"tags\":[[\"r\",\"https://a.example\"]],"
    "\"content\":\"hi \\\"there\\\"\"}");
  g_autofree gchar *j2 = ns_event_unsigned_json(1, 1, "aa", tags, "", FALSE);
  g_assert_true(g_str_has_prefix(j2, "{\"pubkey\":\"aa\","));
  /* tags were serialised, not consumed */
  g_assert_cmpuint(json_array_get_length(tags), ==, 1);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nostr-share/event/extract-urls", test_extract_urls);
  g_test_add_func("/nostr-share/event/r-tags", test_r_tags);
  g_test_add_func("/nostr-share/event/imeta-1063", test_imeta_and_1063);
  g_test_add_func("/nostr-share/event/blossom-helpers", test_blossom_helpers);
  g_test_add_func("/nostr-share/event/article", test_article_helpers);
  g_test_add_func("/nostr-share/event/recipient", test_recipient);
  g_test_add_func("/nostr-share/event/unsigned-json", test_unsigned_json);
  return g_test_run();
}
