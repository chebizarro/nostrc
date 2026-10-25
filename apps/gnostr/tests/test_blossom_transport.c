/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Real app entrypoints + GnBlossomClient/libblossom + an in-process server.
 * Only the app's signer/settings boundary is substituted; signatures are real. */
#include "util/blossom.h"
#include "util/blossom_settings.h"
#include "ipc/gnostr-signer-service.h"
#include <libsoup/soup.h>
#include <nostr/bud02/auth.h>
#include <nostr-event.h>
#include <nostr-keys.h>
#include <glib/gstdio.h>
#include <unistd.h>

static const char payload[] = "gnostr upload bytes";
static char *base, *fail_base, *hash, *file_path, *sk;
static guint upload_requests, deletes, signs;
static gboolean finished, deny_sign, omit_url;
static GnostrBlossomBlob *uploaded;
static GnostrBlossomBlob **listed;
static GError *failure;
static gboolean deleted;

GnostrSignerService *gnostr_signer_service_get_default(void) { return (GnostrSignerService *)&signs; }
gboolean gnostr_signer_service_is_available(GnostrSignerService *self) { return self != NULL; }
const char **gnostr_blossom_settings_get_enabled_urls(gsize *n) {
  const char **urls = g_new(const char *, 2);
  urls[0] = fail_base; urls[1] = base; *n = 2; return urls;
}
void gnostr_sign_event_async(const char *json, const char *user, const char *app,
    GCancellable *cancel, GAsyncReadyCallback callback, gpointer data) {
  (void)user; (void)app;
  signs++;
  GTask *task = g_task_new(NULL, cancel, callback, data);
  if (g_task_return_error_if_cancelled(task)) { g_object_unref(task); return; }
  if (deny_sign) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "User denied signing");
  } else {
    NostrEvent *event = nostr_event_new();
    g_assert_cmpint(nostr_event_deserialize_compact(event, json, NULL), ==, 1);
    g_assert_cmpint(nostr_event_sign(event, sk), ==, 0);
    g_task_return_pointer(task, nostr_event_serialize_compact(event), free);
    nostr_event_free(event);
  }
  g_object_unref(task);
}
gboolean gnostr_sign_event_finish(GAsyncResult *result, char **json, GError **error) {
  *json = g_task_propagate_pointer(G_TASK(result), error); return *json != NULL;
}
static void handler(SoupServer *server, SoupServerMessage *msg, const char *path,
                    GHashTable *query, gpointer data) {
  (void)server; (void)query; (void)data;
  const char *method = soup_server_message_get_method(msg);
  if (g_str_equal(method, "PUT") || g_str_equal(method, "DELETE")) {
    gboolean upload = g_str_equal(method, "PUT");
    if (upload) upload_requests++; else deletes++;
    const char *auth = soup_message_headers_get_one(soup_server_message_get_request_headers(msg), "Authorization");
    NostrEvent *event = NULL;
    /* libblossom's existing BUD-11 upload route uses unpadded base64url.
     * The BUD-02 codec accepts standard padded base64; normalize for parsing. */
    g_assert_nonnull(auth);
    GString *normalized = g_string_new(auth);
    for (gsize i = 6; i < normalized->len; i++) {
      if (normalized->str[i] == '-') normalized->str[i] = '+';
      else if (normalized->str[i] == '_') normalized->str[i] = '/';
    }
    while ((normalized->len - 6) % 4) g_string_append_c(normalized, '=');
    g_assert_cmpint(nostr_bud02_parse_auth_header(normalized->str, &event), ==, NOSTR_BUD02_OK);
    g_string_free(normalized, TRUE);
    nostr_bud02_validate_options_t opts = { .expected_sha256 = hash };
    g_assert_cmpint(nostr_bud02_validate_auth_event(event,
      upload ? NOSTR_BUD02_ACTION_UPLOAD : NOSTR_BUD02_ACTION_DELETE, &opts), ==, NOSTR_BUD02_OK);
    nostr_event_free(event);
    if (g_str_has_prefix(path, "/fail/")) {
      soup_server_message_set_status(msg, 401, NULL); return;
    }
    if (upload) {
      SoupMessageBody *body = soup_server_message_get_request_body(msg);
      g_assert_cmpmem(body->data, body->length, payload, sizeof payload - 1);
      g_assert_cmpstr(soup_message_headers_get_content_type(soup_server_message_get_request_headers(msg), NULL), ==, "image/png");
      g_autofree char *json = g_strdup_printf("{\"sha256\":\"%s\",\"size\":%zu,\"url\":\"%s\"}",
        hash, sizeof payload - 1, omit_url ? "" : "https://cdn.example/blob.png");
      soup_server_message_set_response(msg, "application/json", SOUP_MEMORY_COPY, json, strlen(json));
    }
    soup_server_message_set_status(msg, upload ? 200 : 204, NULL);
  } else {
    if (g_str_has_prefix(path, "/fail/list/")) {
      soup_server_message_set_status(msg, 404, NULL); return;
    }
    g_assert_true(g_str_has_prefix(path, "/list/"));
    g_assert_null(soup_message_headers_get_one(soup_server_message_get_request_headers(msg), "Authorization"));
    g_autofree char *json = g_strdup_printf("[{\"sha256\":\"%s\",\"size\":%zu,\"type\":\"image/png\",\"url\":\"https://cdn.example/blob.png\"}]", hash, sizeof payload - 1);
    soup_server_message_set_response(msg, "application/json", SOUP_MEMORY_COPY, json, strlen(json));
    soup_server_message_set_status(msg, 200, NULL);
  }
}
static void upload_done(GnostrBlossomBlob *blob, GError *error, gpointer data) {
  (void)data; uploaded = blob; failure = error ? g_error_copy(error) : NULL; finished = TRUE;
}
static void list_done(GnostrBlossomBlob **blobs, GError *error, gpointer data) {
  (void)data; listed = blobs; failure = error ? g_error_copy(error) : NULL; finished = TRUE;
}
static void delete_done(gboolean ok, GError *error, gpointer data) {
  (void)data; deleted = ok; failure = error ? g_error_copy(error) : NULL; finished = TRUE;
}
static void wait_done(void) { while (!finished) g_main_context_iteration(NULL, TRUE); finished = FALSE; }
static void upload_ok(void) {
  gnostr_blossom_upload_async(base, file_path, "image/png", upload_done, NULL, NULL);
  wait_done(); g_assert_no_error(failure); g_assert_nonnull(uploaded);
  g_assert_cmpstr(uploaded->sha256, ==, hash);
  g_assert_cmpuint(uploaded->size, ==, sizeof payload - 1);
  if (omit_url) {
    g_autofree char *url = g_strdup_printf("%s/%s.png", base, hash);
    g_assert_cmpstr(uploaded->url, ==, url);
  } else g_assert_cmpstr(uploaded->url, ==, "https://cdn.example/blob.png");
  gnostr_blossom_blob_free(uploaded); uploaded = NULL;
}
static void upload_no_url(void) { omit_url = TRUE; upload_ok(); omit_url = FALSE; }
static void fallback(void) {
  guint before = upload_requests;
  gnostr_blossom_upload_with_fallback_async(file_path, "image/png", upload_done, NULL, NULL);
  wait_done(); g_assert_no_error(failure); g_assert_nonnull(uploaded);
  g_assert_cmpuint(upload_requests, ==, before + 2);
  gnostr_blossom_blob_free(uploaded); uploaded = NULL;
}
static void denied(void) {
  guint before = upload_requests; deny_sign = TRUE;
  gnostr_blossom_upload_async(base, file_path, "image/png", upload_done, NULL, NULL);
  wait_done(); deny_sign = FALSE;
  g_assert_error(failure, GNOSTR_BLOSSOM_ERROR, GNOSTR_BLOSSOM_ERROR_SIGNING_FAILED);
  g_assert_cmpuint(upload_requests, ==, before); g_clear_error(&failure);
}
static void cancelled(void) {
  guint before = upload_requests, before_sign = signs;
  g_autoptr(GCancellable) cancel = g_cancellable_new(); g_cancellable_cancel(cancel);
  gnostr_blossom_upload_with_fallback_async(file_path, "image/png", upload_done, NULL, cancel);
  wait_done(); g_assert_error(failure, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpuint(upload_requests, ==, before); g_assert_cmpuint(signs, ==, before_sign);
  g_clear_error(&failure);
}
static void http_errors(void) {
  gnostr_blossom_upload_async(fail_base, file_path, "image/png", upload_done, NULL, NULL);
  wait_done(); g_assert_null(uploaded);
  g_assert_error(failure, GNOSTR_BLOSSOM_ERROR, GNOSTR_BLOSSOM_ERROR_UPLOAD_FAILED);
  g_clear_error(&failure);
  gnostr_blossom_delete_async(fail_base, hash, delete_done, NULL, NULL);
  wait_done(); g_assert_false(deleted);
  g_assert_error(failure, GNOSTR_BLOSSOM_ERROR, GNOSTR_BLOSSOM_ERROR_SERVER_ERROR);
  g_clear_error(&failure);
  gnostr_blossom_list_async(fail_base, hash, list_done, NULL, NULL);
  wait_done(); g_assert_null(listed);
  g_assert_error(failure, GNOSTR_BLOSSOM_ERROR, GNOSTR_BLOSSOM_ERROR_SERVER_ERROR);
  g_clear_error(&failure);
}
static void list_delete(void) {
  gnostr_blossom_list_async(base, hash, list_done, NULL, NULL);
  wait_done(); g_assert_no_error(failure); g_assert_nonnull(listed);
  g_assert_nonnull(listed[0]); g_assert_null(listed[1]);
  g_assert_cmpstr(listed[0]->sha256, ==, hash);
  gnostr_blossom_blob_free(listed[0]); g_free(listed); listed = NULL;
  guint before = deletes;
  gnostr_blossom_delete_async(base, hash, delete_done, NULL, NULL);
  wait_done(); g_assert_no_error(failure); g_assert_true(deleted);
  g_assert_cmpuint(deletes, ==, before + 1);
}
int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  sk = nostr_key_generate_private();
  hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, payload, sizeof payload - 1);
  int fd = g_file_open_tmp("gnostr-blossom-XXXXXX", &file_path, NULL);
  g_assert_cmpint(fd, >=, 0); close(fd);
  g_assert_true(g_file_set_contents(file_path, payload, sizeof payload - 1, NULL));
  SoupServer *server = soup_server_new(NULL, NULL);
  soup_server_add_handler(server, NULL, handler, NULL, NULL);
  g_autoptr(GInetAddress) address = g_inet_address_new_from_string("127.0.0.1");
  g_autoptr(GSocketAddress) bind = g_inet_socket_address_new(address, 0);
  g_assert_true(soup_server_listen(server, bind, 0, NULL));
  GSList *uris = soup_server_get_uris(server);
  base = g_uri_to_string(uris->data); base[strlen(base) - 1] = 0;
  fail_base = g_strconcat(base, "/fail", NULL);
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
  g_test_add_func("/gnostr/blossom/upload", upload_ok);
  g_test_add_func("/gnostr/blossom/upload-no-url", upload_no_url);
  g_test_add_func("/gnostr/blossom/fallback", fallback);
  g_test_add_func("/gnostr/blossom/denied", denied);
  g_test_add_func("/gnostr/blossom/cancel", cancelled);
  g_test_add_func("/gnostr/blossom/list-delete", list_delete);
  g_test_add_func("/gnostr/blossom/http-errors", http_errors);
  int result = g_test_run();
  soup_server_disconnect(server); g_object_unref(server);
  g_unlink(file_path); g_free(file_path); g_free(base); g_free(fail_base); g_free(hash);
  memset(sk, 0, strlen(sk)); free(sk);
  return result;
}
