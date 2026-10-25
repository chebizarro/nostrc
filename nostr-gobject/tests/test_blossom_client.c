/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <nostr-gobject-1.0/gn-blossom-client.h>
#include <libsoup/soup.h>
#include <nostr/bud02/auth.h>
#include <nostr-event.h>
#include <nostr-keys.h>
#include <stdlib.h>
#include <string.h>

static const gchar payload[] = "exact encrypted wire bytes";
static gchar *base, *hash;
static SoupServer *server;
static GAsyncResult *completed;
static guint upload_count;
static GCancellable *active_cancel;
static gboolean bad_descriptor, auth_refused;
static void done(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source; (void)data;
  g_assert_true(g_main_context_is_owner(g_main_context_default()));
  completed = g_object_ref(result);
}
static void wait_done(void)
{
  while (!completed) g_main_context_iteration(NULL, TRUE);
}
static void handler(SoupServer *s, SoupServerMessage *m, const gchar *path,
                     GHashTable *query, gpointer data)
{
  (void)s; (void)query; (void)data;
  if (g_str_equal(path, "/redirect")) {
    g_autofree gchar *url = g_strdup_printf("%s/%s.bin", base, hash);
    soup_server_message_set_redirect(m, 302, url); return;
  }
  if (g_str_equal(soup_server_message_get_method(m), "PUT")) {
    upload_count++;
    SoupMessageBody *body = soup_server_message_get_request_body(m);
    g_assert_cmpmem(body->data, body->length, payload, sizeof payload - 1);
    const gchar *header = soup_message_headers_get_one(soup_server_message_get_request_headers(m), "Authorization");
    NostrEvent *ev = NULL;
    g_assert_cmpint(nostr_bud02_parse_auth_header(header, &ev), ==, NOSTR_BUD02_OK);
    nostr_bud02_validate_options_t opts = { .expected_sha256 = hash };
    g_assert_cmpint(nostr_bud02_validate_auth_event(ev, NOSTR_BUD02_ACTION_UPLOAD, &opts), ==, NOSTR_BUD02_OK);
    nostr_event_free(ev);
    g_autofree gchar *json = g_strdup_printf("{\"sha256\":\"%s\",\"size\":%zu,\"url\":\"%s/%s.bin\"}",
      bad_descriptor ? "0000000000000000000000000000000000000000000000000000000000000000" : hash,
      sizeof payload - 1, base, hash);
    soup_server_message_set_response(m, "application/json", SOUP_MEMORY_COPY, json, strlen(json));
    soup_server_message_set_status(m, auth_refused ? 401 : 200, NULL);
  } else {
    soup_server_message_set_response(m, "application/octet-stream", SOUP_MEMORY_STATIC, payload, sizeof payload - 1);
    soup_server_message_set_status(m, 200, NULL);
  }
  if (active_cancel) g_cancellable_cancel(active_cancel);
}
static GnBlossomClient *client(gsize cap)
{
  GnBlossomClient *c = gn_blossom_client_new(base);
  gn_blossom_client_set_network(c, NULL, GN_BLOSSOM_NETWORK_LOCAL_TEST);
  gn_blossom_client_set_limits(c, cap, 3, "");
  return c;
}
static gchar *authorization(void)
{
  char *sk = nostr_key_generate_private();
  NostrEvent *event = nostr_bud02_create_auth_event(NOSTR_BUD02_ACTION_UPLOAD, hash, 0, NULL);
  g_assert_cmpint(nostr_event_sign(event, sk), ==, 0);
  char *raw = nostr_event_serialize_compact(event);
  gchar *json = g_strdup(raw); free(raw);
  /* The serializer is malloc-owned. */
  nostr_event_free(event); memset(sk, 0, strlen(sk)); free(sk);
  return json;
}
static void upload(void)
{
  guint before = upload_count;
  g_autoptr(GnBlossomClient) c = client(sizeof payload - 1);
  g_autoptr(GBytes) bytes = g_bytes_new_static(payload, sizeof payload - 1);
  g_autofree gchar *auth = authorization();
  g_autoptr(GError) error = NULL;
  gn_blossom_client_upload_async(c, bytes, hash, "application/octet-stream", auth, NULL, done, NULL);
  wait_done();
  g_autofree gchar *url = gn_blossom_client_upload_finish(c, completed, &error);
  if (bad_descriptor) g_assert_error(error, GN_BLOSSOM_ERROR, GN_BLOSSOM_ERROR_BAD_RESPONSE);
  else if (auth_refused) g_assert_error(error, GN_BLOSSOM_ERROR, GN_BLOSSOM_ERROR_AUTH);
  else { g_assert_no_error(error); g_assert_nonnull(url); }
  g_assert_cmpuint(upload_count, ==, before + 1);
  g_clear_object(&completed);
}
static void bad_answer(void) { bad_descriptor = TRUE; upload(); bad_descriptor = FALSE; }
static void refused(void) { auth_refused = TRUE; upload(); auth_refused = FALSE; }
static void download_case(const gchar *path, gsize cap, gboolean cancel, gboolean wrong_hash)
{
  g_autoptr(GnBlossomClient) c = client(cap);
  g_autoptr(GCancellable) cancellable = g_cancellable_new();
  g_autofree gchar *url = g_strconcat(base, path, NULL);
  if (cancel) active_cancel = cancellable;
  gn_blossom_client_download_async(c, url, wrong_hash ? "0000000000000000000000000000000000000000000000000000000000000000" : hash,
    cancellable, done, NULL);
  wait_done(); active_cancel = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) bytes = gn_blossom_client_download_finish(c, completed, &error);
  if (cancel) g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  else if (cap < sizeof payload - 1) g_assert_error(error, GN_BLOSSOM_ERROR, GN_BLOSSOM_ERROR_TOO_LARGE);
  else if (wrong_hash) g_assert_error(error, GN_BLOSSOM_ERROR, GN_BLOSSOM_ERROR_BAD_RESPONSE);
  else { g_assert_no_error(error); g_assert_cmpuint(g_bytes_get_size(bytes), ==, sizeof payload - 1); }
  g_clear_object(&completed);
}
static void download(void) { download_case("/blob", 1024, FALSE, FALSE); }
static void redirect(void) { download_case("/redirect", 1024, FALSE, FALSE); }
static void too_large(void) { download_case("/blob", 4, FALSE, FALSE); }
static void cancel(void) { download_case("/blob", 1024, TRUE, FALSE); }
static void hash_mismatch(void) { download_case("/blob", 1024, FALSE, TRUE); }
static void policy(void)
{
  g_autoptr(GnBlossomClient) c = gn_blossom_client_new("https://example.com");
  gn_blossom_client_download_async(c, "http://not-allowed.onion/blob", hash, NULL, done, NULL);
  wait_done();
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) bytes = gn_blossom_client_download_finish(c, completed, &error);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  g_clear_object(&completed);
}
static void read_exact(GInputStream *in, void *buffer, gsize n)
{
  gsize got = 0;
  g_assert_true(g_input_stream_read_all(in, buffer, n, &got, NULL, NULL));
  g_assert_cmpuint(got, ==, n);
}
static gboolean socks_connection(GThreadedSocketService *service, GSocketConnection *connection,
                                  GObject *source, gpointer data)
{
  (void)service; (void)source; (void)data;
  GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(connection));
  GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(connection));
  guint8 b[512];
  read_exact(in, b, 2); g_assert_cmpuint(b[0], ==, 5);
  guint methods = b[1]; read_exact(in, b, methods);
  gboolean have_auth = FALSE;
  for (guint i = 0; i < methods; i++) have_auth |= b[i] == 2;
  g_assert_true(have_auth);
  g_assert_true(g_output_stream_write_all(out, "\5\2", 2, NULL, NULL, NULL));
  read_exact(in, b, 2); g_assert_cmpuint(b[0], ==, 1);
  guint n = b[1]; read_exact(in, b, n); b[n] = 0;
  g_assert_cmpstr((gchar *)b, ==, "isolated-user");
  read_exact(in, b, 1); n = b[0]; read_exact(in, b, n); b[n] = 0;
  g_assert_cmpstr((gchar *)b, ==, "isolated-password");
  g_assert_true(g_output_stream_write_all(out, "\1\0", 2, NULL, NULL, NULL));
  read_exact(in, b, 5);
  g_assert_cmpuint(b[0], ==, 5); g_assert_cmpuint(b[1], ==, 1);
  g_assert_cmpuint(b[3], ==, 3); /* SOCKS domain, not locally resolved IP. */
  n = b[4]; read_exact(in, b, n); b[n] = 0;
  g_assert_cmpstr((gchar *)b, ==, "unresolvable-test.onion");
  read_exact(in, b, 2);
  const guint8 connected[] = {5,0,0,1,127,0,0,1,0,80};
  g_assert_true(g_output_stream_write_all(out, connected, sizeof connected, NULL, NULL, NULL));
  g_autoptr(GDataInputStream) lines = g_data_input_stream_new(in);
  g_data_input_stream_set_newline_type(lines, G_DATA_STREAM_NEWLINE_TYPE_CR_LF);
  gboolean first = TRUE;
  for (;;) {
    g_autofree gchar *line = g_data_input_stream_read_line(lines, NULL, NULL, NULL);
    g_assert_nonnull(line);
    if (first) g_assert_true(g_str_has_prefix(line, "GET /blob "));
    first = FALSE;
    g_assert_false(g_str_has_prefix(line, "User-Agent:"));
    g_assert_false(g_str_has_prefix(line, "Authorization:"));
    if (!*line) break;
  }
  g_autofree gchar *response = g_strdup_printf("HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s", sizeof payload - 1, payload);
  g_assert_true(g_output_stream_write_all(out, response, strlen(response), NULL, NULL, NULL));
  return TRUE;
}
static void socks_remote_dns(void)
{
  g_autoptr(GSocketService) proxy = g_threaded_socket_service_new(1);
  g_signal_connect(proxy, "run", G_CALLBACK(socks_connection), NULL);
  g_autoptr(GInetAddress) address = g_inet_address_new_from_string("127.0.0.1");
  g_autoptr(GSocketAddress) bind = g_inet_socket_address_new(address, 0);
  GSocketAddress *effective = NULL;
  g_assert_true(g_socket_listener_add_address(G_SOCKET_LISTENER(proxy), bind, G_SOCKET_TYPE_STREAM,
    G_SOCKET_PROTOCOL_TCP, NULL, &effective, NULL));
  guint port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(effective));
  g_object_unref(effective); g_socket_service_start(proxy);
  g_autofree gchar *route = g_strdup_printf("socks5://isolated-user:isolated-password@127.0.0.1:%u", port);
  g_autoptr(GProxyResolver) resolver = g_simple_proxy_resolver_new(route, NULL);
  g_autoptr(GnBlossomClient) c = gn_blossom_client_new("http://unresolvable-test.onion");
  gn_blossom_client_set_limits(c, 1024, 3, "");
  gn_blossom_client_set_network(c, resolver, GN_BLOSSOM_NETWORK_TOR);
  gn_blossom_client_download_async(c, "http://unresolvable-test.onion/blob", hash, NULL, done, NULL);
  wait_done();
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) bytes = gn_blossom_client_download_finish(c, completed, &error);
  g_assert_no_error(error); g_assert_nonnull(bytes); g_clear_object(&completed);
  g_socket_service_stop(proxy); g_socket_listener_close(G_SOCKET_LISTENER(proxy));
}

int main(int argc, char **argv)
{
  /* Explicit proxy policy must win even over this environment bypass. */
  g_setenv("NO_PROXY", "*", TRUE);
  g_test_init(&argc, &argv, NULL);
  server = soup_server_new(NULL, NULL);
  soup_server_add_handler(server, NULL, handler, NULL, NULL);
  g_autoptr(GInetAddress) address = g_inet_address_new_from_string("127.0.0.1");
  g_autoptr(GSocketAddress) socket_address = g_inet_socket_address_new(address, 0);
  g_assert_true(soup_server_listen(server, socket_address, 0, NULL));
  GSList *uris = soup_server_get_uris(server);
  base = g_uri_to_string(uris->data); base[strlen(base) - 1] = 0;
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
  hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, payload, sizeof payload - 1);
  g_test_add_func("/blossom/upload", upload);
  g_test_add_func("/blossom/bad-descriptor", bad_answer);
  g_test_add_func("/blossom/auth-refused", refused);
  g_test_add_func("/blossom/download", download);
  g_test_add_func("/blossom/redirect", redirect);
  g_test_add_func("/blossom/size-limit", too_large);
  g_test_add_func("/blossom/cancel-in-flight", cancel);
  g_test_add_func("/blossom/hash-mismatch", hash_mismatch);
  g_test_add_func("/blossom/onion-policy", policy);
  g_test_add_func("/blossom/socks-remote-dns", socks_remote_dns);
  int result = g_test_run();
  soup_server_disconnect(server); g_object_unref(server); g_free(base); g_free(hash);
  return result;
}
