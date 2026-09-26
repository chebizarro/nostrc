/* test_lists — NIP-65 (10002) and BUD-03 (10063) models, event builders,
 * publish-target rules, and fetch/publish through libnostr-publish fixture
 * transports with an in-process (libnostr key) vtable signer.
 * SPDX-License-Identifier: MIT */
#include "nss-lists.h"
#include "nss-net.h"

#include "nostr-event.h"
#include "nostr-keys.h"

#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <string.h>

static struct { gchar *sk, *pk; } K;

/* ── signer ── */

static gchar *
sign_json(const gchar *unsigned_json)
{
  NostrEvent *ev = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(ev, unsigned_json, NULL), ==, 1);
  g_assert_cmpint(nostr_event_sign(ev, K.sk), ==, 0);
  char *s = nostr_event_serialize_compact(ev);
  nostr_event_free(ev);
  gchar *out = g_strdup(s);
  free(s);
  return out;
}

static gint n_signed;

static gchar *
mock_sign(gpointer ud, const gchar *unsigned_json, GCancellable *c, GError **e)
{
  (void)ud; (void)c; (void)e;
  n_signed++;
  return sign_json(unsigned_json);
}

static gchar *
deny_sign(gpointer ud, const gchar *unsigned_json, GCancellable *c, GError **e)
{
  (void)ud; (void)unsigned_json; (void)c;
  g_set_error_literal(e, NOSTR_PUBLISH_SIGNER_ERROR, NOSTR_PUBLISH_SIGNER_ERROR_DENIED,
                      "user denied");
  return NULL;
}

/* ── fixture relays ── */

static struct {
  GHashTable *canned;     /* "url|kind" → GPtrArray of event JSON */
  GHashTable *reject;     /* url → reason */
  GPtrArray  *published;  /* "url json" */
} R;

static void
relays_reset(void)
{
  g_clear_pointer(&R.canned, g_hash_table_unref);
  g_clear_pointer(&R.reject, g_hash_table_unref);
  g_clear_pointer(&R.published, g_ptr_array_unref);
  R.canned = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                   (GDestroyNotify)g_ptr_array_unref);
  R.reject = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  R.published = g_ptr_array_new_with_free_func(g_free);
}

static void
can(const gchar *url, gint kind, gchar *json)
{
  g_autofree gchar *key = g_strdup_printf("%s|%d", url, kind);
  GPtrArray *a = g_hash_table_lookup(R.canned, key);
  if (a == NULL) {
    a = g_ptr_array_new_with_free_func(g_free);
    g_hash_table_insert(R.canned, g_strdup(key), a);
  }
  g_ptr_array_add(a, json);
}

static void
respond(NostrPublishTransport *t)
{
  const gchar *url = nostr_publish_transport_get_url(t);
  gsize n = 0;
  gchar **frames = nostr_publish_transport_fixture_take_sent(t, &n);
  for (gsize i = 0; i < n; i++) {
    g_autoptr(JsonParser) p = json_parser_new();
    g_assert_true(json_parser_load_from_data(p, frames[i], -1, NULL));
    JsonArray *a = json_node_get_array(json_parser_get_root(p));
    const gchar *cmd = json_array_get_string_element(a, 0);
    if (g_str_equal(cmd, "REQ")) {
      const gchar *sub = json_array_get_string_element(a, 1);
      JsonObject *f = json_array_get_object_element(a, 2);
      gint64 kind = json_array_get_int_element(json_object_get_array_member(f, "kinds"), 0);
      g_autofree gchar *key = g_strdup_printf("%s|%" G_GINT64_FORMAT, url, kind);
      GPtrArray *evs = g_hash_table_lookup(R.canned, key);
      for (guint j = 0; evs && j < evs->len; j++) {
        g_autofree gchar *env = g_strdup_printf("[\"EVENT\",\"%s\",%s]", sub,
                                                (gchar *)g_ptr_array_index(evs, j));
        nostr_publish_transport_fixture_deliver_frame(t, "EVENT", env);
      }
      g_autofree gchar *eose = g_strdup_printf("[\"EOSE\",\"%s\"]", sub);
      nostr_publish_transport_fixture_deliver_frame(t, "EOSE", eose);
    } else if (g_str_equal(cmd, "EVENT")) {
      JsonObject *ev = json_array_get_object_element(a, 1);
      const gchar *id = json_object_get_string_member(ev, "id");
      g_autofree gchar *evjson = json_to_string(json_array_get_element(a, 1), FALSE);
      g_ptr_array_add(R.published, g_strdup_printf("%s %s", url, evjson));
      const gchar *reason = g_hash_table_lookup(R.reject, url);
      g_autofree gchar *ok = g_strdup_printf("[\"OK\",\"%s\",%s,\"%s\"]", id,
                                             reason ? "false" : "true", reason ? reason : "");
      nostr_publish_transport_fixture_deliver_frame(t, "OK", ok);
    }
  }
  g_strfreev(frames);
}

/* The fixture transport reports "connected" as soon as connect_async() is
 * called; this source answers whatever the code under test sent. */
typedef struct { NostrPublishTransport *t; } Responder;

static gboolean
responder_tick(gpointer data)
{
  Responder *r = data;
  respond(r->t);
  return G_SOURCE_CONTINUE;
}

static void
responder_free(gpointer data)
{
  Responder *r = data;
  nostr_publish_transport_unref(r->t);
  g_free(r);
}

static NostrPublishTransport *
fixture_factory(const gchar *url, gpointer ud)
{
  (void)ud;
  NostrPublishTransport *t = nostr_publish_transport_new_fixture(url);
  Responder *r = g_new0(Responder, 1);
  r->t = nostr_publish_transport_ref(t);
  GSource *s = g_timeout_source_new(2);
  g_source_set_callback(s, responder_tick, r, responder_free);
  GMainContext *ctx = g_main_context_get_thread_default();
  g_source_attach(s, ctx ? ctx : g_main_context_default());
  g_source_unref(s);
  return t;
}

static NssNet
fixture_net(void)
{
  NssNet net = { 0 };
  net.factory = fixture_factory;
  return net;
}

static gchar *
signed_event(gint kind, gint64 created_at, const gchar *tags_json, const gchar *sk)
{
  g_autofree gchar *pk = nostr_key_get_public(sk);
  g_autofree gchar *u = g_strdup_printf(
    "{\"pubkey\":\"%s\",\"created_at\":%" G_GINT64_FORMAT ",\"kind\":%d,"
    "\"tags\":%s,\"content\":\"\"}", pk, created_at, kind, tags_json);
  gchar *saved = K.sk;
  K.sk = (gchar *)sk;
  gchar *out = sign_json(u);
  K.sk = saved;
  return out;
}

/* ── pure ── */

static void
test_urls(void)
{
  struct { const gchar *in, *out; } ok[] = {
    { "wss://relay.damus.io", "wss://relay.damus.io" },
    { "  WSS://Relay.Example.COM/path  ", "wss://relay.example.com/path" },
    { "ws://localhost:7777", "ws://localhost:7777" },
    { "wss://relay.example/", "wss://relay.example/" },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(ok); i++) {
    g_autofree gchar *n = nss_relay_url_normalize(ok[i].in, NULL);
    g_assert_cmpstr(n, ==, ok[i].out);
  }
  const gchar *bad[] = { "", "relay.damus.io", "https://relay.example", "wss://",
                         "wss://user:pw@relay.example", "wss://r.example/?x=1",
                         "wss://r.example/#f", "wss://r example" };
  for (gsize i = 0; i < G_N_ELEMENTS(bad); i++) {
    GError *e = NULL;
    g_assert_null(nss_relay_url_normalize(bad[i], &e));
    g_assert_error(e, NSS_LISTS_ERROR, NSS_LISTS_ERROR_BAD_URL);
    g_clear_error(&e);
  }
  g_autofree gchar *b = nss_blossom_url_normalize("https://Blossom.Example//", NULL);
  g_assert_cmpstr(b, ==, "https://blossom.example");
  g_assert_null(nss_blossom_url_normalize("http://blossom.example", NULL));
  g_assert_null(nss_blossom_url_normalize("wss://blossom.example", NULL));
}

static void
test_relay_list_build_parse(void)
{
  g_autoptr(GPtrArray) l = g_ptr_array_new_with_free_func((GDestroyNotify)nss_relay_entry_free);
  g_ptr_array_add(l, nss_relay_entry_new("wss://both.example", TRUE, TRUE));
  g_ptr_array_add(l, nss_relay_entry_new("wss://read.example", TRUE, FALSE));
  g_ptr_array_add(l, nss_relay_entry_new("wss://write.example", FALSE, TRUE));
  GError *e = NULL;
  g_autofree gchar *json = nss_relay_list_build(l, K.pk, 1700000000, &e);
  g_assert_no_error(e);
  g_assert_nonnull(strstr(json, "\"kind\":10002"));
  g_assert_nonnull(strstr(json,
    "[[\"r\",\"wss://both.example\"],[\"r\",\"wss://read.example\",\"read\"],"
    "[\"r\",\"wss://write.example\",\"write\"]]"));
  g_assert_nonnull(strstr(json, "\"content\":\"\""));
  /* Signable by a real key and parses back identically. */
  g_autofree gchar *signed_json = sign_json(json);
  g_autoptr(GPtrArray) back = nss_relay_list_parse(signed_json, &e);
  g_assert_no_error(e);
  g_assert_cmpuint(back->len, ==, 3);
  for (guint i = 0; i < 3; i++) {
    NssRelayEntry *a = g_ptr_array_index(l, i), *b = g_ptr_array_index(back, i);
    g_assert_cmpstr(a->url, ==, b->url);
    g_assert_cmpint(a->read, ==, b->read);
    g_assert_cmpint(a->write, ==, b->write);
  }
  /* Also the libnostr-publish NIP-65 reader agrees. */
  g_auto(GStrv) w = nostr_publish_nip65_relays(signed_json, NOSTR_PUBLISH_NIP65_WRITE, NULL);
  g_assert_cmpuint(g_strv_length(w), ==, 2);
  g_assert_cmpstr(w[1], ==, "wss://write.example");

  /* Errors: duplicate, no marker, bad URL. */
  g_ptr_array_add(l, nss_relay_entry_new("WSS://Both.Example", TRUE, FALSE));
  g_assert_null(nss_relay_list_build(l, NULL, 1, &e));
  g_assert_error(e, NSS_LISTS_ERROR, NSS_LISTS_ERROR_DUPLICATE);
  g_clear_error(&e);
  g_ptr_array_remove_index(l, 3);
  g_ptr_array_add(l, nss_relay_entry_new("wss://none.example", FALSE, FALSE));
  g_assert_null(nss_relay_list_build(l, NULL, 1, &e));
  g_assert_error(e, NSS_LISTS_ERROR, NSS_LISTS_ERROR_NO_MARKER);
  g_clear_error(&e);

  /* Parser tolerance: duplicates merge markers, junk skipped, wrong kind fails. */
  g_autoptr(GPtrArray) m = nss_relay_list_parse(
    "{\"kind\":10002,\"tags\":[[\"r\",\"wss://x.example\",\"read\"],"
    "[\"r\",\"wss://x.example\",\"write\"],[\"r\",\"https://nope\"],[\"p\",\"ab\"],"
    "[\"r\",\"wss://y.example\",\"bogus\"],[\"r\"],7]}", &e);
  g_assert_no_error(e);
  g_assert_cmpuint(m->len, ==, 1);
  NssRelayEntry *x = g_ptr_array_index(m, 0);
  g_assert_true(x->read && x->write);
  g_assert_null(nss_relay_list_parse("{\"kind\":3,\"tags\":[]}", &e));
  g_assert_error(e, NSS_LISTS_ERROR, NSS_LISTS_ERROR_BAD_EVENT);
  g_clear_error(&e);
  g_assert_null(nss_relay_list_parse("not json", NULL));
}

static void
test_blossom_build_parse(void)
{
  const gchar *servers[] = { "https://one.example/", "https://two.example", NULL };
  GError *e = NULL;
  g_autofree gchar *json = nss_blossom_list_build(servers, K.pk, 1700000000, &e);
  g_assert_no_error(e);
  g_assert_nonnull(strstr(json, "\"kind\":10063"));
  g_assert_nonnull(strstr(json,
    "[[\"server\",\"https://one.example\"],[\"server\",\"https://two.example\"]]"));
  g_autofree gchar *signed_json = sign_json(json);
  g_auto(GStrv) back = nss_blossom_list_parse(signed_json, &e);
  g_assert_no_error(e);
  g_assert_cmpuint(g_strv_length(back), ==, 2);
  g_assert_cmpstr(back[0], ==, "https://one.example");

  const gchar *none[] = { NULL };
  g_assert_null(nss_blossom_list_build(none, NULL, 1, &e));
  g_assert_error(e, NSS_LISTS_ERROR, NSS_LISTS_ERROR_EMPTY);
  g_clear_error(&e);
  const gchar *dup[] = { "https://a.example", "https://A.example/", NULL };
  g_assert_null(nss_blossom_list_build(dup, NULL, 1, &e));
  g_assert_error(e, NSS_LISTS_ERROR, NSS_LISTS_ERROR_DUPLICATE);
  g_clear_error(&e);
  const gchar *insecure[] = { "http://a.example", NULL };
  g_assert_null(nss_blossom_list_build(insecure, NULL, 1, &e));
  g_clear_error(&e);
}

static void
test_targets(void)
{
  g_autoptr(GPtrArray) newl = g_ptr_array_new_with_free_func((GDestroyNotify)nss_relay_entry_free);
  g_ptr_array_add(newl, nss_relay_entry_new("wss://w1", TRUE, TRUE));
  g_ptr_array_add(newl, nss_relay_entry_new("wss://r1", TRUE, FALSE));
  g_autoptr(GPtrArray) oldl = g_ptr_array_new_with_free_func((GDestroyNotify)nss_relay_entry_free);
  g_ptr_array_add(oldl, nss_relay_entry_new("wss://removed", FALSE, TRUE));
  g_ptr_array_add(oldl, nss_relay_entry_new("wss://w1", TRUE, TRUE));
  const gchar *signer[] = { "wss://s1", "wss://w1", NULL };
  g_auto(GStrv) req = NULL;
  g_auto(GStrv) t = nss_relay_list_publish_targets(newl, oldl, signer, &req);
  const gchar *want[] = { "wss://w1", "wss://r1", "wss://removed", "wss://s1", NULL };
  g_assert_cmpstrv(t, want);
  const gchar *want_req[] = { "wss://w1", NULL };
  g_assert_cmpstrv(req, want_req);

  g_auto(GStrv) req2 = NULL;
  g_auto(GStrv) t2 = nss_blossom_publish_targets(newl, signer, &req2);
  g_assert_cmpstrv(t2, want_req);
  g_assert_cmpstrv(req2, want_req);
  g_auto(GStrv) req3 = NULL;
  g_auto(GStrv) t3 = nss_blossom_publish_targets(NULL, signer, &req3);
  g_assert_cmpstrv(t3, signer);
  g_assert_cmpstrv(req3, signer);
}

/* ── network through fixtures ── */

static void
test_fetch_newest_verified(void)
{
  relays_reset();
  g_autofree gchar *other_sk = nostr_key_generate_private();
  can("wss://a.test", 10002,
      signed_event(10002, 100, "[[\"r\",\"wss://old.example\"]]", K.sk));
  can("wss://b.test", 10002,
      signed_event(10002, 200, "[[\"r\",\"wss://new.example\"]]", K.sk));
  /* Newer, but by someone else: ignored. */
  gchar *forged = signed_event(10002, 900, "[[\"r\",\"wss://evil.example\"]]", other_sk);
  gchar *spoof = g_strdup(forged);
  g_autofree gchar *other_pk = nostr_key_get_public(other_sk);
  gchar *at = strstr(spoof, other_pk);
  memcpy(at, K.pk, 64);                   /* claims to be us; sig no longer verifies */
  can("wss://b.test", 10002, spoof);
  g_free(forged);
  NssNet net = fixture_net();
  const gchar *relays[] = { "wss://a.test", "wss://b.test", NULL };
  g_autofree gchar *src = NULL;
  g_autofree gchar *ev = nss_net_fetch_replaceable(&net, relays, 10002, K.pk, 3000, &src);
  g_assert_nonnull(ev);
  g_assert_cmpstr(src, ==, "wss://b.test");
  g_assert_nonnull(strstr(ev, "new.example"));
  g_autofree gchar *none = nss_net_fetch_replaceable(&net, relays, 10063, K.pk, 3000, NULL);
  g_assert_null(none);
}

static void
test_publish_all_required(void)
{
  relays_reset();
  NssNet net = fixture_net();
  NostrPublishSignerVTable vt = { mock_sign, NULL };
  g_autoptr(NostrPublishSigner) signer = nostr_publish_signer_new_from_vtable(&vt, NULL);
  g_autoptr(GPtrArray) l = g_ptr_array_new_with_free_func((GDestroyNotify)nss_relay_entry_free);
  g_ptr_array_add(l, nss_relay_entry_new("wss://w1.test", TRUE, TRUE));
  g_ptr_array_add(l, nss_relay_entry_new("wss://w2.test", FALSE, TRUE));
  g_autofree gchar *json = nss_relay_list_build(l, K.pk, g_get_real_time() / G_USEC_PER_SEC, NULL);
  const gchar *old[] = { NULL };
  g_auto(GStrv) req = NULL;
  g_auto(GStrv) targets = nss_relay_list_publish_targets(l, NULL, old, &req);
  NssPublishReport r;
  GError *e = NULL;
  n_signed = 0;
  g_assert_true(nss_net_publish(&net, signer, json, (const gchar *const *)targets,
                                (const gchar *const *)req, 5, &r, &e));
  g_assert_no_error(e);
  g_assert_cmpint(n_signed, ==, 1);
  g_assert_cmpuint(r.n_required, ==, 2);
  g_assert_cmpuint(r.n_required_ok, ==, 2);
  g_assert_cmpuint(R.published->len, ==, 2);
  g_assert_nonnull(r.signed_json);
  /* What went out is the signed 10002 and it verifies. */
  NostrEvent *ev = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_signed(ev, r.signed_json, NULL), ==,
                  NOSTR_EVENT_VALIDATION_OK);
  g_assert_cmpint(nostr_event_get_kind(ev), ==, 10002);
  nostr_event_free(ev);
  nss_publish_report_clear(&r);

  /* One write relay rejects → partial: FALSE, report still complete. */
  relays_reset();
  g_hash_table_insert(R.reject, g_strdup("wss://w2.test"), g_strdup("blocked: not allowed"));
  g_assert_false(nss_net_publish(&net, signer, json, (const gchar *const *)targets,
                                 (const gchar *const *)req, 5, &r, &e));
  g_assert_error(e, NSS_NET_ERROR, NSS_NET_ERROR_PUBLISH);
  g_assert_nonnull(strstr(e->message, "1 of 2"));
  g_clear_error(&e);
  g_assert_cmpuint(r.results->len, >=, 1);
  gboolean saw_reject = FALSE;
  for (guint i = 0; i < r.results->len; i++) {
    NssRelayResult *x = g_ptr_array_index(r.results, i);
    if (g_str_equal(x->url, "wss://w2.test")) {
      g_assert_false(x->accepted);
      g_assert_nonnull(strstr(x->detail, "blocked"));
      saw_reject = TRUE;
    }
  }
  g_assert_true(saw_reject);
  nss_publish_report_clear(&r);

  /* Signer refuses → error, nothing sent. */
  relays_reset();
  NostrPublishSignerVTable deny = { deny_sign, NULL };
  g_autoptr(NostrPublishSigner) no = nostr_publish_signer_new_from_vtable(&deny, NULL);
  g_assert_false(nss_net_publish(&net, no, json, (const gchar *const *)targets,
                                 (const gchar *const *)req, 5, &r, &e));
  g_assert_error(e, NSS_NET_ERROR, NSS_NET_ERROR_NO_SIGNER);
  g_clear_error(&e);
  g_assert_cmpuint(R.published->len, ==, 0);
  nss_publish_report_clear(&r);

  /* No targets. */
  const gchar *empty[] = { NULL };
  g_assert_false(nss_net_publish(&net, signer, json, empty, empty, 5, &r, &e));
  g_assert_error(e, NSS_NET_ERROR, NSS_NET_ERROR_NO_RELAYS);
  g_clear_error(&e);
  nss_publish_report_clear(&r);
}

static void
test_publish_blossom(void)
{
  relays_reset();
  NssNet net = fixture_net();
  NostrPublishSignerVTable vt = { mock_sign, NULL };
  g_autoptr(NostrPublishSigner) signer = nostr_publish_signer_new_from_vtable(&vt, NULL);
  const gchar *servers[] = { "https://blossom.test", NULL };
  g_autofree gchar *json = nss_blossom_list_build(servers, K.pk,
                                                  g_get_real_time() / G_USEC_PER_SEC, NULL);
  const gchar *signer_relays[] = { "wss://s1.test", NULL };
  g_auto(GStrv) req = NULL;
  g_auto(GStrv) t = nss_blossom_publish_targets(NULL, signer_relays, &req);
  NssPublishReport r;
  GError *e = NULL;
  g_assert_true(nss_net_publish(&net, signer, json, (const gchar *const *)t,
                                (const gchar *const *)req, 5, &r, &e));
  g_assert_no_error(e);
  g_assert_cmpuint(R.published->len, ==, 1);
  g_assert_nonnull(strstr(g_ptr_array_index(R.published, 0), "\"kind\":10063"));
  nss_publish_report_clear(&r);
  /* And fetching it back (served by the fixture) yields the same list. */
  can("wss://s1.test", 10063, g_strdup(strchr(g_ptr_array_index(R.published, 0), ' ') + 1));
  g_autofree gchar *ev = nss_net_fetch_replaceable(&net, signer_relays, 10063, K.pk, 3000, NULL);
  g_auto(GStrv) back = nss_blossom_list_parse(ev, NULL);
  g_assert_cmpstrv(back, servers);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  K.sk = nostr_key_generate_private();
  K.pk = nostr_key_get_public(K.sk);
  g_setenv("NOSTR_SETTINGS_SESSION_RELAY_SOCKET", "/nonexistent", TRUE);
  g_test_add_func("/nostr-settings/lists/urls", test_urls);
  g_test_add_func("/nostr-settings/lists/10002", test_relay_list_build_parse);
  g_test_add_func("/nostr-settings/lists/10063", test_blossom_build_parse);
  g_test_add_func("/nostr-settings/lists/targets", test_targets);
  g_test_add_func("/nostr-settings/lists/fetch-verified", test_fetch_newest_verified);
  g_test_add_func("/nostr-settings/lists/publish-10002", test_publish_all_required);
  g_test_add_func("/nostr-settings/lists/publish-10063", test_publish_blossom);
  return g_test_run();
}
