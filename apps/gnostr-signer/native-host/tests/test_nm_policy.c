/* test_nm_policy — origin policy, unsigned-event validation, hex/npub,
 * getRelays shape, NUL-escape guard (nostrc-jjyp). */
#include "nm_policy.h"

#include <string.h>

/* BIP-340 test vector 0: secret key 3. */
#define PK_HEX  "f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9"
#define PK_NPUB "npub1lycg5qvjtrp3qjf5f7zl382j9x6nrjz9sdhenvyxq8c3808qxmus6gq266"

typedef struct { const gchar *in; const gchar *want; } OriginCase;

static void test_origin_accepts(void) {
  const OriginCase ok[] = {
    { "https://snort.social",            "https://snort.social" },
    { "https://snort.social:8443",       "https://snort.social:8443" },
    { "https://sub.do-main_x.example",   "https://sub.do-main_x.example" },
    { "https://xn--80ak6aa92e.com",      "https://xn--80ak6aa92e.com" },
    { "https://192.0.2.10",              "https://192.0.2.10" },
    { "https://[2001:db8::1]:9000",      "https://[2001:db8::1]:9000" },
    { "http://localhost",                "http://localhost" },
    { "http://localhost:5173",           "http://localhost:5173" },
    { "http://app.localhost:3000",       "http://app.localhost:3000" },
    { "http://127.0.0.1:8080",           "http://127.0.0.1:8080" },
    { "http://[::1]",                    "http://[::1]" },
    { "http://[::1]:3000",               "http://[::1]:3000" },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(ok); i++) {
    const gchar *why = NULL;
    g_autofree gchar *app = nm_origin_to_app_id(ok[i].in, &why);
    if (!app) g_error("origin %s refused: %s", ok[i].in, why);
    g_assert_cmpstr(app, ==, ok[i].want);
  }
}

static void test_origin_refuses(void) {
  const gchar *bad[] = {
    NULL, "", "null",
    "http://snort.social",            /* insecure */
    "http://localhost.evil.com",      /* not a loopback name */
    "http://127.0.0.2",               /* only 127.0.0.1 */
    "http://[::2]",
    "ws://localhost",
    "file:///home/u/x.html",
    "data:text/html,hi",
    "moz-extension://abcd",
    "chrome-extension://ljigikpdhlameofnnalkmjnbeagdhbin",
    "about:blank",
    "https://",
    "https://user@snort.social",
    "https://user:pw@snort.social",
    "https://snort.social/path",
    "https://snort.social/?q=1",
    "https://snort.social#frag",
    "https://snort.social:0",
    "https://snort.social:65536",
    "https://snort.social:123456",
    "https://snort.social:",
    "https://snort..social",
    "https://.snort.social",
    "https://snort.social.",
    "https://sn ort.social",
    "https://snört.social",
    "https://snort.social\\@evil.com",
    "https://[::1",
    "https://[zz::1]",
    "javascript:alert(1)",
    /* non-canonical spellings of acceptable origins: the browser never
     * serializes these, so the host refuses rather than re-canonicalizes */
    "https://snort.social/",
    "HTTPS://snort.social",
    "https://Snort.Social",
    "https://snort.social:443",
    "https://snort.social:0443",
    "http://localhost:80",
    "https://[2001:DB8::1]",
    /* loopback look-alikes that are not on the http allowlist */
    "http://[::ffff:127.0.0.1]",
    "http://localhost.",
    "http://0x7f.0.0.1",
    "https://sn%6Frt.social",
    "https://[fe80::1%25eth0]",
  };
  for (gsize i = 0; i < G_N_ELEMENTS(bad); i++) {
    const gchar *why = NULL;
    g_autofree gchar *app = nm_origin_to_app_id(bad[i], &why);
    if (app) g_error("origin %s accepted as %s", bad[i] ? bad[i] : "(null)", app);
    g_assert_nonnull(why);
  }
  g_autofree gchar *longo = g_strdup_printf("https://%0*d.example", NM_MAX_ORIGIN_LEN, 0);
  g_assert_null(nm_origin_to_app_id(longo, NULL));
}

static JsonNode *parse(const gchar *json) {
  g_autoptr(JsonParser) p = json_parser_new();
  g_assert_true(json_parser_load_from_data(p, json, -1, NULL));
  return json_node_ref(json_parser_get_root(p));
}

static void test_event_canonical(void) {
  JsonNode *n = parse("{\"kind\":1,\"created_at\":1700000000,\"tags\":[[\"e\",\"aa\",\"\"],[]],"
                      "\"content\":\"hi \\\"there\\\"\",\"pubkey\":\"" PK_HEX "\","
                      "\"id\":\"junk\",\"sig\":\"junk\",\"extra\":{\"x\":1}}");
  const gchar *why = NULL;
  g_autofree gchar *ev = nm_event_canonicalize(n, 42, &why);
  g_assert_nonnull(ev);
  g_assert_cmpstr(ev, ==,
    "{\"kind\":1,\"created_at\":1700000000,\"tags\":[[\"e\",\"aa\",\"\"],[]],\"content\":\"hi \\\"there\\\"\"}");
  json_node_unref(n);

  /* created_at 0 -> now (the signer would substitute its own clock) */
  n = parse("{\"kind\":1,\"created_at\":0,\"tags\":[],\"content\":\"\"}");
  g_autofree gchar *ev0 = nm_event_canonicalize(n, 99, &why);
  g_assert_cmpstr(ev0, ==, "{\"kind\":1,\"created_at\":99,\"tags\":[],\"content\":\"\"}");
  json_node_unref(n);

  /* created_at absent -> now */
  n = parse("{\"kind\":30023,\"tags\":[],\"content\":\"\"}");
  g_autofree gchar *ev2 = nm_event_canonicalize(n, 1234, &why);
  g_assert_cmpstr(ev2, ==, "{\"kind\":30023,\"created_at\":1234,\"tags\":[],\"content\":\"\"}");
  json_node_unref(n);
}

static void test_event_rejects(void) {
  const gchar *bad[] = {
    "[]",
    "\"str\"",
    "{\"tags\":[],\"content\":\"\"}",                                  /* no kind */
    "{\"kind\":\"1\",\"tags\":[],\"content\":\"\"}",                   /* kind string */
    "{\"kind\":1.5,\"tags\":[],\"content\":\"\"}",                     /* kind float */
    "{\"kind\":-1,\"tags\":[],\"content\":\"\"}",
    "{\"kind\":65536,\"tags\":[],\"content\":\"\"}",
    "{\"kind\":true,\"tags\":[],\"content\":\"\"}",
    "{\"kind\":1,\"content\":\"\"}",                                   /* no tags */
    "{\"kind\":1,\"tags\":{},\"content\":\"\"}",
    "{\"kind\":1,\"tags\":[\"e\"],\"content\":\"\"}",                  /* tag not array */
    "{\"kind\":1,\"tags\":[[\"e\",1]],\"content\":\"\"}",              /* tag item int */
    "{\"kind\":1,\"tags\":[[\"e\",null]],\"content\":\"\"}",
    "{\"kind\":1,\"tags\":[[[\"e\"]]],\"content\":\"\"}",
    "{\"kind\":1,\"tags\":[]}",                                        /* no content */
    "{\"kind\":1,\"tags\":[],\"content\":42}",
    "{\"kind\":1,\"tags\":[],\"content\":\"\",\"created_at\":\"1\"}",
    "{\"kind\":1,\"tags\":[],\"content\":\"\",\"created_at\":1.0}",
    "{\"kind\":1,\"tags\":[],\"content\":\"\",\"created_at\":-5}",
    "{\"kind\":1,\"tags\":[],\"content\":\"\",\"pubkey\":\"abc\"}",
    "{\"kind\":1,\"tags\":[],\"content\":\"\",\"pubkey\":7}",
  };
  for (gsize i = 0; i < G_N_ELEMENTS(bad); i++) {
    JsonNode *n = parse(bad[i]);
    const gchar *why = NULL;
    g_autofree gchar *ev = nm_event_canonicalize(n, 1, &why);
    if (ev) g_error("accepted malformed event %s -> %s", bad[i], ev);
    g_assert_nonnull(why);
    json_node_unref(n);
  }
  g_assert_null(nm_event_canonicalize(NULL, 1, NULL));
}

static void test_hex_and_npub(void) {
  g_assert_true(nm_is_hex64(PK_HEX));
  g_assert_false(nm_is_hex64(PK_HEX "0"));
  g_assert_false(nm_is_hex64("f9308a"));
  g_assert_false(nm_is_hex64(NULL));
  g_autofree gchar *up = g_ascii_strup(PK_HEX, -1);
  g_autofree gchar *norm = nm_pubkey_normalize(up);
  g_assert_cmpstr(norm, ==, PK_HEX);

  g_autofree gchar *hex = nm_pubkey_to_hex(PK_NPUB);
  g_assert_cmpstr(hex, ==, PK_HEX);
  g_autofree gchar *upn = g_ascii_strup(PK_NPUB, -1);
  g_autofree gchar *hex2 = nm_pubkey_to_hex(upn);
  g_assert_cmpstr(hex2, ==, PK_HEX);
  g_autofree gchar *hex3 = nm_pubkey_to_hex(up);
  g_assert_cmpstr(hex3, ==, PK_HEX);

  /* checksum flip, mixed case, wrong hrp, wrong length */
  gchar *flip = g_strdup(PK_NPUB);
  flip[strlen(flip) - 1] = flip[strlen(flip) - 1] == 'q' ? 'p' : 'q';
  g_assert_null(nm_pubkey_to_hex(flip));
  flip[5] = g_ascii_toupper(flip[5]);
  g_assert_null(nm_pubkey_to_hex(flip));
  g_free(flip);
  g_assert_null(nm_pubkey_to_hex("nsec1lycg5qvjtrp3qjf5f7zl382j9x6nrjz9sdhenvyxq8c3808qxmus6gq266"));
  g_assert_null(nm_pubkey_to_hex("npub1abc"));
  g_assert_null(nm_pubkey_to_hex(NULL));
}

static void test_relays_shape(void) {
  JsonNode *n = nm_relays_to_nip07("[\"wss://relay.example\",\"wss://nos.lol\",\"https://no\",5]");
  g_assert_nonnull(n);
  g_autoptr(JsonGenerator) g = json_generator_new();
  json_generator_set_root(g, n);
  g_autofree gchar *s = json_generator_to_data(g, NULL);
  g_assert_cmpstr(s, ==,
    "{\"wss://relay.example\":{\"read\":true,\"write\":true},\"wss://nos.lol\":{\"read\":true,\"write\":true}}");
  json_node_unref(n);

  n = nm_relays_to_nip07("{\"wss://a\":{\"read\":true,\"write\":false}}");
  g_assert_true(JSON_NODE_HOLDS_OBJECT(n));
  json_node_unref(n);

  g_assert_null(nm_relays_to_nip07("not json"));
  g_assert_null(nm_relays_to_nip07("42"));
}

static void test_event_id(void) {
  JsonNode *tags = parse("[[\"t\",\"nip07\"]]");
  g_autofree gchar *id = nm_event_id(PK_HEX, 1700000000, 1, tags, "hello from the bridge", TRUE);
  g_assert_cmpstr(id, ==, "f499a1ea25941d4d7c0f68aca9dfcd888393bcd08ede3f4f0aae99241216aa56");
  json_node_unref(tags);

  /* NIP-01 escapes + \u00xx for other controls (libnostr: DEL too) */
  tags = parse("[]");
  g_autofree gchar *id2 = nm_event_id(PK_HEX, 1, 1, tags, "a\n\"b\"\x01\x7f", TRUE);
  g_assert_cmpstr(id2, ==, "8ad31e79f2b821becb55faee30b074597dc902b8550d5ca6a0312c247969e990");
  g_autofree gchar *id3 = nm_event_id(PK_HEX, 1, 1, tags, "a\n\"b\"\x01\x7f", FALSE);
  g_assert_cmpstr(id3, !=, id2);
  json_node_unref(tags);
}

static gchar *signed_json(const gchar *id, const gchar *content, gint64 kind, gint64 ca, const gchar *tags) {
  return g_strdup_printf("{\"id\":\"%s\",\"pubkey\":\"" PK_HEX "\",\"created_at\":%" G_GINT64_FORMAT
                         ",\"kind\":%" G_GINT64_FORMAT ",\"tags\":%s,\"content\":\"%s\",\"sig\":\"%0128d\"}",
                         id, ca, kind, tags, content, 0);
}

static void test_signed_event_matches(void) {
  const gchar *sent = "{\"kind\":1,\"created_at\":1700000000,\"tags\":[[\"t\",\"nip07\"]],\"content\":\"hello from the bridge\"}";
  const gchar *good_id = "f499a1ea25941d4d7c0f68aca9dfcd888393bcd08ede3f4f0aae99241216aa56";
  struct { gchar *json; gboolean ok; } cases[] = {
    { signed_json(good_id, "hello from the bridge", 1, 1700000000, "[[\"t\",\"nip07\"]]"), TRUE },
    { signed_json(good_id, "hello from the bridgE", 1, 1700000000, "[[\"t\",\"nip07\"]]"), FALSE },
    { signed_json(good_id, "hello from the bridge", 7, 1700000000, "[[\"t\",\"nip07\"]]"), FALSE },
    { signed_json(good_id, "hello from the bridge", 1, 1700000001, "[[\"t\",\"nip07\"]]"), FALSE },
    { signed_json(good_id, "hello from the bridge", 1, 1700000000, "[[\"t\",\"nip0\"]]"), FALSE },
    { signed_json(good_id, "hello from the bridge", 1, 1700000000, "[]"), FALSE },
    { signed_json("0f499a1ea25941d4d7c0f68aca9dfcd888393bcd08ede3f4f0aae99241216aa5", "hello from the bridge", 1, 1700000000, "[[\"t\",\"nip07\"]]"), FALSE },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(cases); i++) {
    JsonNode *n = parse(cases[i].json);
    const gchar *why = NULL;
    gboolean ok = nm_signed_event_matches(sent, n, &why);
    if (ok != cases[i].ok) g_error("case %" G_GSIZE_FORMAT ": want %d got %d (%s)", i, cases[i].ok, ok, why);
    json_node_unref(n);
    g_free(cases[i].json);
  }
  JsonNode *bare = parse("\"deadbeef\"");
  g_assert_false(nm_signed_event_matches(sent, bare, NULL));
  json_node_unref(bare);
}

static void test_nul_escape(void) {
  const gchar *yes[] = { "\"a\\u0000b\"", "\"\\\\\\u0000\"", "{\"k\":\"\\u0000\"}" };
  const gchar *no[] = { "\"a\\\\u0000b\"", "\"\\u0001\"", "\"u0000\"", "\"\\\\\"" };
  for (gsize i = 0; i < G_N_ELEMENTS(yes); i++)
    g_assert_true(nm_json_has_nul_escape(yes[i], strlen(yes[i])));
  for (gsize i = 0; i < G_N_ELEMENTS(no); i++)
    g_assert_false(nm_json_has_nul_escape(no[i], strlen(no[i])));
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nmh/policy/origin-accepts", test_origin_accepts);
  g_test_add_func("/nmh/policy/origin-refuses", test_origin_refuses);
  g_test_add_func("/nmh/policy/event-canonical", test_event_canonical);
  g_test_add_func("/nmh/policy/event-rejects", test_event_rejects);
  g_test_add_func("/nmh/policy/hex-and-npub", test_hex_and_npub);
  g_test_add_func("/nmh/policy/relays-shape", test_relays_shape);
  g_test_add_func("/nmh/policy/event-id", test_event_id);
  g_test_add_func("/nmh/policy/signed-event-matches", test_signed_event_matches);
  g_test_add_func("/nmh/policy/nul-escape", test_nul_escape);
  return g_test_run();
}
