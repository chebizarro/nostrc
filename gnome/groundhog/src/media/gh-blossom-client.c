#include "gh-blossom-client.h"

#include <json-glib/json-glib.h>
#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr-tag.h>
#include <secure_buf.h>
#include <stdlib.h>
#include <string.h>

#define DESCRIPTOR_MAX_BYTES (64 * 1024)

G_DEFINE_QUARK(gh-blossom-error-quark, gh_blossom_error)

struct _GhBlossomClient {
  GObject parent_instance;
  GSettings *settings;
  GhNetHttp *http;
  GStrv servers_override;
  gchar *account_pubkey;
  GhBlossomSignAsyncFunc sign_async;
  GhBlossomSignFinishFunc sign_finish;
  gpointer sign_data;
  GDestroyNotify sign_destroy;
  GHashTable *consent; /* normalized server URL set */
  gsize max_file_size;
  gboolean allow_private_hosts; /* tests only */
};

G_DEFINE_FINAL_TYPE(GhBlossomClient, gh_blossom_client, G_TYPE_OBJECT)

static gboolean
hex64_any_case(const gchar *s)
{
  if (!s || strlen(s) != 64)
    return FALSE;
  for (const gchar *p = s; *p; p++)
    if (!g_ascii_isxdigit(*p))
      return FALSE;
  return TRUE;
}

static gboolean
lower_hex64(const gchar *s)
{
  if (!s || strlen(s) != 64)
    return FALSE;
  for (const gchar *p = s; *p; p++)
    if (!g_ascii_isdigit(*p) && (*p < 'a' || *p > 'f'))
      return FALSE;
  return TRUE;
}

/* scheme://host[:port][/path] without a trailing slash, query, fragment or
 * user info; NULL for anything but an http(s) URL with a host. Whether it may
 * be contacted in the current mode is GhNetHttp's decision. */
static gchar *
normalize_server(const gchar *server, gchar **out_host)
{
  g_autofree gchar *trimmed = g_strstrip(g_strdup(server ? server : ""));
  g_autoptr(GUri) uri = *trimmed ? g_uri_parse(trimmed, G_URI_FLAGS_ENCODED, NULL) : NULL;
  const gchar *scheme = uri ? g_uri_get_scheme(uri) : NULL;
  const gchar *host = uri ? g_uri_get_host(uri) : NULL;
  if (!scheme || !host || !*host || g_uri_get_userinfo(uri) || g_uri_get_query(uri) ||
      g_uri_get_fragment(uri) ||
      (g_ascii_strcasecmp(scheme, "https") != 0 && g_ascii_strcasecmp(scheme, "http") != 0))
    return NULL;
  g_autofree gchar *lower_scheme = g_ascii_strdown(scheme, -1);
  g_autofree gchar *lower_host = g_ascii_strdown(host, -1);
  g_autofree gchar *path = g_strdup(g_uri_get_path(uri));
  gsize len = strlen(path);
  while (len > 0 && path[len - 1] == '/')
    path[--len] = '\0';
  gboolean ipv6 = strchr(lower_host, ':') != NULL;
  gint port = g_uri_get_port(uri);
  g_autofree gchar *port_part = port > 0 ? g_strdup_printf(":%d", port) : g_strdup("");
  if (out_host)
    *out_host = g_strdup(lower_host);
  return g_strdup_printf("%s://%s%s%s%s%s", lower_scheme, ipv6 ? "[" : "", lower_host,
                         ipv6 ? "]" : "", port_part, path);
}

GStrv
gh_blossom_client_dup_servers(GhBlossomClient *self)
{
  g_return_val_if_fail(GH_IS_BLOSSOM_CLIENT(self), NULL);
  g_auto(GStrv) configured = NULL;
  if (self->servers_override)
    configured = g_strdupv(self->servers_override);
  else if (self->settings) {
    g_autoptr(GSettingsSchema) schema = NULL;
    g_object_get(self->settings, "settings-schema", &schema, NULL);
    if (schema && g_settings_schema_has_key(schema, "blossom-servers"))
      configured = g_settings_get_strv(self->settings, "blossom-servers");
  }
  g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
  for (guint i = 0; configured && configured[i]; i++) {
    g_autofree gchar *stripped = g_strstrip(g_strdup(configured[i]));
    if (*stripped)
      g_strv_builder_add(builder, stripped);
  }
  return g_strv_builder_end(builder);
}

void
gh_blossom_client_set_servers(GhBlossomClient *self, const gchar *const *servers)
{
  g_return_if_fail(GH_IS_BLOSSOM_CLIENT(self));
  g_strfreev(self->servers_override);
  self->servers_override = servers ? g_strdupv((gchar **)servers) : NULL;
}

void
gh_blossom_client_set_account_signer(GhBlossomClient *self, const gchar *account_pubkey,
                                     GhBlossomSignAsyncFunc sign_async,
                                     GhBlossomSignFinishFunc sign_finish, gpointer user_data,
                                     GDestroyNotify user_data_destroy)
{
  g_return_if_fail(GH_IS_BLOSSOM_CLIENT(self));
  g_return_if_fail(!sign_async || (sign_finish && lower_hex64(account_pubkey)));
  if (self->sign_destroy && self->sign_data)
    self->sign_destroy(self->sign_data);
  g_clear_pointer(&self->account_pubkey, g_free);
  self->sign_async = sign_async;
  self->sign_finish = sign_async ? sign_finish : NULL;
  self->sign_data = sign_async ? user_data : NULL;
  self->sign_destroy = sign_async ? user_data_destroy : NULL;
  if (sign_async)
    self->account_pubkey = g_strdup(account_pubkey);
  g_hash_table_remove_all(self->consent);
}

void
gh_blossom_client_set_account_consent(GhBlossomClient *self, const gchar *server,
                                      gboolean consent)
{
  g_return_if_fail(GH_IS_BLOSSOM_CLIENT(self));
  gchar *normalized = normalize_server(server, NULL);
  g_return_if_fail(normalized != NULL);
  if (consent)
    g_hash_table_add(self->consent, normalized);
  else {
    g_hash_table_remove(self->consent, normalized);
    g_free(normalized);
  }
}

gboolean
gh_blossom_client_get_account_consent(GhBlossomClient *self, const gchar *server)
{
  g_return_val_if_fail(GH_IS_BLOSSOM_CLIENT(self), FALSE);
  g_autofree gchar *normalized = normalize_server(server, NULL);
  return normalized && g_hash_table_contains(self->consent, normalized);
}

void
gh_blossom_client_set_max_file_size(GhBlossomClient *self, gsize max_file_size)
{
  g_return_if_fail(GH_IS_BLOSSOM_CLIENT(self));
  g_return_if_fail(max_file_size > 0 && max_file_size <= GH_BLOSSOM_MAX_FILE_SIZE);
  self->max_file_size = max_file_size;
}

gsize
gh_blossom_client_get_max_file_size(GhBlossomClient *self)
{
  g_return_val_if_fail(GH_IS_BLOSSOM_CLIENT(self), 0);
  return self->max_file_size;
}

/* The largest ciphertext: the file plus the GCM tag. */
static gsize
max_ciphertext(GhBlossomClient *self)
{
  return self->max_file_size + 16;
}

/* ---- kind 24242 ------------------------------------------------------------------ */

static NostrTags *
auth_tags(const gchar *sha256, const gchar *host, gint64 expiration)
{
  gchar expires[24];
  g_snprintf(expires, sizeof expires, "%" G_GINT64_FORMAT, expiration);
  return nostr_tags_new(4, nostr_tag_new("t", "upload", NULL), nostr_tag_new("x", sha256, NULL),
                        nostr_tag_new("expiration", expires, NULL),
                        nostr_tag_new("server", host, NULL));
}

static NostrEvent *
auth_event_new(const gchar *pubkey, const gchar *sha256, const gchar *host, gint64 now)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, GH_BLOSSOM_AUTH_KIND);
  nostr_event_set_created_at(event, now);
  nostr_event_set_content(event, "Upload file");
  if (pubkey)
    nostr_event_set_pubkey(event, pubkey);
  nostr_event_set_tags(event, auth_tags(sha256, host, now + GH_BLOSSOM_AUTH_LIFETIME_S));
  return event;
}

static gboolean
secret_from_hex(const gchar *hex, nostr_secure_buf *out)
{
  if (!hex || strlen(hex) != 64)
    return FALSE;
  *out = secure_alloc(32);
  if (!out->ptr)
    return FALSE;
  guint8 *bytes = out->ptr;
  for (gsize i = 0; i < 32; i++) {
    gint hi = g_ascii_xdigit_value(hex[2 * i]);
    gint lo = g_ascii_xdigit_value(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) {
      secure_free(out);
      return FALSE;
    }
    bytes[i] = (guint8)((hi << 4) | lo);
  }
  return TRUE;
}

/* The upload authorization signed by a key made for it alone, whose secret
 * is wiped before this returns (never the account's, never reused). */
static gchar *
auth_sign_ephemeral(const gchar *sha256, const gchar *host, gint64 now)
{
  gchar *signed_json = NULL;
  nostr_secure_buf secret = { 0 };
  char *secret_hex = nostr_key_generate_private();
  char *pubkey = secret_hex ? nostr_key_get_public(secret_hex) : NULL;
  gboolean have_secret = secret_from_hex(secret_hex, &secret);
  if (secret_hex) {
    secure_wipe(secret_hex, strlen(secret_hex));
    free(secret_hex);
  }
  if (pubkey && have_secret) {
    NostrEvent *event = auth_event_new(pubkey, sha256, host, now);
    if (nostr_event_sign_secure(event, &secret) == 0) {
      char *raw = nostr_event_serialize_compact(event);
      signed_json = raw ? g_strdup(raw) : NULL;
      free(raw);
    }
    nostr_event_free(event);
  }
  if (have_secret)
    secure_free(&secret);
  free(pubkey);
  return signed_json;
}

static gboolean
tag_is(NostrTags *tags, const gchar *key, const gchar *value)
{
  guint seen = 0;
  for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (g_strcmp0(nostr_tag_get_key(tag), key) != 0)
      continue;
    if (nostr_tag_size(tag) < 2 || g_strcmp0(nostr_tag_get_value(tag), value) != 0)
      return FALSE;
    seen++;
  }
  return seen == 1;
}

/* The account signer's answer is used only if it is exactly what was asked
 * for: valid id and signature, kind 24242, the account's pubkey, the same
 * tags, content and time. */
static gboolean
auth_verify(const gchar *signed_json, const gchar *pubkey, const gchar *sha256, const gchar *host,
            gint64 now)
{
  NostrEvent *event = nostr_event_new();
  gchar expires[24];
  g_snprintf(expires, sizeof expires, "%" G_GINT64_FORMAT, now + GH_BLOSSOM_AUTH_LIFETIME_S);
  gboolean ok = signed_json &&
                nostr_event_deserialize_signed(event, signed_json, NULL) ==
                  NOSTR_EVENT_VALIDATION_OK &&
                nostr_event_validate(event, NULL) == NOSTR_EVENT_VALIDATION_OK &&
                nostr_event_get_kind(event) == GH_BLOSSOM_AUTH_KIND &&
                g_strcmp0(nostr_event_get_pubkey(event), pubkey) == 0 &&
                nostr_event_get_created_at(event) == now &&
                g_strcmp0(nostr_event_get_content(event), "Upload file") == 0;
  NostrTags *tags = ok ? nostr_event_get_tags(event) : NULL;
  ok = ok && nostr_tags_size(tags) == 4 && tag_is(tags, "t", "upload") &&
       tag_is(tags, "x", sha256) && tag_is(tags, "expiration", expires) &&
       tag_is(tags, "server", host);
  nostr_event_free(event);
  return ok;
}

/* ---- upload ---------------------------------------------------------------------- */

typedef struct {
  GBytes *ciphertext;
  gchar sha256[65];
  GStrv servers;
  guint next;
  gchar *server;      /* the one being tried (normalized) */
  gchar *host;
  gboolean as_account;
  gint64 now;
  GError *last_error; /* the latest server's failure */
} Upload;

static void
upload_free(gpointer data)
{
  Upload *upload = data;
  g_bytes_unref(upload->ciphertext);
  g_strfreev(upload->servers);
  g_free(upload->server);
  g_free(upload->host);
  g_clear_error(&upload->last_error);
  g_free(upload);
}

static void upload_next(GTask *task);

static void
upload_failed(GTask *task, GError *error)
{
  Upload *upload = g_task_get_task_data(task);
  g_clear_error(&upload->last_error);
  upload->last_error = error;
  upload_next(task);
}

/* The server's descriptor must describe what was sent (charter §6 step 5). */
static gboolean
descriptor_matches(GBytes *body, const gchar *sha256, gsize size)
{
  gsize length = 0;
  const gchar *text = body ? g_bytes_get_data(body, &length) : NULL;
  if (!text || length == 0)
    return FALSE;
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, text, (gssize)length, NULL))
    return FALSE;
  JsonNode *root = json_parser_get_root(parser);
  JsonObject *object = root && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;
  JsonNode *hash = object ? json_object_get_member(object, "sha256") : NULL;
  JsonNode *bytes = object ? json_object_get_member(object, "size") : NULL;
  if (!hash || !bytes || json_node_get_value_type(hash) != G_TYPE_STRING ||
      json_node_get_value_type(bytes) != G_TYPE_INT64)
    return FALSE;
  return g_ascii_strcasecmp(json_node_get_string(hash), sha256) == 0 &&
         json_node_get_int(bytes) == (gint64)size;
}

static void
on_uploaded(GObject *source, GAsyncResult *result, gpointer data)
{
  GTask *task = data;
  Upload *upload = g_task_get_task_data(task);
  GError *error = NULL;
  g_autoptr(GBytes) body = gh_net_http_send_finish(GH_NET_HTTP(source), result, &error);
  if (!body) {
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_task_return_error(task, error);
      g_object_unref(task);
      return;
    }
    if (error->domain == GH_NET_HTTP_ERROR &&
        (error->code == 401 || error->code == 403)) {
      /* Stop here: the user decides whether this server may see the
       * account, and the file goes nowhere else meanwhile (§6 step 4). */
      g_task_return_new_error(task, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_AUTH_REQUIRED,
                              upload->as_account
                                ? "%s refused the upload even from your account (%s)"
                                : "%s only accepts uploads from accounts it knows (%s)",
                              upload->host, error->message);
      g_error_free(error);
      g_object_unref(task);
      return;
    }
    if (error->domain == GH_NET_HTTP_ERROR) {
      GError *refused = g_error_new(GH_BLOSSOM_ERROR,
                                    error->code == 413 ? GH_BLOSSOM_ERROR_TOO_LARGE
                                                       : GH_BLOSSOM_ERROR_REFUSED,
                                    "%s: %s", upload->host, error->message);
      g_error_free(error);
      error = refused;
    }
    /* Unreachable, refused in this network mode, or refused by the server:
     * the next server may do. */
    upload_failed(task, error);
    return;
  }
  if (!descriptor_matches(body, upload->sha256, g_bytes_get_size(upload->ciphertext))) {
    upload_failed(task, g_error_new(GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_BAD_ANSWER,
                                    "%s answered with a different file", upload->host));
    return;
  }
  gchar *url = g_strconcat(upload->server, "/", upload->sha256, NULL);
  g_task_return_pointer(task, url, g_free);
  g_object_unref(task);
}

static void
upload_put(GTask *task, const gchar *auth_json)
{
  GhBlossomClient *self = g_task_get_source_object(task);
  Upload *upload = g_task_get_task_data(task);
  g_autofree gchar *encoded = g_base64_encode((const guchar *)auth_json, strlen(auth_json));
  g_autofree gchar *authorization = g_strconcat("Nostr ", encoded, NULL);
  g_autofree gchar *uri = g_strconcat(upload->server, "/upload", NULL);
  GhNetHttpRequest request = {
    .method = "PUT",
    .uri = uri,
    .accept = "application/json",
    .authorization = authorization,
    .content_type = "application/octet-stream",
    .body = upload->ciphertext,
    .max_bytes = DESCRIPTOR_MAX_BYTES,
  };
  gh_net_http_send_async(self->http, &request, g_task_get_cancellable(task), on_uploaded, task);
}

static void
on_account_signed(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  GTask *task = data;
  GhBlossomClient *self = g_task_get_source_object(task);
  Upload *upload = g_task_get_task_data(task);
  GError *error = NULL;
  g_autofree gchar *signed_json = self->sign_finish ? self->sign_finish(result, &error) : NULL;
  if (g_task_return_error_if_cancelled(task)) {
    g_clear_error(&error);
    g_object_unref(task);
    return;
  }
  if (!signed_json || !self->account_pubkey ||
      !auth_verify(signed_json, self->account_pubkey, upload->sha256, upload->host, upload->now)) {
    g_task_return_new_error(task, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_SIGNER,
                            "Your signer did not sign the upload%s%s", error ? ": " : "",
                            error ? error->message : "");
    g_clear_error(&error);
    g_object_unref(task);
    return;
  }
  upload_put(task, signed_json);
}

static void
upload_next(GTask *task)
{
  GhBlossomClient *self = g_task_get_source_object(task);
  Upload *upload = g_task_get_task_data(task);
  g_clear_pointer(&upload->server, g_free);
  g_clear_pointer(&upload->host, g_free);
  while (upload->servers[upload->next] && !upload->server)
    upload->server = normalize_server(upload->servers[upload->next++], &upload->host);
  if (!upload->server) {
    if (upload->last_error)
      g_task_return_error(task, g_steal_pointer(&upload->last_error));
    else
      g_task_return_new_error(task, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_NO_SERVER,
                              "Choose an attachment server in Preferences to send files");
    g_object_unref(task);
    return;
  }
  upload->now = g_get_real_time() / G_USEC_PER_SEC;
  upload->as_account = self->sign_async && g_hash_table_contains(self->consent, upload->server);
  if (upload->as_account) {
    NostrEvent *event = auth_event_new(self->account_pubkey, upload->sha256, upload->host,
                                       upload->now);
    char *raw = nostr_event_serialize_compact(event);
    nostr_event_free(event);
    g_autofree gchar *unsigned_json = raw ? g_strdup(raw) : NULL;
    free(raw);
    self->sign_async(self->sign_data, unsigned_json, g_task_get_cancellable(task),
                     on_account_signed, task);
    return;
  }
  g_autofree gchar *auth = auth_sign_ephemeral(upload->sha256, upload->host, upload->now);
  if (!auth) {
    g_task_return_new_error(task, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_SIGNER,
                            "Could not sign the upload");
    g_object_unref(task);
    return;
  }
  upload_put(task, auth);
}

void
gh_blossom_client_upload_async(GhBlossomClient *self, GBytes *ciphertext, const gchar *sha256_hex,
                               GCancellable *cancellable, GAsyncReadyCallback callback,
                               gpointer user_data)
{
  g_return_if_fail(GH_IS_BLOSSOM_CLIENT(self));
  g_return_if_fail(ciphertext != NULL && sha256_hex != NULL);
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, gh_blossom_client_upload_async);
  gsize size = g_bytes_get_size(ciphertext);
  g_autofree gchar *sha256 = g_ascii_strdown(sha256_hex, -1);
  if (!lower_hex64(sha256) || size == 0) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "An upload needs a non-empty file and its SHA-256");
    g_object_unref(task);
    return;
  }
  if (size > max_ciphertext(self)) {
    g_task_return_new_error(task, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_TOO_LARGE,
                            "Files can be at most %" G_GSIZE_FORMAT " MB",
                            self->max_file_size / (1024 * 1024));
    g_object_unref(task);
    return;
  }
  Upload *upload = g_new0(Upload, 1);
  upload->ciphertext = g_bytes_ref(ciphertext);
  memcpy(upload->sha256, sha256, 65);
  upload->servers = gh_blossom_client_dup_servers(self);
  g_task_set_task_data(task, upload, upload_free);
  if (g_task_return_error_if_cancelled(task)) {
    g_object_unref(task);
    return;
  }
  upload_next(task);
}

gchar *
gh_blossom_client_upload_finish(GhBlossomClient *self, GAsyncResult *result, gchar **out_server,
                                GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self), NULL);
  gchar *url = g_task_propagate_pointer(G_TASK(result), error);
  if (out_server) {
    Upload *upload = url ? g_task_get_task_data(G_TASK(result)) : NULL;
    *out_server = upload ? g_strdup(upload->server) : NULL;
  }
  return url;
}

/* ---- download -------------------------------------------------------------------- */

void
gh_blossom_client_set_allow_private_hosts(GhBlossomClient *self, gboolean allow)
{
  g_return_if_fail(GH_IS_BLOSSOM_CLIENT(self));
  self->allow_private_hosts = allow;
}

/* An IP address on the user's own machine or network, or no real address. */
static gboolean
address_private(GInetAddress *address)
{
  if (g_inet_address_get_is_loopback(address) || g_inet_address_get_is_link_local(address) ||
      g_inet_address_get_is_site_local(address) || g_inet_address_get_is_multicast(address) ||
      g_inet_address_get_is_any(address))
    return TRUE;
  const guint8 *b = g_inet_address_to_bytes(address);
  if (g_inet_address_get_family(address) == G_SOCKET_FAMILY_IPV4)
    return b[0] == 0 || b[0] >= 240 ||                 /* "this network", reserved */
           (b[0] == 100 && (b[1] & 0xc0) == 64) ||     /* 100.64/10 shared (CGNAT) */
           (b[0] == 192 && b[1] == 0 && b[2] == 0) ||  /* 192.0.0/24 IETF */
           (b[0] == 198 && (b[1] & 0xfe) == 18);       /* 198.18/15 benchmarking */
  if ((b[0] & 0xfe) == 0xfc)                           /* fc00::/7 unique local */
    return TRUE;
  static const guint8 mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
  if (memcmp(b, mapped, sizeof mapped) == 0) {         /* ::ffff:a.b.c.d */
    g_autoptr(GInetAddress) v4 = g_inet_address_new_from_bytes(b + 12, G_SOCKET_FAMILY_IPV4);
    return address_private(v4);
  }
  return FALSE;
}

/* A host a sender may point Download at: a public name or address, or a
 * .onion (reachable only in Tor mode, which GhNetHttp enforces). */
static gboolean
host_public(const gchar *host)
{
  g_autofree gchar *lower = g_ascii_strdown(host, -1);
  gsize len = strlen(lower);
  while (len > 0 && lower[len - 1] == '.')
    lower[--len] = '\0';
  if (len == 0 || strchr(lower, '%'))
    return FALSE; /* empty, or an IPv6 zone */
  if (g_str_has_suffix(lower, ".onion"))
    return TRUE;
  g_autoptr(GInetAddress) address = g_inet_address_new_from_string(lower);
  if (address)
    return !address_private(address);
  /* A name. No single label (the local search domain), no local names, and
   * nothing numeric that is not a canonical address (127.1, 0x7f.1). */
  const gchar *last = strrchr(lower, '.');
  if (!last)
    return FALSE;
  last++;
  gboolean numeric = *last != '\0';
  for (const gchar *p = last; *p; p++)
    numeric &= g_ascii_isdigit(*p);
  if (numeric || g_str_has_prefix(last, "0x") || strchr(lower, ':'))
    return FALSE;
  static const gchar *const local_suffixes[] = { "localhost", "local", "lan", "internal",
                                                 "home.arpa", "localdomain", NULL };
  for (guint i = 0; local_suffixes[i]; i++) {
    g_autofree gchar *dotted = g_strconcat(".", local_suffixes[i], NULL);
    if (g_str_equal(lower, local_suffixes[i]) || g_str_has_suffix(lower, dotted))
      return FALSE;
  }
  return TRUE;
}

/* The path names the blob: its last segment is sha256 (any case), maybe
 * with an extension (BUD-01 GET /<sha256>[.ext]). */
static gboolean
path_names_blob(const gchar *path, const gchar *sha256)
{
  const gchar *segment = path ? strrchr(path, '/') : NULL;
  if (!segment)
    return FALSE;
  segment++;
  if (g_ascii_strncasecmp(segment, sha256, 64) != 0)
    return FALSE;
  const gchar *rest = segment + 64;
  if (*rest == '\0')
    return TRUE;
  if (*rest != '.' || strlen(rest + 1) == 0 || strlen(rest + 1) > 16)
    return FALSE;
  for (const gchar *p = rest + 1; *p; p++)
    if (!g_ascii_isalnum(*p))
      return FALSE;
  return TRUE;
}

static gboolean
download_url_allowed(GhBlossomClient *self, const gchar *url, const gchar *sha256,
                     GError **error)
{
  g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_ENCODED, NULL);
  const gchar *host = uri ? g_uri_get_host(uri) : NULL;
  if (!uri || !host || g_uri_get_userinfo(uri) || g_uri_get_query(uri) ||
      g_uri_get_fragment(uri) || !hex64_any_case(sha256) ||
      !path_names_blob(g_uri_get_path(uri), sha256)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        "This file's address is not a Blossom address for it, so it isn't "
                        "downloaded");
    return FALSE;
  }
  if (!self->allow_private_hosts && !host_public(host)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        "This file's address points at your own computer or local network, "
                        "so it isn't downloaded");
    return FALSE;
  }
  return TRUE;
}

static void
on_downloaded(GObject *source, GAsyncResult *result, gpointer data)
{
  GTask *task = data;
  GError *error = NULL;
  GBytes *body = gh_net_http_get_finish(GH_NET_HTTP(source), result, &error);
  if (body)
    g_task_return_pointer(task, body, (GDestroyNotify)g_bytes_unref);
  else
    g_task_return_error(task, error);
  g_object_unref(task);
}

void
gh_blossom_client_download_async(GhBlossomClient *self, const gchar *url,
                                 const gchar *sha256_hex, guint64 size,
                                 GCancellable *cancellable, GAsyncReadyCallback callback,
                                 gpointer user_data)
{
  g_return_if_fail(GH_IS_BLOSSOM_CLIENT(self));
  g_return_if_fail(url != NULL && sha256_hex != NULL);
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, gh_blossom_client_download_async);
  GError *refused = NULL;
  if (!download_url_allowed(self, url, sha256_hex, &refused)) {
    g_task_return_error(task, refused);
    g_object_unref(task);
    return;
  }
  const gsize cap = max_ciphertext(self);
  if (size > cap) {
    g_task_return_new_error(task, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_TOO_LARGE,
                            "This file is larger than %" G_GSIZE_FORMAT " MB, so it isn't "
                            "downloaded", self->max_file_size / (1024 * 1024));
    g_object_unref(task);
    return;
  }
  const gsize limit = (size ? (gsize)size : cap) + GH_BLOSSOM_DOWNLOAD_SLACK;
  gh_net_http_get_accept_async(self->http, url, "application/octet-stream", limit, cancellable,
                               on_downloaded, task);
}

GBytes *
gh_blossom_client_download_finish(GhBlossomClient *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

/* ---- object ---------------------------------------------------------------------- */

GhBlossomClient *
gh_blossom_client_new(GSettings *settings, GhNetHttp *http)
{
  g_return_val_if_fail(!settings || G_IS_SETTINGS(settings), NULL);
  g_return_val_if_fail(GH_IS_NET_HTTP(http), NULL);
  GhBlossomClient *self = g_object_new(GH_TYPE_BLOSSOM_CLIENT, NULL);
  self->settings = settings ? g_object_ref(settings) : NULL;
  self->http = g_object_ref(http);
  return self;
}

static void
gh_blossom_client_dispose(GObject *object)
{
  GhBlossomClient *self = GH_BLOSSOM_CLIENT(object);
  if (self->sign_destroy && self->sign_data)
    self->sign_destroy(self->sign_data);
  self->sign_async = NULL;
  self->sign_destroy = NULL;
  self->sign_data = NULL;
  g_clear_object(&self->settings);
  g_clear_object(&self->http);
  G_OBJECT_CLASS(gh_blossom_client_parent_class)->dispose(object);
}

static void
gh_blossom_client_finalize(GObject *object)
{
  GhBlossomClient *self = GH_BLOSSOM_CLIENT(object);
  g_strfreev(self->servers_override);
  g_free(self->account_pubkey);
  g_hash_table_unref(self->consent);
  G_OBJECT_CLASS(gh_blossom_client_parent_class)->finalize(object);
}

static void
gh_blossom_client_class_init(GhBlossomClientClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = gh_blossom_client_dispose;
  G_OBJECT_CLASS(klass)->finalize = gh_blossom_client_finalize;
}

static void
gh_blossom_client_init(GhBlossomClient *self)
{
  self->consent = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->max_file_size = GH_BLOSSOM_MAX_FILE_SIZE;
}
