/* NIP-89 suggestions (nostrc-prqu.1): parsing, templates, ranking, cache. */
#include "nd-event.h"
#include "nd-nip89.h"
#include "nd-uri.h"

#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

#include "nostr-event.h"
#include "nostr-keys.h"
#include "nostr-tag.h"

static char *sk_a, *pk_a, *sk_b, *pk_b, *sk_c, *pk_c;

static char *sign(const char *sk, const char *pk, int kind, gint64 at, const char *content,
                  NostrTags *tags) {
  NostrEvent *e = nostr_event_new();
  nostr_event_set_pubkey(e, pk);
  nostr_event_set_kind(e, kind);
  nostr_event_set_created_at(e, at);
  nostr_event_set_content(e, content ? content : "");
  nostr_event_set_tags(e, tags ? tags : nostr_tags_new(0));
  g_assert_cmpint(nostr_event_sign(e, sk), ==, 0);
  char *json = nostr_event_serialize_compact(e);
  nostr_event_free(e);
  char *out = g_strdup(json);
  free(json);
  return out;
}

/* A 31990 for kind 30311 with three web templates and a flatpak hint. */
static char *handler_event(const char *sk, const char *pk, const char *d, gint64 at,
                           const char *content) {
  return sign(sk, pk, 31990, at, content,
              nostr_tags_new(6, nostr_tag_new("d", d, NULL), nostr_tag_new("k", "30311", NULL),
                             nostr_tag_new("web", "https://live.example/a/<bech32>", "naddr", NULL),
                             nostr_tag_new("web", "https://live.example/e/<bech32>", "nevent", NULL),
                             nostr_tag_new("web", "https://live.example/<bech32>", NULL),
                             nostr_tag_new("flatpak", "org.example.Live", NULL)));
}

static void test_parse(void) {
  g_autofree char *json =
      handler_event(sk_a, pk_a, "live", 1700000000,
                    "{\"name\":\"Live\\u202e\\nStream\",\"display_name\":\"\"}");
  g_autoptr(GPtrArray) keep = g_ptr_array_new_with_free_func(nd_nip89_handler_free);
  NdNip89Handler *h = nd_nip89_handler_from_json(json, 30311);
  g_assert_nonnull(h);
  g_ptr_array_add(keep, h);
  g_autofree char *addr = g_strdup_printf("31990:%s:live", pk_a);
  g_assert_cmpstr(h->address, ==, addr);
  g_assert_cmpstr(h->name, ==, "LiveStream"); /* bidi + newline stripped */
  g_assert_cmpstr(h->app_id, ==, "org.example.Live");
  g_assert_cmpuint(h->web->len, ==, 3);
  g_assert_cmpint(h->created_at, ==, 1700000000);

  /* Wrong kind asked, tampered, not a 31990. */
  g_assert_null(nd_nip89_handler_from_json(json, 1));
  g_autofree char *tampered = g_strdup(json);
  char *p = strstr(tampered, "live.example");
  p[0] = 'L';
  g_assert_null(nd_nip89_handler_from_json(tampered, 30311));
  g_autofree char *note = sign(sk_a, pk_a, 1, 1700000000, "x",
                               nostr_tags_new(1, nostr_tag_new("k", "30311", NULL)));
  g_assert_null(nd_nip89_handler_from_json(note, 30311));

  /* Invalid app id hint is ignored. */
  g_autofree char *bad = sign(sk_a, pk_a, 31990, 1700000000, "",
                              nostr_tags_new(3, nostr_tag_new("d", "x", NULL),
                                             nostr_tag_new("k", "30311", NULL),
                                             nostr_tag_new("linux", "../../etc", NULL)));
  NdNip89Handler *hb = nd_nip89_handler_from_json(bad, 30311);
  g_assert_nonnull(hb);
  g_assert_null(hb->app_id);
  g_assert_null(hb->name);
  nd_nip89_handler_free(hb);
}

static void test_web_url(void) {
  g_autofree char *json = handler_event(sk_a, pk_a, "live", 1700000000, "");
  NdNip89Handler *h = nd_nip89_handler_from_json(json, 30311);

  NdTarget addr = {.entity = ND_ENTITY_ADDRESS, .pubkey_hex = pk_b, .identifier = "stream1",
                   .kind = 30311};
  g_autofree char *u1 = nd_nip89_web_url(h, &addr);
  g_assert_nonnull(u1);
  g_assert_true(g_str_has_prefix(u1, "https://live.example/a/naddr1"));
  g_autofree char *host = nd_nip89_url_host(u1);
  g_assert_cmpstr(host, ==, "live.example");

  /* Profile: no nprofile/npub marker -> the unmarked template. */
  NdTarget prof = {.entity = ND_ENTITY_PROFILE, .pubkey_hex = pk_b, .kind = 0};
  g_autofree char *u2 = nd_nip89_web_url(h, &prof);
  g_assert_nonnull(u2);
  g_assert_true(g_str_has_prefix(u2, "https://live.example/npub1"));
  nd_nip89_handler_free(h);

  /* http://, userinfo and javascript: templates are never used. */
  const char *bad[] = {"http://x.example/<bech32>", "https://user@x.example/<bech32>",
                       "javascript:alert('<bech32>')", "https:///<bech32>", NULL};
  for (guint i = 0; bad[i]; i++) {
    g_autofree char *ev = sign(sk_a, pk_a, 31990, 1700000000, "",
                               nostr_tags_new(3, nostr_tag_new("d", "b", NULL),
                                              nostr_tag_new("k", "30311", NULL),
                                              nostr_tag_new("web", bad[i], NULL)));
    NdNip89Handler *hb = nd_nip89_handler_from_json(ev, 30311);
    g_assert_nonnull(hb);
    g_autofree char *u = nd_nip89_web_url(hb, &addr);
    g_assert_null(u);
    nd_nip89_handler_free(hb);
  }
}

static NdEvent *parsed(char *json) {
  NdEvent *ev = nd_event_parse(json, -1, NULL);
  g_free(json);
  return ev;
}

static void test_rank(void) {
  g_autoptr(GPtrArray) hs = g_ptr_array_new_with_free_func(nd_nip89_handler_free);
  /* A: newest, unrecommended. B: older, recommended twice (+1 duplicate).
   * A': an older revision of A (same address) that must be dropped. */
  g_autofree char *a = handler_event(sk_a, pk_a, "live", 1700000300, "{\"name\":\"A\"}");
  g_autofree char *a_old = handler_event(sk_a, pk_a, "live", 1700000100, "{\"name\":\"A-old\"}");
  g_autofree char *b = handler_event(sk_b, pk_b, "zs", 1700000000, "{\"name\":\"B\"}");
  g_ptr_array_add(hs, nd_nip89_handler_from_json(a_old, 30311));
  g_ptr_array_add(hs, nd_nip89_handler_from_json(a, 30311));
  g_ptr_array_add(hs, nd_nip89_handler_from_json(b, 30311));
  g_autofree char *baddr = g_strdup_printf("31990:%s:zs", pk_b);
  g_autoptr(GPtrArray) recs = g_ptr_array_new_with_free_func((GDestroyNotify)nd_event_free);
  g_ptr_array_add(recs, parsed(sign(sk_c, pk_c, 31989, 1, "",
                                    nostr_tags_new(2, nostr_tag_new("d", "30311", NULL),
                                                   nostr_tag_new("a", baddr, NULL)))));
  g_ptr_array_add(recs, parsed(sign(sk_c, pk_c, 31989, 2, "",
                                    nostr_tags_new(2, nostr_tag_new("d", "30311", NULL),
                                                   nostr_tag_new("a", baddr, NULL)))));
  g_ptr_array_add(recs, parsed(sign(sk_a, pk_a, 31989, 1, "",
                                    nostr_tags_new(2, nostr_tag_new("d", "30311", NULL),
                                                   nostr_tag_new("a", baddr, NULL)))));
  /* A recommendation for another kind does not count. */
  g_autofree char *aaddr = g_strdup_printf("31990:%s:live", pk_a);
  g_ptr_array_add(recs, parsed(sign(sk_b, pk_b, 31989, 1, "",
                                    nostr_tags_new(2, nostr_tag_new("d", "1", NULL),
                                                   nostr_tag_new("a", aaddr, NULL)))));
  nd_nip89_apply_recommendations(hs, recs, 30311);
  nd_nip89_rank(hs);
  g_assert_cmpuint(hs->len, ==, 2);
  const NdNip89Handler *first = g_ptr_array_index(hs, 0), *second = g_ptr_array_index(hs, 1);
  g_assert_cmpstr(first->name, ==, "B");
  g_assert_cmpuint(first->recommended_by, ==, 2);
  g_assert_cmpstr(second->name, ==, "A"); /* the newer revision survived */
  g_assert_cmpuint(second->recommended_by, ==, 0);

  g_autofree char *body_b = nd_nip89_offer_body(first, 30311, "https://live.example/x");
  g_assert_nonnull(strstr(body_b, "Recommended by 2 people you follow"));
  g_assert_nonnull(strstr(body_b, "live.example"));
  g_assert_nonnull(strstr(body_b, "org.example.Live"));
  g_autofree char *body_a = nd_nip89_offer_body(second, 30311, NULL);
  g_assert_nonnull(strstr(body_a, "Nobody you follow recommends it"));
  g_autofree char *btn = nd_nip89_offer_button(first, "https://live.example/x");
  g_assert_cmpstr(btn, ==, "Open in B (live.example)");
}

static void test_lists_and_filters(void) {
  g_autofree char *rl = sign(sk_a, pk_a, 10002, 1, "",
                             nostr_tags_new(5, nostr_tag_new("r", "wss://read.example", "read", NULL),
                                            nostr_tag_new("r", "wss://write.example", "write", NULL),
                                            nostr_tag_new("r", "wss://both.example", NULL),
                                            nostr_tag_new("r", "http://nope.example", NULL),
                                            nostr_tag_new("r", "wss://both.example", NULL)));
  g_auto(GStrv) read = nd_nip89_read_relays(rl);
  g_assert_cmpuint(g_strv_length(read), ==, 2);
  g_assert_cmpstr(read[0], ==, "wss://read.example");
  g_assert_cmpstr(read[1], ==, "wss://both.example");

  g_autofree char *k3 = sign(sk_a, pk_a, 3, 1, "",
                             nostr_tags_new(3, nostr_tag_new("p", pk_b, NULL),
                                            nostr_tag_new("p", "NOTHEX", NULL),
                                            nostr_tag_new("p", pk_c, NULL)));
  g_auto(GStrv) f = nd_nip89_follows(k3, 10);
  g_assert_cmpuint(g_strv_length(f), ==, 2);
  g_auto(GStrv) f1 = nd_nip89_follows(k3, 1);
  g_assert_cmpuint(g_strv_length(f1), ==, 1);

  g_autofree char *hf = nd_nip89_handlers_filter(30311);
  g_assert_cmpstr(hf, ==, "{\"kinds\":[31990],\"#k\":[\"30311\"],\"limit\":50}");
  const char *authors[] = {pk_a, NULL};
  g_autofree char *rf = nd_nip89_recommendations_filter(30311, authors);
  g_assert_nonnull(strstr(rf, "\"#d\":[\"30311\"]"));
  g_assert_null(nd_nip89_recommendations_filter(30311, NULL));
}

static void test_cache(void) {
  g_autofree char *dir = g_dir_make_tmp("nd-nip89-XXXXXX", NULL);
  g_autoptr(GPtrArray) hs = g_ptr_array_new_with_free_func(nd_nip89_handler_free);
  g_autofree char *a = handler_event(sk_a, pk_a, "live", 1700000000, "{\"name\":\"A\"}");
  NdNip89Handler *h = nd_nip89_handler_from_json(a, 30311);
  h->recommended_by = 3;
  g_ptr_array_add(hs, h);
  g_assert_true(nd_nip89_cache_store(dir, 30311, hs, 1000, NULL));

  gboolean fresh = FALSE;
  g_autoptr(GPtrArray) back = nd_nip89_cache_load(dir, 30311, 1000 + 60, 0, &fresh);
  g_assert_nonnull(back);
  g_assert_true(fresh);
  g_assert_cmpuint(back->len, ==, 1);
  g_assert_cmpuint(((NdNip89Handler *)g_ptr_array_index(back, 0))->recommended_by, ==, 3);
  g_autoptr(GPtrArray) stale = nd_nip89_cache_load(dir, 30311, 1000 + ND_NIP89_TTL_FOUND_S, 0,
                                                   &fresh);
  g_assert_false(fresh);
  g_assert_null(nd_nip89_cache_load(dir, 1, 1000, 0, NULL)); /* no file */

  /* Tampered cache entries are re-validated away. */
  g_autofree char *path = g_build_filename(dir, "30311.json", NULL);
  g_autofree char *data = NULL;
  g_assert_true(g_file_get_contents(path, &data, NULL, NULL));
  char *p = strstr(data, "live.example");
  p[0] = 'L';
  g_assert_true(g_file_set_contents(path, data, -1, NULL));
  g_autoptr(GPtrArray) tampered = nd_nip89_cache_load(dir, 30311, 1000, 0, NULL);
  g_assert_nonnull(tampered);
  g_assert_cmpuint(tampered->len, ==, 0);
  g_remove(path);
  g_rmdir(dir);
}

/* Only offers the service presented are actionable (review A-1). */
static void test_offer_tokens(void) {
  g_autofree char *dir = g_dir_make_tmp("nd-offer-XXXXXX", NULL);
  g_autofree char *tok = nd_nip89_offer_store(dir, 30311, "31990:aa:x", "nostr:naddr1x",
                                              "org.example.App", 1000, NULL);
  g_assert_nonnull(tok);
  g_assert_cmpuint(strlen(tok), ==, 32);
  g_autofree char *path = g_build_filename(dir, tok, NULL);
  GStatBuf st;
  g_assert_cmpint(g_stat(path, &st), ==, 0);
  g_assert_cmpint(st.st_mode & 0777, ==, 0600);

  guint32 kind = 0;
  char *addr = NULL, *uri = NULL, *app = NULL;
  g_assert_false(nd_nip89_offer_take(dir, "../../etc/passwd", 1000, &kind, &addr, &uri, &app));
  g_assert_false(nd_nip89_offer_take(dir, "00000000000000000000000000000000", 1000, &kind,
                                     &addr, &uri, &app));
  g_assert_true(nd_nip89_offer_take(dir, tok, 1001, &kind, &addr, &uri, &app));
  g_assert_cmpuint(kind, ==, 30311);
  g_assert_cmpstr(addr, ==, "31990:aa:x");
  g_assert_cmpstr(uri, ==, "nostr:naddr1x");
  g_assert_cmpstr(app, ==, "org.example.App");
  g_free(addr); g_free(uri); g_free(app);
  /* single use */
  g_assert_false(nd_nip89_offer_take(dir, tok, 1002, &kind, &addr, &uri, &app));

  /* expired */
  g_autofree char *old = nd_nip89_offer_store(dir, 1, "31990:bb:y", NULL, NULL, 1000, NULL);
  g_assert_false(nd_nip89_offer_take(dir, old, 1000 + ND_NIP89_OFFER_TTL_S + 1, &kind, &addr,
                                     &uri, &app));
  g_rmdir(dir);
}

/* An entity-marked template never serves another entity; the displayed
 * host is the URI's real host. */
static void test_entity_and_host(void) {
  g_autofree char *ev = sign(sk_a, pk_a, 31990, 1700000000, "",
                             nostr_tags_new(3, nostr_tag_new("d", "ev", NULL),
                                            nostr_tag_new("k", "30311", NULL),
                                            nostr_tag_new("web", "https://ev.example/<bech32>",
                                                          "nevent", NULL)));
  NdNip89Handler *h = nd_nip89_handler_from_json(ev, 30311);
  NdTarget addr = {.entity = ND_ENTITY_ADDRESS, .pubkey_hex = pk_b, .identifier = "s",
                   .kind = 30311};
  g_autofree char *u = nd_nip89_web_url(h, &addr);
  g_assert_null(u);
  nd_nip89_handler_free(h);

  g_autofree char *host = nd_nip89_url_host("https://evil.example#@good.example/x");
  g_assert_cmpstr(host, ==, "evil.example");
}

static void test_discover_offline(void) {
  g_autofree char *dir = g_dir_make_tmp("nd-nip89-XXXXXX", NULL);
  NdNip89Options o = {.socket_path = "/nonexistent/relay.sock", .use_network = FALSE,
                      .cache_dir = dir, .no_signer = TRUE, .budget_ms = 500};
  g_autoptr(GPtrArray) hs = nd_nip89_discover_sync(30311, &o, NULL);
  g_assert_cmpuint(hs->len, ==, 0);
  /* The negative result is cached (1 h). */
  gboolean fresh = FALSE;
  g_autoptr(GPtrArray) c = nd_nip89_cache_load(dir, 30311,
                                               g_get_real_time() / G_USEC_PER_SEC, 0, &fresh);
  g_assert_nonnull(c);
  g_assert_true(fresh);
  g_autofree char *path = g_build_filename(dir, "30311.json", NULL);
  g_remove(path);
  g_rmdir(dir);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  sk_a = nostr_key_generate_private();
  pk_a = nostr_key_get_public(sk_a);
  sk_b = nostr_key_generate_private();
  pk_b = nostr_key_get_public(sk_b);
  sk_c = nostr_key_generate_private();
  pk_c = nostr_key_get_public(sk_c);
  g_test_add_func("/nd/nip89/parse", test_parse);
  g_test_add_func("/nd/nip89/web-url", test_web_url);
  g_test_add_func("/nd/nip89/rank", test_rank);
  g_test_add_func("/nd/nip89/lists-filters", test_lists_and_filters);
  g_test_add_func("/nd/nip89/cache", test_cache);
  g_test_add_func("/nd/nip89/discover-offline", test_discover_offline);
  g_test_add_func("/nd/nip89/offer-tokens", test_offer_tokens);
  g_test_add_func("/nd/nip89/entity-and-host", test_entity_and_host);
  int rc = g_test_run();
  free(sk_a); free(pk_a); free(sk_b); free(pk_b); free(sk_c); free(pk_c);
  return rc;
}
