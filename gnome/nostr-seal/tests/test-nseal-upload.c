/* Fixture-backed tests of the upload engine used by nseal_publish_file(). */
#include "nseal-publish.h"
#include "ns-kind.h"
#include "nostr-event.h"
#include "nostr-keys.h"
#include <nostr/bud02/auth.h>
#include <libsoup/soup.h>
#include <string.h>

typedef enum {
  UPLOAD_OK, UPLOAD_HTTPS_URL, UPLOAD_AUTH_REFUSED, UPLOAD_BAD_HASH,
  UPLOAD_SIGNER_DENIED, UPLOAD_SIGNER_CHANGED, UPLOAD_HTTP_REFUSED
} UploadCase;

typedef struct {
  GMainContext *context;
  GMainLoop *loop;
  GMutex lock;
  GCond ready;
  char *endpoint;
  const char *hash;
  const char *pubkey;
  GBytes *bytes;
  UploadCase mode;
  guint requests;
} Blossom;

static void upload_request(SoupServer *server G_GNUC_UNUSED, SoupServerMessage *msg,
                           const char *path, GHashTable *query G_GNUC_UNUSED, gpointer data) {
  Blossom *b = data;
  b->requests++;
  g_assert_cmpstr(path, ==, "/upload");
  g_assert_cmpstr(soup_server_message_get_method(msg), ==, "PUT");
  SoupMessageHeaders *headers = soup_server_message_get_request_headers(msg);
  g_assert_cmpstr(soup_message_headers_get_content_type(headers, NULL), ==, NSEAL_MIME_TYPE);
  g_autoptr(GBytes) body = soup_message_body_flatten(soup_server_message_get_request_body(msg));
  g_assert_true(g_bytes_equal(body, b->bytes));
  const char *auth = soup_message_headers_get_one(headers, "Authorization");
  g_assert_nonnull(auth);
  NostrEvent *event = NULL;
  g_assert_cmpint(nostr_bud02_parse_auth_header(auth, &event), ==, NOSTR_BUD02_OK);
  nostr_bud02_validate_options_t opts = { .expected_sha256 = b->hash, .expected_pubkey = b->pubkey };
  g_assert_cmpint(nostr_bud02_validate_auth_event(event, NOSTR_BUD02_ACTION_UPLOAD, &opts), ==, NOSTR_BUD02_OK);
  nostr_event_free(event);
  if (b->mode == UPLOAD_AUTH_REFUSED) {
    soup_server_message_set_status(msg, SOUP_STATUS_UNAUTHORIZED, NULL);
    return;
  }
  g_autofree char *wrong_hash = g_strnfill(64, '0');
  g_autofree char *reply = g_strdup_printf(
    "{\"sha256\":\"%s\",\"size\":%zu%s}",
    b->mode == UPLOAD_BAD_HASH ? wrong_hash : b->hash, g_bytes_get_size(body),
    b->mode == UPLOAD_HTTPS_URL ? ",\"url\":\"https://cdn.example/sealed\"" : "");
  soup_server_message_set_status(msg, SOUP_STATUS_OK, NULL);
  soup_server_message_set_response(msg, "application/json", SOUP_MEMORY_COPY, reply, strlen(reply));
}

static gpointer serve(gpointer data) {
  Blossom *b = data;
  g_main_context_push_thread_default(b->context);
  g_autoptr(SoupServer) server = soup_server_new(NULL, NULL);
  soup_server_add_handler(server, "/upload", upload_request, b, NULL);
  g_assert_true(soup_server_listen_local(server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, NULL));
  GSList *uris = soup_server_get_uris(server);
  g_mutex_lock(&b->lock);
  b->endpoint = g_strdup_printf("http://127.0.0.1:%d", g_uri_get_port(uris->data));
  g_cond_signal(&b->ready);
  g_mutex_unlock(&b->lock);
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
  g_main_loop_run(b->loop);
  soup_server_disconnect(server);
  g_main_context_pop_thread_default(b->context);
  return NULL;
}

static gboolean stop_server(gpointer data) {
  g_main_loop_quit(data);
  return G_SOURCE_REMOVE;
}

typedef struct {
  char *key;
  UploadCase mode;
  guint calls;
} Signer;

static char *sign_auth(gpointer data, const char *json, GCancellable *cancel G_GNUC_UNUSED,
                        GError **error) {
  Signer *s = data;
  s->calls++;
  if (s->mode == UPLOAD_SIGNER_DENIED) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "fixture signer denied");
    return NULL;
  }
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(event, json, NULL), ==, 1);
  if (s->mode == UPLOAD_SIGNER_CHANGED) nostr_event_set_content(event, "changed by signer");
  g_assert_cmpint(nostr_event_sign(event, s->key), ==, 0);
  char *raw = nostr_event_serialize_compact(event);
  char *result = g_strdup(raw);
  free(raw);
  nostr_event_free(event);
  return result;
}

static void test_upload(gconstpointer data) {
  UploadCase mode = GPOINTER_TO_INT(data);
  Signer signing = { .key = nostr_key_generate_private(), .mode = mode };
  g_assert_nonnull(signing.key);
  char *pubkey = nostr_key_get_public(signing.key);
  const guint8 ciphertext[] = {0, 0xff, 's', 'e', 'a', 'l', 0};
  g_autoptr(GBytes) bytes = g_bytes_new_static(ciphertext, sizeof(ciphertext));
  g_autofree char *hash = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, bytes);
  Blossom b = { .context = g_main_context_new(), .bytes = bytes,
                 .hash = hash, .pubkey = pubkey, .mode = mode };
  b.loop = g_main_loop_new(b.context, FALSE);
  g_mutex_init(&b.lock);
  g_cond_init(&b.ready);
  GThread *thread = g_thread_new("seal-blossom", serve, &b);
  g_mutex_lock(&b.lock);
  while (!b.endpoint) g_cond_wait(&b.ready, &b.lock);
  g_mutex_unlock(&b.lock);
  NostrPublishSignerVTable vtable = { .sign_event_json = sign_auth };
  NostrPublishSigner *signer = nostr_publish_signer_new_from_vtable(&vtable, &signing);
  /* The first endpoint is refused before signing; the second is the fixture. */
  const char *servers[] = {"http://not-public.example", b.endpoint, NULL};
  g_autofree char *url = NULL;
  g_autoptr(GError) error = NULL;
  gboolean ok = nseal_publish_upload(servers, signer, pubkey, bytes, hash,
                                     mode != UPLOAD_HTTP_REFUSED, &url, &error);
  g_main_context_invoke(b.context, stop_server, b.loop);
  g_thread_join(thread);
  if (mode == UPLOAD_OK || mode == UPLOAD_HTTPS_URL) {
    g_assert_true(ok);
    g_assert_no_error(error);
    g_autofree char *expected = g_strdup_printf("%s/%s", b.endpoint, hash);
    g_assert_cmpstr(url, ==, mode == UPLOAD_HTTPS_URL ? "https://cdn.example/sealed" : expected);
  } else {
    g_assert_false(ok);
    g_assert_null(url);
    g_assert_error(error, NS_ERROR, NS_ERROR_UPLOAD);
    g_assert_nonnull(strstr(error->message, "Blossom upload failed:"));
    if (mode == UPLOAD_SIGNER_DENIED)
      g_assert_nonnull(strstr(error->message, "fixture signer denied"));
    if (mode == UPLOAD_SIGNER_CHANGED)
      g_assert_nonnull(strstr(error->message, "different or invalid event"));
    if (mode == UPLOAD_HTTP_REFUSED)
      g_assert_nonnull(strstr(error->message, "https:// only"));
  }
  gboolean no_http = mode == UPLOAD_SIGNER_DENIED || mode == UPLOAD_SIGNER_CHANGED ||
                     mode == UPLOAD_HTTP_REFUSED;
  g_assert_cmpuint(b.requests, ==, no_http ? 0 : 1);
  g_assert_cmpuint(signing.calls, ==, mode == UPLOAD_HTTP_REFUSED ? 0 : 1);
  nostr_publish_signer_unref(signer);
  memset(signing.key, 0, strlen(signing.key));
  free(signing.key);
  free(pubkey);
  g_free(b.endpoint);
  g_main_loop_unref(b.loop);
  g_main_context_unref(b.context);
  g_mutex_clear(&b.lock);
  g_cond_clear(&b.ready);
}

void nseal_test_add_upload_cases(void) {
  g_test_add_data_func("/nseal/publish/upload/bytes-and-fallback-url", GINT_TO_POINTER(UPLOAD_OK), test_upload);
  g_test_add_data_func("/nseal/publish/upload/https-url", GINT_TO_POINTER(UPLOAD_HTTPS_URL), test_upload);
  g_test_add_data_func("/nseal/publish/upload/auth-refused", GINT_TO_POINTER(UPLOAD_AUTH_REFUSED), test_upload);
  g_test_add_data_func("/nseal/publish/upload/bad-hash", GINT_TO_POINTER(UPLOAD_BAD_HASH), test_upload);
  g_test_add_data_func("/nseal/publish/upload/signer-denied", GINT_TO_POINTER(UPLOAD_SIGNER_DENIED), test_upload);
  g_test_add_data_func("/nseal/publish/upload/signer-changed", GINT_TO_POINTER(UPLOAD_SIGNER_CHANGED), test_upload);
  g_test_add_data_func("/nseal/publish/upload/http-refused", GINT_TO_POINTER(UPLOAD_HTTP_REFUSED), test_upload);
}
