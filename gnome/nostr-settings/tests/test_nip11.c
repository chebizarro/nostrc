/* test_nip11 — NIP-11 parsing and the bounded fetch against an in-process
 * libsoup server (no network). SPDX-License-Identifier: MIT */
#include "nss-nip11.h"

#include <libsoup/soup.h>
#include <string.h>

static void
test_parse(void)
{
  GError *e = NULL;
  g_autoptr(NssNip11Info) i = nss_nip11_parse(
    "{\"name\":\"Damus\",\"description\":\"line1\\nline2\\u0007\",\"software\":"
    "\"git+https://github.com/damus-io/strfry\",\"version\":\"1.0.1\","
    "\"supported_nips\":[42,1,11,11,\"9\",-3,1.5,50],\"contact\":7,"
    "\"limitation\":{\"auth_required\":false},\"pubkey\":\"abcd\"}", -1, &e);
  g_assert_no_error(e);
  g_assert_cmpstr(i->name, ==, "Damus");
  g_assert_cmpstr(i->description, ==, "line1 line2");   /* ctrl dropped, \n → space */
  g_assert_cmpstr(i->software, ==, "git+https://github.com/damus-io/strfry");
  g_assert_cmpstr(i->version, ==, "1.0.1");
  g_assert_null(i->contact);                             /* wrong type → absent */
  g_assert_cmpstr(i->pubkey, ==, "abcd");
  g_autofree gchar *n = nss_nip11_format_nips(i);
  g_assert_cmpstr(n, ==, "1, 11, 42, 50");               /* sorted, unique, ints only */

  g_autoptr(NssNip11Info) empty = nss_nip11_parse("{}", -1, &e);
  g_assert_no_error(e);
  g_assert_null(empty->name);
  g_assert_cmpuint(empty->supported_nips->len, ==, 0);

  g_autofree gchar *longname = g_strnfill(1000, 'x');
  g_autofree gchar *doc = g_strdup_printf("{\"name\":\"%s\"}", longname);
  g_autoptr(NssNip11Info) capped = nss_nip11_parse(doc, -1, NULL);
  g_assert_cmpuint(g_utf8_strlen(capped->name, -1), ==, NSS_NIP11_MAX_STRING + 1);
  g_assert_true(g_str_has_suffix(capped->name, "…"));

  const gchar *bad[] = { "", "[]", "\"x\"", "{", "null" };
  for (gsize k = 0; k < G_N_ELEMENTS(bad); k++) {
    g_assert_null(nss_nip11_parse(bad[k], -1, &e));
    g_assert_error(e, NSS_NIP11_ERROR, NSS_NIP11_ERROR_PARSE);
    g_clear_error(&e);
  }
}

static void
test_http_url(void)
{
  g_autofree gchar *a = nss_nip11_http_url("wss://relay.example/sub", NULL);
  g_assert_cmpstr(a, ==, "https://relay.example/sub");
  g_autofree gchar *b = nss_nip11_http_url("ws://127.0.0.1:7777", NULL);
  g_assert_cmpstr(b, ==, "http://127.0.0.1:7777");
  g_assert_null(nss_nip11_http_url("https://x", NULL));
  g_assert_null(nss_nip11_http_url("wss://", NULL));
}

/* ── server ── */

static gchar *last_accept;
static GPtrArray *paused;

static void
handler(SoupServer *srv, SoupServerMessage *msg, const char *path, GHashTable *q, gpointer d)
{
  (void)srv; (void)q; (void)d;
  g_free(last_accept);
  last_accept = g_strdup(soup_message_headers_get_one(
    soup_server_message_get_request_headers(msg), "Accept"));
  if (g_str_equal(path, "/ok")) {
    const gchar *body = "{\"name\":\"Local\",\"software\":\"nostrc\",\"supported_nips\":[1,11]}";
    soup_server_message_set_status(msg, 200, NULL);
    soup_server_message_set_response(msg, "application/nostr+json", SOUP_MEMORY_COPY,
                                     body, strlen(body));
  } else if (g_str_equal(path, "/big")) {
    gsize n = NSS_NIP11_MAX_BYTES * 2;
    gchar *body = g_malloc(n);
    memset(body, ' ', n);
    body[0] = '{';
    body[n - 1] = '}';
    soup_server_message_set_status(msg, 200, NULL);
    soup_server_message_set_response(msg, "application/nostr+json", SOUP_MEMORY_TAKE, body, n);
  } else if (g_str_equal(path, "/garbage")) {
    soup_server_message_set_status(msg, 200, NULL);
    soup_server_message_set_response(msg, "text/html", SOUP_MEMORY_STATIC, "<html>", 6);
  } else if (g_str_equal(path, "/hang")) {
    soup_server_message_pause(msg);
    g_ptr_array_add(paused, g_object_ref(msg));
  } else {
    soup_server_message_set_status(msg, 404, NULL);
  }
}

typedef struct {
  gboolean      done;
  NssNip11Info *info;
  GError       *error;
} Wait;

static void
on_fetched(GObject *src, GAsyncResult *res, gpointer data)
{
  (void)src;
  Wait *w = data;
  w->info = nss_nip11_fetch_finish(res, &w->error);
  w->done = TRUE;
}

static Wait
fetch(SoupSession *s, const gchar *url, guint timeout, GCancellable *c)
{
  Wait w = { 0 };
  nss_nip11_fetch_async(s, url, timeout, c, on_fetched, &w);
  while (!w.done)
    g_main_context_iteration(NULL, TRUE);
  return w;
}

static gboolean
cancel_later(gpointer c)
{
  g_cancellable_cancel(c);
  return G_SOURCE_REMOVE;
}

static void
test_fetch(void)
{
  paused = g_ptr_array_new_with_free_func(g_object_unref);
  g_autoptr(SoupServer) srv = soup_server_new(NULL, NULL);
  soup_server_add_handler(srv, NULL, handler, NULL, NULL);
  GError *e = NULL;
  g_assert_true(soup_server_listen_local(srv, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &e));
  g_assert_no_error(e);
  GSList *uris = soup_server_get_uris(srv);
  gint port = g_uri_get_port(uris->data);
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
  g_autoptr(SoupSession) s = soup_session_new();

  g_autofree gchar *ok = g_strdup_printf("ws://127.0.0.1:%d/ok", port);
  Wait w = fetch(s, ok, 5, NULL);
  g_assert_no_error(w.error);
  g_assert_cmpstr(w.info->name, ==, "Local");
  g_assert_cmpstr(last_accept, ==, "application/nostr+json");
  nss_nip11_info_free(w.info);

  g_autofree gchar *big = g_strdup_printf("ws://127.0.0.1:%d/big", port);
  w = fetch(s, big, 5, NULL);
  g_assert_error(w.error, NSS_NIP11_ERROR, NSS_NIP11_ERROR_TOO_LARGE);
  g_clear_error(&w.error);

  g_autofree gchar *nf = g_strdup_printf("ws://127.0.0.1:%d/missing", port);
  w = fetch(s, nf, 5, NULL);
  g_assert_error(w.error, NSS_NIP11_ERROR, NSS_NIP11_ERROR_HTTP);
  g_assert_nonnull(strstr(w.error->message, "404"));
  g_clear_error(&w.error);

  g_autofree gchar *gb = g_strdup_printf("ws://127.0.0.1:%d/garbage", port);
  w = fetch(s, gb, 5, NULL);
  g_assert_error(w.error, NSS_NIP11_ERROR, NSS_NIP11_ERROR_PARSE);
  g_clear_error(&w.error);

  g_autofree gchar *hang = g_strdup_printf("ws://127.0.0.1:%d/hang", port);
  gint64 t0 = g_get_monotonic_time();
  w = fetch(s, hang, 1, NULL);
  gint64 took = g_get_monotonic_time() - t0;
  g_assert_error(w.error, NSS_NIP11_ERROR, NSS_NIP11_ERROR_TIMEOUT);
  g_assert_cmpint(took, <, 4 * G_USEC_PER_SEC);
  g_clear_error(&w.error);

  g_autoptr(GCancellable) c = g_cancellable_new();
  g_timeout_add(100, cancel_later, c);
  w = fetch(s, hang, 30, c);
  g_assert_error(w.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_clear_error(&w.error);

  w = fetch(s, "https://not-a-relay", 5, NULL);
  g_assert_error(w.error, NSS_NIP11_ERROR, NSS_NIP11_ERROR_BAD_URL);
  g_clear_error(&w.error);

  for (guint i = 0; i < paused->len; i++)
    soup_server_message_unpause(g_ptr_array_index(paused, i));
  g_ptr_array_unref(paused);
  g_clear_pointer(&last_accept, g_free);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nostr-settings/nip11/parse", test_parse);
  g_test_add_func("/nostr-settings/nip11/http-url", test_http_url);
  g_test_add_func("/nostr-settings/nip11/fetch", test_fetch);
  return g_test_run();
}
