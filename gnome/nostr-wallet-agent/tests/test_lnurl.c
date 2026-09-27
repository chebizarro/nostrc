/* test_lnurl.c - LNURL-pay / Lightning address resolution, request
 * parsing, invoice checks, and the HTTP fetch against an in-process
 * libsoup server on loopback (allowed in test builds only).
 *
 * SPDX-License-Identifier: MIT
 */
#include "nwa-error.h"
#include "nwa-lnurl.h"
#include "nwa-test-bolt11.h"

#include <string.h>
#include <time.h>

/* bech32-encode @url with hrp "lnurl" (LUD-01), upper case like QR codes */
static gchar *
lnurl_encode(const gchar *url)
{
  GByteArray *w = g_byte_array_new();
  nwa_test_words_from_bytes(w, (const guint8 *)url, strlen(url));
  const gchar *hrp = "lnurl";
  GByteArray *chk = g_byte_array_new();
  guint8 zero = 0;
  for (const gchar *p = hrp; *p; p++) { guint8 x = (guint8)(*p >> 5); g_byte_array_append(chk, &x, 1); }
  g_byte_array_append(chk, &zero, 1);
  for (const gchar *p = hrp; *p; p++) { guint8 x = (guint8)(*p & 31); g_byte_array_append(chk, &x, 1); }
  g_byte_array_append(chk, w->data, w->len);
  for (int i = 0; i < 6; i++) g_byte_array_append(chk, &zero, 1);
  guint32 pm = nwa_test_b32_polymod(chk->data, chk->len) ^ 1;
  GString *s = g_string_new("lnurl1");
  for (guint i = 0; i < w->len; i++) g_string_append_c(s, nwa_test_b32[w->data[i]]);
  for (int i = 0; i < 6; i++) g_string_append_c(s, nwa_test_b32[(pm >> (5 * (5 - i))) & 31]);
  g_byte_array_free(chk, TRUE);
  g_byte_array_free(w, TRUE);
  gchar *up = g_ascii_strup(s->str, -1);
  g_string_free(s, TRUE);
  return up;
}

static void
test_targets(void)
{
  g_autoptr(GError) e = NULL;
  /* LUD-01 example */
  g_autofree gchar *u1 = nwa_lnurl_target_url(
    "LNURL1DP68GURN8GHJ7UM9WFMXJCM99E3K7MF0V9CXJ0M385EKVCENXC6R2C35XVUKXEFCV5MKVV34X5EKZD3EV56NYD3HXQURZEPEXEJXXEPNXSCRVWFNV9NXZCN9XQ6XYEFHVGCXXCMYXYMNSERXFQ5FNS", &e);
  g_assert_no_error(e);
  g_assert_cmpstr(u1, ==, "https://service.com/api?q=3fc3645b439ce8e7f2553a69e5267081d96dcd340693afabe04be7b0ccd178df");
  g_autofree gchar *enc = lnurl_encode("https://pay.example/lnurlp/abc?x=1");
  g_autofree gchar *low = g_ascii_strdown(enc, -1);
  g_autofree gchar *u2 = nwa_lnurl_target_url(low, &e);
  g_assert_no_error(e);
  g_assert_cmpstr(u2, ==, "https://pay.example/lnurlp/abc?x=1");
  g_autofree gchar *u3 = nwa_lnurl_target_url("Alice@Example.COM", &e);
  g_assert_no_error(e);
  g_assert_cmpstr(u3, ==, "https://example.com/.well-known/lnurlp/alice");
  g_autofree gchar *u4 = nwa_lnurl_target_url("bob@abcdefghij.onion", &e);
  g_assert_cmpstr(u4, ==, "http://abcdefghij.onion/.well-known/lnurlp/bob");
#ifdef NWA_ORIGIN_BRIDGE_ENV
  g_autofree gchar *u5 = nwa_lnurl_target_url("carol@127.0.0.1:8080", &e);
  g_assert_cmpstr(u5, ==, "http://127.0.0.1:8080/.well-known/lnurlp/carol");
#endif
  /* plain http to the clearnet, bad addresses, mixed case, other hrp */
  g_autofree gchar *http = lnurl_encode("http://pay.example/x");
  g_assert_null(nwa_lnurl_target_url(http, &e));
  g_assert_error(e, NWA_ERROR, NWA_ERROR_INVALID_ARGS);
  g_clear_error(&e);
  g_assert_null(nwa_lnurl_target_url("al ice@example.com", &e));
  g_clear_error(&e);
  g_assert_null(nwa_lnurl_target_url("a@b@c", &e));
  g_clear_error(&e);
  g_assert_null(nwa_lnurl_target_url("@example.com", &e));
  g_clear_error(&e);
  g_autofree gchar *mixed = g_strdup(enc);
  gchar *letter = mixed + 6;
  while (!g_ascii_isalpha(*letter)) letter++;
  *letter = g_ascii_tolower(*letter);
  g_assert_null(nwa_lnurl_target_url(mixed, &e));
  g_clear_error(&e);
  g_assert_false(nwa_lnurl_url_allowed("https://user:pw@pay.example/"));
  g_assert_false(nwa_lnurl_url_allowed("ftp://pay.example/"));
}

static JsonNode *
json(const gchar *s)
{
  g_autoptr(JsonParser) p = json_parser_new();
  g_assert_true(json_parser_load_from_data(p, s, -1, NULL));
  return json_node_copy(json_parser_get_root(p));
}

#define META "[[\\\"text/plain\\\",\\\"Coffee for Alice\\\"],[\\\"text/long-desc\\\",\\\"Thanks!\\\"],[\\\"text/identifier\\\",\\\"alice@example.com\\\"]]"

static void
test_parse_pay(void)
{
  g_autoptr(GError) e = NULL;
  NwaLnurlPay p;
  g_autoptr(JsonNode) ok = json("{\"tag\":\"payRequest\",\"callback\":\"https://example.com/cb?id=1\","
                                "\"minSendable\":1000,\"maxSendable\":2000000,\"commentAllowed\":32,"
                                "\"metadata\":\"" META "\"}");
  g_assert_true(nwa_lnurl_parse_pay(ok, "https://example.com/.well-known/lnurlp/alice", "alice@example.com", &p, &e));
  g_assert_no_error(e);
  g_assert_cmpuint(p.min_msat, ==, 1000);
  g_assert_cmpuint(p.max_msat, ==, 2000000);
  g_assert_cmpuint(p.comment_allowed, ==, 32);
  g_assert_cmpstr(p.description, ==, "Coffee for Alice");
  g_assert_cmpstr(p.long_description, ==, "Thanks!");
  g_assert_cmpstr(p.identifier, ==, "alice@example.com");
  g_assert_cmpstr(p.domain, ==, "example.com");

  /* callback URL: existing query kept, comment trimmed to 32 chars + escaped */
  g_autofree gchar *cb = nwa_lnurl_callback_url(&p, 21000, "a comment that is far too long to be sent in full&x");
  g_assert_cmpstr(cb, ==, "https://example.com/cb?id=1&amount=21000&comment=a%20comment%20that%20is%20far%20too%20long%20t");
  g_autofree gchar *cb2 = nwa_lnurl_callback_url(&p, 5000, "");
  g_assert_cmpstr(cb2, ==, "https://example.com/cb?id=1&amount=5000");
  p.comment_allowed = 0;
  g_autofree gchar *cb3 = nwa_lnurl_callback_url(&p, 5000, "ignored");
  g_assert_cmpstr(cb3, ==, "https://example.com/cb?id=1&amount=5000");

  /* invoice checks: amount and description_hash = sha256(metadata) */
  g_autofree gchar *h = g_compute_checksum_for_string(G_CHECKSUM_SHA256, p.metadata, -1);
  const gchar *ph = "0001020304050607080900010203040506070809000102030405060708090102";
  gint64 now = (gint64)time(NULL);
  struct { guint64 amt; const gchar *d; const gchar *hh; gboolean good; } cases[] = {
    { 21000, NULL, h, TRUE },
    { 22000, NULL, h, FALSE },                        /* not the approved amount */
    { 21000, "Coffee for Alice", NULL, FALSE },       /* `d`, no commitment */
    { 21000, NULL, ph, FALSE },                       /* commits to something else */
  };
  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    g_autofree gchar *inv = nwa_test_bolt11_mint(cases[i].amt, cases[i].d, cases[i].hh, ph, now, 0);
    NwaBolt11 b;
    g_assert_true(nwa_bolt11_decode(inv, &b, NULL));
    GError *ce = NULL;
    gboolean r = nwa_lnurl_check_invoice(&p, 21000, &b, &ce);
    g_assert_cmpint(r, ==, cases[i].good);
    g_clear_error(&ce);
    nwa_bolt11_clear(&b);
  }
  nwa_lnurl_pay_clear(&p);

  /* refusals */
  const gchar *bad[] = {
    "{\"tag\":\"withdrawRequest\",\"callback\":\"https://e/cb\",\"k1\":\"x\"}",
    "{\"tag\":\"payRequest\",\"callback\":\"https://e/cb\",\"minSendable\":5,\"maxSendable\":4,\"metadata\":\"" META "\"}",
    "{\"tag\":\"payRequest\",\"callback\":\"http://e.example/cb\",\"minSendable\":1,\"maxSendable\":4,\"metadata\":\"" META "\"}",
    "{\"tag\":\"payRequest\",\"callback\":\"https://e/cb\",\"minSendable\":1,\"maxSendable\":4,\"metadata\":\"[]\"}",
    "{\"tag\":\"payRequest\",\"callback\":\"https://e/cb\",\"minSendable\":1,\"maxSendable\":4}",
    "{\"tag\":\"payRequest\",\"callback\":\"https://e/cb\",\"minSendable\":0,\"maxSendable\":4,\"metadata\":\"" META "\"}",
  };
  for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
    g_autoptr(JsonNode) n = json(bad[i]);
    GError *be = NULL;
    g_assert_false(nwa_lnurl_parse_pay(n, "https://e/x", NULL, &p, &be));
    g_assert_nonnull(be);
    if (i == 0) g_assert_error(be, NWA_ERROR, NWA_ERROR_UNSUPPORTED);
    g_clear_error(&be);
  }
  /* an address must match the identifier the server claims */
  GError *me = NULL;
  g_assert_false(nwa_lnurl_parse_pay(ok, "https://example.com/x", "mallory@example.com", &p, &me));
  g_assert_nonnull(strstr(me->message, "another address"));
  g_clear_error(&me);
}

/* ---- fetch ---- */

static void
serve(SoupServer *srv, SoupServerMessage *msg, const char *path, GHashTable *q, gpointer data)
{
  (void)srv; (void)q; (void)data;
  if (g_str_equal(path, "/ok")) {
    soup_server_message_set_status(msg, 200, NULL);
    const gchar *b = "{\"pr\":\"lnbc1x\"}";
    soup_server_message_set_response(msg, "application/json", SOUP_MEMORY_STATIC, b, strlen(b));
  } else if (g_str_equal(path, "/err")) {
    soup_server_message_set_status(msg, 200, NULL);
    const gchar *b = "{\"status\":\"ERROR\",\"reason\":\"amount too small\"}";
    soup_server_message_set_response(msg, "application/json", SOUP_MEMORY_STATIC, b, strlen(b));
  } else if (g_str_equal(path, "/big")) {
    gsize n = NWA_LNURL_MAX_BODY + 10;
    gchar *b = g_malloc(n);
    memset(b, ' ', n);
    b[0] = '{'; b[n - 1] = '}';
    soup_server_message_set_status(msg, 200, NULL);
    soup_server_message_set_response(msg, "application/json", SOUP_MEMORY_TAKE, b, n);
  } else if (g_str_equal(path, "/redir")) {
    soup_server_message_set_redirect(msg, 302, "/ok");
  } else {
    soup_server_message_set_status(msg, 404, NULL);
  }
}

typedef struct { gboolean done; JsonNode *node; GError *err; } Res;

static void
on_fetched(GObject *src, GAsyncResult *r, gpointer d)
{
  (void)src;
  Res *res = d;
  res->node = nwa_lnurl_fetch_json_finish(r, &res->err);
  res->done = TRUE;
}

static void
fetch(SoupSession *ss, const gchar *base, const gchar *path, Res *r)
{
  memset(r, 0, sizeof *r);
  g_autofree gchar *url = g_strconcat(base, path, NULL);
  nwa_lnurl_fetch_json_async(ss, url, NULL, on_fetched, r);
  while (!r->done) g_main_context_iteration(NULL, TRUE);
}

static void
test_fetch(void)
{
#ifndef NWA_ORIGIN_BRIDGE_ENV
  g_test_skip("loopback LNURL servers are only allowed in test builds");
  return;
#endif
  SoupServer *srv = soup_server_new(NULL, NULL);
  soup_server_add_handler(srv, NULL, serve, NULL, NULL);
  g_assert_true(soup_server_listen_local(srv, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, NULL));
  GSList *uris = soup_server_get_uris(srv);
  g_autofree gchar *base = g_strdup_printf("http://127.0.0.1:%d", g_uri_get_port(uris->data));
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
  SoupSession *ss = soup_session_new();
  Res r;

  fetch(ss, base, "/ok", &r);
  g_assert_no_error(r.err);
  g_assert_cmpstr(json_object_get_string_member(json_node_get_object(r.node), "pr"), ==, "lnbc1x");
  json_node_unref(r.node);
  fetch(ss, base, "/err", &r);
  g_assert_error(r.err, NWA_ERROR, NWA_ERROR_WALLET);
  g_assert_cmpstr(r.err->message, ==, "[LNURL] amount too small");
  g_clear_error(&r.err);
  fetch(ss, base, "/big", &r);
  g_assert_nonnull(r.err);
  g_assert_nonnull(strstr(r.err->message, "too large"));
  g_clear_error(&r.err);
  fetch(ss, base, "/redir", &r);
  g_assert_nonnull(r.err);
  g_assert_nonnull(strstr(r.err->message, "redirects are not followed"));
  g_clear_error(&r.err);
  fetch(ss, base, "/missing", &r);
  g_assert_nonnull(strstr(r.err->message, "HTTP 404"));
  g_clear_error(&r.err);
  fetch(ss, "http://pay.example", "/x", &r); /* clearnet http: refused before any I/O */
  g_assert_error(r.err, NWA_ERROR, NWA_ERROR_INVALID_ARGS);
  g_clear_error(&r.err);
  g_object_unref(ss);
  g_object_unref(srv);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/lnurl/targets", test_targets);
  g_test_add_func("/lnurl/parse-pay-and-invoice", test_parse_pay);
  g_test_add_func("/lnurl/fetch", test_fetch);
  return g_test_run();
}
