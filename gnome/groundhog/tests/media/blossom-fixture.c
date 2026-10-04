#include "blossom-fixture.h"

#include <libsoup/soup.h>
#include <nostr-event.h>
#include <nostr-tag.h>
#include <string.h>

struct _BlossomFixture {
  SoupServer *server;
  gchar *url;
  guint16 port;
  GHashTable *blobs;        /* sha256 -> GBytes */
  GPtrArray *requests;      /* BlossomRequest */
  gchar *required_pubkey;
  gchar *lie;
  gboolean strict_upload;
  gboolean reject_opaque;
  gboolean hold;
  gboolean chunked;
  gboolean stall;
  GPtrArray *held;          /* SoupServerMessage, paused or stalled */
};

static void
request_free(gpointer data)
{
  BlossomRequest *request = data;
  g_free(request->method);
  g_free(request->path);
  g_free(request->auth_pubkey);
  g_free(request->auth_x);
  g_free(request->auth_server);
  g_free(request->content_type);
  g_free(request->x_sha256);
  g_free(request);
}

static gchar *
sha256_of(const guint8 *data, gsize size)
{
  return g_compute_checksum_for_data(G_CHECKSUM_SHA256, data, size);
}

static const gchar *
tag_value(NostrTags *tags, const gchar *key)
{
  for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (g_strcmp0(nostr_tag_get_key(tag), key) == 0 && nostr_tag_size(tag) >= 2)
      return nostr_tag_get_value(tag);
  }
  return NULL;
}

/* Parses "Nostr <base64 event>" into request; TRUE when it is a valid
 * upload authorization for body_sha256. */
static gboolean
check_auth(BlossomRequest *request, const gchar *header, const gchar *body_sha256)
{
  if (!header)
    return FALSE;
  request->has_auth = TRUE;
  if (!g_str_has_prefix(header, "Nostr "))
    return FALSE;
  const gchar *encoded = header + 6;
  request->auth_base64url = *encoded != '\0' && strspn(encoded,
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") == strlen(encoded);
  g_autofree gchar *standard = g_malloc(strlen(encoded) + 4);
  strcpy(standard, encoded);
  gsize n = strlen(standard);
  while (n % 4) standard[n++] = '=';
  standard[n] = '\0';
  for (gchar *p = standard; *p; p++) {
    if (*p == '-') *p = '+';
    else if (*p == '_') *p = '/';
  }
  gsize length = 0;
  g_autofree guchar *raw = g_base64_decode(standard, &length);
  g_autofree gchar *json = raw ? g_strndup((const gchar *)raw, length) : NULL;
  if (!json)
    return FALSE;
  NostrEvent *event = nostr_event_new();
  gboolean parsed = nostr_event_deserialize_signed(event, json, NULL) == NOSTR_EVENT_VALIDATION_OK;
  gboolean valid = parsed && nostr_event_validate(event, NULL) == NOSTR_EVENT_VALIDATION_OK;
  if (parsed) {
    NostrTags *tags = nostr_event_get_tags(event);
    request->auth_pubkey = g_strdup(nostr_event_get_pubkey(event));
    request->auth_x = g_strdup(tag_value(tags, "x"));
    request->auth_server = g_strdup(tag_value(tags, "server"));
    const gchar *expiration = tag_value(tags, "expiration");
    request->auth_expiration = expiration ? g_ascii_strtoll(expiration, NULL, 10) : 0;
    valid = valid && nostr_event_get_kind(event) == 24242 &&
            g_strcmp0(tag_value(tags, "t"), "upload") == 0 &&
            g_strcmp0(request->auth_x, body_sha256) == 0 &&
            request->auth_expiration > g_get_real_time() / G_USEC_PER_SEC;
  }
  nostr_event_free(event);
  request->auth_valid = valid;
  return valid;
}

static void
refuse(SoupServerMessage *message, guint status, const gchar *reason)
{
  soup_message_headers_replace(soup_server_message_get_response_headers(message), "X-Reason",
                               reason);
  soup_server_message_set_status(message, status, NULL);
}

/* A deliberately small image-only policy, based on the bytes rather than
 * the claimed Content-Type. This is not a general MIME detector: it models
 * the public servers' refusal of opaque ciphertext even when labeled PNG. */
static gboolean
has_image_magic(const guint8 *data, gsize size)
{
  static const guint8 png[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
  return (size >= sizeof png && memcmp(data, png, sizeof png) == 0) ||
         (size >= 5 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff &&
          data[size - 2] == 0xff && data[size - 1] == 0xd9) ||
         (size >= 6 && (memcmp(data, "GIF87a", 6) == 0 ||
                        memcmp(data, "GIF89a", 6) == 0)) ||
         (size >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0);
}

static void
on_put(BlossomFixture *f, SoupServerMessage *message, BlossomRequest *request)
{
  SoupMessageBody *body = soup_server_message_get_request_body(message);
  g_autoptr(GBytes) bytes = soup_message_body_flatten(body);
  gsize size = 0;
  const guint8 *data = g_bytes_get_data(bytes, &size);
  request->body_size = size;
  g_autofree gchar *sha256 = sha256_of(data, size);
  SoupMessageHeaders *headers = soup_server_message_get_request_headers(message);
  request->content_type = g_strdup(soup_message_headers_get_content_type(headers, NULL));
  request->x_sha256 = g_strdup(soup_message_headers_get_one(headers, "X-SHA-256"));
  const gchar *auth = soup_message_headers_get_one(headers, "Authorization");
  if (!check_auth(request, auth, sha256)) {
    refuse(message, SOUP_STATUS_UNAUTHORIZED, "Invalid upload authorization");
    return;
  }
  if (f->strict_upload && (!request->auth_base64url ||
                           g_strcmp0(request->x_sha256, sha256) != 0 ||
                           g_strcmp0(request->content_type, "application/octet-stream") != 0)) {
    refuse(message, SOUP_STATUS_BAD_REQUEST, "Invalid encrypted upload headers");
    return;
  }
  if (f->reject_opaque && !has_image_magic(data, size)) {
    refuse(message, SOUP_STATUS_UNSUPPORTED_MEDIA_TYPE, "File type not allowed");
    return;
  }
  if (f->required_pubkey && g_strcmp0(request->auth_pubkey, f->required_pubkey) != 0) {
    refuse(message, SOUP_STATUS_UNAUTHORIZED, "Pubkey not allowed");
    return;
  }
  g_hash_table_replace(f->blobs, g_strdup(sha256), g_bytes_ref(bytes));
  g_autofree gchar *descriptor = g_strdup_printf(
    "{\"url\":\"%s/%s\",\"sha256\":\"%s\",\"size\":%" G_GSIZE_FORMAT ","
    "\"type\":\"application/octet-stream\",\"uploaded\":%" G_GINT64_FORMAT "}",
    f->url, sha256, f->lie ? f->lie : sha256, size, g_get_real_time() / G_USEC_PER_SEC);
  soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
  soup_server_message_set_response(message, "application/json", SOUP_MEMORY_COPY, descriptor,
                                   strlen(descriptor));
}

static void
on_get(BlossomFixture *f, SoupServerMessage *message, const gchar *path)
{
  g_autofree gchar *name = g_strdup(path + 1);
  gchar *dot = strchr(name, '.');
  if (dot)
    *dot = '\0';
  GBytes *blob = g_hash_table_lookup(f->blobs, name);
  if (!blob) {
    refuse(message, SOUP_STATUS_NOT_FOUND, "Blob not found");
    return;
  }
  soup_server_message_set_status(message, SOUP_STATUS_OK, NULL);
  SoupMessageHeaders *headers = soup_server_message_get_response_headers(message);
  soup_message_headers_replace(headers, "Content-Type", "application/octet-stream");
  if (!f->chunked && !f->stall) {
    gsize size = 0;
    gconstpointer data = g_bytes_get_data(blob, &size);
    soup_server_message_set_response(message, "application/octet-stream", SOUP_MEMORY_COPY, data,
                                     size);
    return;
  }
  soup_message_headers_set_encoding(headers, SOUP_ENCODING_CHUNKED);
  SoupMessageBody *body = soup_server_message_get_response_body(message);
  gsize size = g_bytes_get_size(blob);
  const gsize chunk = 16 * 1024;
  for (gsize offset = 0; offset < size; offset += chunk) {
    g_autoptr(GBytes) part = g_bytes_new_from_bytes(blob, offset, MIN(chunk, size - offset));
    soup_message_body_append_bytes(body, part);
    if (f->stall) {
      /* The rest never comes (until released): mid-transfer. */
      g_ptr_array_add(f->held, g_object_ref(message));
      return;
    }
  }
  soup_message_body_complete(body);
}

static void
on_request(SoupServer *server, SoupServerMessage *message, const char *path, GHashTable *query,
           gpointer data)
{
  (void)server;
  (void)query;
  BlossomFixture *f = data;
  BlossomRequest *request = g_new0(BlossomRequest, 1);
  request->method = g_strdup(soup_server_message_get_method(message));
  request->path = g_strdup(path);
  g_ptr_array_add(f->requests, request);
  if (g_str_equal(request->method, "PUT") && g_str_equal(path, "/upload"))
    on_put(f, message, request);
  else if (g_str_equal(request->method, "GET") && strlen(path) > 1)
    on_get(f, message, path);
  else
    refuse(message, SOUP_STATUS_NOT_FOUND, "No such endpoint");
  if (f->hold && !f->stall) {
    soup_server_message_pause(message);
    g_ptr_array_add(f->held, g_object_ref(message));
  }
}

BlossomFixture *
blossom_fixture_new(void)
{
  BlossomFixture *f = g_new0(BlossomFixture, 1);
  f->blobs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                   (GDestroyNotify)g_bytes_unref);
  f->requests = g_ptr_array_new_with_free_func(request_free);
  f->held = g_ptr_array_new_with_free_func(g_object_unref);
  f->server = soup_server_new(NULL, NULL);
  soup_server_add_handler(f->server, NULL, on_request, f, NULL);
  g_autoptr(GError) error = NULL;
  g_assert_true(soup_server_listen_local(f->server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
  g_assert_no_error(error);
  GSList *uris = soup_server_get_uris(f->server);
  g_assert_nonnull(uris);
  f->port = (guint16)g_uri_get_port(uris->data);
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
  f->url = g_strdup_printf("http://127.0.0.1:%u", f->port);
  return f;
}

void
blossom_fixture_release_held(BlossomFixture *f)
{
  g_autoptr(GPtrArray) held = g_steal_pointer(&f->held);
  f->held = g_ptr_array_new_with_free_func(g_object_unref);
  for (guint i = 0; i < held->len; i++) {
    SoupServerMessage *message = g_ptr_array_index(held, i);
    SoupMessageBody *body = soup_server_message_get_response_body(message);
    if (soup_message_headers_get_encoding(soup_server_message_get_response_headers(message)) ==
        SOUP_ENCODING_CHUNKED)
      soup_message_body_complete(body);
    soup_server_message_unpause(message);
  }
}

void
blossom_fixture_free(BlossomFixture *f)
{
  if (!f)
    return;
  blossom_fixture_release_held(f);
  soup_server_disconnect(f->server);
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_object_unref(f->server);
  g_ptr_array_unref(f->held);
  g_ptr_array_unref(f->requests);
  g_hash_table_unref(f->blobs);
  g_free(f->required_pubkey);
  g_free(f->lie);
  g_free(f->url);
  g_free(f);
}

const gchar *
blossom_fixture_url(BlossomFixture *f)
{
  return f->url;
}

guint16
blossom_fixture_port(BlossomFixture *f)
{
  return f->port;
}

void
blossom_fixture_require_pubkey(BlossomFixture *f, const gchar *pubkey)
{
  g_free(f->required_pubkey);
  f->required_pubkey = g_strdup(pubkey);
}

void
blossom_fixture_set_strict_upload(BlossomFixture *f, gboolean strict)
{
  f->strict_upload = strict;
}

void
blossom_fixture_reject_opaque(BlossomFixture *f, gboolean reject)
{
  f->reject_opaque = reject;
}

void
blossom_fixture_set_lie(BlossomFixture *f, const gchar *sha256)
{
  g_free(f->lie);
  f->lie = g_strdup(sha256);
}

void
blossom_fixture_set_hold(BlossomFixture *f, gboolean hold)
{
  f->hold = hold;
}

void
blossom_fixture_set_chunked(BlossomFixture *f, gboolean chunked)
{
  f->chunked = chunked;
}

void
blossom_fixture_set_stall(BlossomFixture *f, gboolean stall)
{
  f->stall = stall;
}

void
blossom_fixture_put_blob(BlossomFixture *f, const gchar *sha256, GBytes *bytes)
{
  g_hash_table_replace(f->blobs, g_strdup(sha256), g_bytes_ref(bytes));
}

GBytes *
blossom_fixture_get_blob(BlossomFixture *f, const gchar *sha256)
{
  return g_hash_table_lookup(f->blobs, sha256);
}

GPtrArray *
blossom_fixture_requests(BlossomFixture *f)
{
  return f->requests;
}

guint
blossom_fixture_count(BlossomFixture *f, const gchar *method)
{
  guint count = 0;
  for (guint i = 0; i < f->requests->len; i++) {
    BlossomRequest *request = g_ptr_array_index(f->requests, i);
    if (!method || g_str_equal(request->method, method))
      count++;
  }
  return count;
}

guint
blossom_fixture_held(BlossomFixture *f)
{
  return f->held->len;
}
