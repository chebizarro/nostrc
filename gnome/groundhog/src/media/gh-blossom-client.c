#include "gh-blossom-client.h"
#include "gh-attachment.h"

#include <nostr-gobject-1.0/gn-blossom-client.h>
#include "gh-net-session.h"
#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr/bud02/auth.h>
#include <secure_buf.h>
#include <stdlib.h>
#include <string.h>

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
 * user info; NULL for anything but an http(s) URL with a host. This defines
 * server/consent identity, not transport admission (GnBlossomClient's policy). */
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

gchar *
gh_blossom_client_normalize_server(const gchar *server, gchar **out_host)
{
  if (out_host)
    *out_host = NULL;
  return normalize_server(server, out_host);
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
gh_blossom_client_has_account_signer(GhBlossomClient *self)
{
  g_return_val_if_fail(GH_IS_BLOSSOM_CLIENT(self), FALSE);
  return self->sign_async != NULL;
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

/* ---- signer plumbing ------------------------------------------------------------- */

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
 * is wiped before this returns (never the account's, never reused); or, with
 * upload_key, by that key (a group picture's 0x8002 upload key, W25). */
static gchar *
auth_sign_ephemeral(NostrEvent *event, const guint8 *upload_key)
{
  gchar *signed_json = NULL;
  nostr_secure_buf secret = { 0 };
  char *secret_hex = NULL;
  if (upload_key) {
    secret_hex = malloc(65);
    for (guint i = 0; secret_hex && i < 32; i++)
      g_snprintf(secret_hex + 2 * i, 3, "%02x", upload_key[i]);
  } else {
    secret_hex = nostr_key_generate_private();
  }
  char *pubkey = secret_hex ? nostr_key_get_public(secret_hex) : NULL;
  gboolean have_secret = secret_from_hex(secret_hex, &secret);
  if (secret_hex) {
    secure_wipe(secret_hex, strlen(secret_hex));
    free(secret_hex);
  }
  if (pubkey && have_secret) {
    nostr_event_set_pubkey(event, pubkey);
    if (nostr_event_sign_secure(event, &secret) == 0) {
      char *raw = nostr_event_serialize_compact(event);
      signed_json = raw ? g_strdup(raw) : NULL;
      free(raw);
    }
  }
  if (have_secret)
    secure_free(&secret);
  free(pubkey);
  return signed_json;
}

/* ---- upload ---------------------------------------------------------------------- */

typedef struct {
  GBytes *ciphertext;
  gchar sha256[65];
  GStrv servers;
  guint next;
  gchar *server;      /* the one being tried (normalized) */
  gchar *host;        /* display name in upload errors */
  gboolean as_account;
  gchar auth_id[65]; /* bind the account signer's answer to its request */
  GError *last_error; /* the latest server's failure */
  nostr_secure_buf upload_key; /* keyed upload: its 32 secret bytes (locked, wiped), else empty */
  gboolean public_only; /* servers someone else named: public addresses only */
  gboolean public_file; /* a public issue file: its type is sent and named */
  gchar *content_type;  /* public file: the sniffed type (else octet-stream) */
  gchar *extension;     /* public file: the type's extension, or NULL */
} Upload;

static void
upload_free(gpointer data)
{
  Upload *upload = data;
  if (upload->upload_key.ptr)
    secure_free(&upload->upload_key);
  g_bytes_unref(upload->ciphertext);
  g_strfreev(upload->servers);
  g_free(upload->server);
  g_free(upload->host);
  g_free(upload->content_type);
  g_free(upload->extension);
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

/* A file extension as BUD-01 allows it after /<sha256>: 1 to 16 ASCII
 * letters and digits. */
static gboolean
extension_ok(const gchar *extension)
{
  gsize length = extension ? strlen(extension) : 0;
  if (length == 0 || length > 16)
    return FALSE;
  for (const gchar *p = extension; *p; p++)
    if (!g_ascii_isalnum(*p))
      return FALSE;
  return TRUE;
}

/* The extension of the descriptor's url (BUD-02), if that url is exactly
 * <server>/<sha256>.<extension> on the server the file went to; NULL
 * otherwise (no url, another place, a query, or no extension). Only the
 * extension is taken from the server: the address is still built here. */
static gchar *
descriptor_extension(const gchar *url, const gchar *server, const gchar *sha256)
{
  const gchar *slash = url ? strrchr(url, '/') : NULL;
  if (!slash)
    return NULL;
  g_autofree gchar *base = g_strndup(url, (gsize)(slash - url));
  g_autofree gchar *normalized = normalize_server(base, NULL);
  const gchar *name = slash + 1;
  if (!normalized || !g_str_equal(normalized, server) ||
      g_ascii_strncasecmp(name, sha256, 64) != 0 || name[64] != '.' ||
      !extension_ok(name + 65))
    return NULL;
  return g_ascii_strdown(name + 65, -1);
}

/* Route policy only: libblossom owns HTTP, redirects and reception guards. */
typedef struct {
  GSettings *settings;
  GCancellable *caller, *cancel;
  gulong caller_id, settings_id;
  GhNetMode mode;
  gchar *tor_address;
  gboolean public_only, allowed_local, revoked;
} Transfer;

static gchar *
policy_string(GSettings *settings, const gchar *key, const gchar *fallback)
{
  g_autoptr(GSettingsSchema) schema = NULL;
  if (settings) g_object_get(settings, "settings-schema", &schema, NULL);
  return schema && g_settings_schema_has_key(schema, key)
    ? g_settings_get_string(settings, key) : g_strdup(fallback);
}

static gboolean
policy_local(GSettings *settings)
{
  g_autoptr(GSettingsSchema) schema = NULL;
  if (settings) g_object_get(settings, "settings-schema", &schema, NULL);
  return schema && g_settings_schema_has_key(schema, "allow-local-network") &&
    g_settings_get_boolean(settings, "allow-local-network");
}

static void
transfer_policy_changed(GSettings *settings, const gchar *key, gpointer data)
{
  (void)key;
  Transfer *t = data;
  g_autofree gchar *mode = policy_string(settings, "network-mode", "system");
  g_autofree gchar *address = policy_string(settings, "tor-socks-address", NULL);
  if (gh_net_mode_from_string(mode) != t->mode ||
      (t->mode == GH_NET_MODE_TOR && g_strcmp0(address, t->tor_address)) ||
      (t->public_only && t->allowed_local && !policy_local(settings))) {
    t->revoked = TRUE;
    g_cancellable_cancel(t->cancel);
  }
}

static void
transfer_free(gpointer data)
{
  Transfer *t = data;
  if (t->settings_id) g_signal_handler_disconnect(t->settings, t->settings_id);
  if (t->caller_id) g_cancellable_disconnect(t->caller, t->caller_id);
  g_clear_object(&t->settings); g_clear_object(&t->caller); g_clear_object(&t->cancel);
  g_free(t->tor_address); g_free(t);
}

static void
transfer_caller_cancelled(GCancellable *caller, gpointer data)
{
  (void)caller;
  g_cancellable_cancel(G_CANCELLABLE(data));
}

static GnBlossomClient *
transfer_new(GhBlossomClient *self, const gchar *endpoint, gsize limit, gboolean public_only,
             GCancellable *caller, GError **error)
{
  Transfer *t = g_new0(Transfer, 1);
  t->settings = gh_net_http_dup_settings(self->http);
  t->cancel = g_cancellable_new();
  t->caller = caller ? g_object_ref(caller) : NULL;
  if (caller) t->caller_id = g_cancellable_connect(caller, G_CALLBACK(transfer_caller_cancelled),
      g_object_ref(t->cancel), g_object_unref);
  g_autofree gchar *mode = policy_string(t->settings, "network-mode", "system");
  t->mode = gh_net_mode_from_string(mode);
  t->tor_address = policy_string(t->settings, "tor-socks-address", NULL);
  t->public_only = public_only;
  t->allowed_local = gh_net_http_allows_local_network(self->http);
  g_autoptr(GProxyResolver) resolver = gh_net_proxy_resolver_new(t->mode, t->tor_address, NULL, error);
  if (!resolver) { transfer_free(t); return NULL; }
  GnBlossomNetworkFlags flags = GN_BLOSSOM_NETWORK_PRIVATE_TLS;
  if (t->mode == GH_NET_MODE_TOR) flags |= GN_BLOSSOM_NETWORK_TOR;
  else if (!public_only || t->allowed_local) flags |= GN_BLOSSOM_NETWORK_ALLOW_LOCAL;
  g_autoptr(GUri) uri = g_uri_parse(endpoint, G_URI_FLAGS_ENCODED, NULL);
  const gchar *host = uri ? g_uri_get_host(uri) : NULL;
  if (self->allow_private_hosts || (host && gh_net_host_is_loopback(host)))
    flags |= GN_BLOSSOM_NETWORK_LOCAL_TEST;
  GnBlossomClient *client = gn_blossom_client_new(endpoint);
  gn_blossom_client_set_network(client, resolver, flags);
  gn_blossom_client_set_limits(client, limit, GH_NET_HTTP_TIMEOUT_S, "");
  if (t->settings)
    t->settings_id = g_signal_connect(t->settings, "changed", G_CALLBACK(transfer_policy_changed), t);
  g_object_set_data_full(G_OBJECT(client), "groundhog-route", t, transfer_free);
  return client;
}

static gboolean
transfer_revoked(GnBlossomClient *client, GError **error)
{
  Transfer *t = g_object_get_data(G_OBJECT(client), "groundhog-route");
  if (t->mode == GH_NET_MODE_TOR && !t->revoked) {
    g_autoptr(GhNetSession) session = gh_net_session_dup_default();
    if (session) gh_net_session_report(session, !*error);
  }
  if (!t->revoked || (t->caller && g_cancellable_is_cancelled(t->caller))) return FALSE;
  g_clear_error(error);
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED,
                      "The network setting changed before the server answered");
  return TRUE;
}

static void
on_uploaded(GObject *source, GAsyncResult *result, gpointer data)
{
  GTask *task = data;
  Upload *upload = g_task_get_task_data(task);
  GError *error = NULL;
  g_autofree gchar *body = gn_blossom_client_upload_finish(GN_BLOSSOM_CLIENT(source), result, &error);
  if (transfer_revoked(GN_BLOSSOM_CLIENT(source), &error))
    g_clear_pointer(&body, g_free);
  if (!body) {
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_task_return_error(task, error);
      g_object_unref(task);
      return;
    }
    if (g_error_matches(error, GN_BLOSSOM_ERROR, GN_BLOSSOM_ERROR_AUTH)) {
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
    if (error->domain == GN_BLOSSOM_ERROR) {
      GError *refused = g_error_new(GH_BLOSSOM_ERROR,
                                    error->code == GN_BLOSSOM_ERROR_TOO_LARGE ? GH_BLOSSOM_ERROR_TOO_LARGE
                                    : error->code == GN_BLOSSOM_ERROR_BAD_RESPONSE ? GH_BLOSSOM_ERROR_BAD_ANSWER
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
  /* A public file is addressed with an extension (BUD-01 /<sha256>.<ext>)
   * so that it is shown inline: the one the server named in its descriptor,
   * else the one of the type it was sent as. */
  g_autofree gchar *extension = upload->public_file
    ? descriptor_extension(body, upload->server, upload->sha256) : NULL;
  if (!extension && upload->public_file && upload->extension)
    extension = g_strdup(upload->extension);
  gchar *url = extension
    ? g_strconcat(upload->server, "/", upload->sha256, ".", extension, NULL)
    : g_strconcat(upload->server, "/", upload->sha256, NULL);
  g_task_return_pointer(task, url, g_free);
  g_object_unref(task);
}

static void
upload_put(GTask *task, const gchar *auth_json)
{
  GhBlossomClient *self = g_task_get_source_object(task);
  Upload *upload = g_task_get_task_data(task);
  GError *error = NULL;
  g_autoptr(GnBlossomClient) client = transfer_new(self, upload->server, max_ciphertext(self),
      upload->public_only, g_task_get_cancellable(task), &error);
  if (!client) { upload_failed(task, error); return; }
  Transfer *transfer = g_object_get_data(G_OBJECT(client), "groundhog-route");
  gn_blossom_client_upload_async(client, upload->ciphertext, upload->sha256,
      upload->content_type ? upload->content_type : "application/octet-stream", auth_json,
      transfer->cancel, on_uploaded, task);
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
  NostrEvent *event = nostr_event_new();
  nostr_bud02_validate_options_t options = {
    .expected_sha256 = upload->sha256,
    .expected_pubkey = self->account_pubkey,
  };
  gboolean valid = signed_json && self->account_pubkey && event &&
    nostr_event_deserialize_signed(event, signed_json, NULL) == NOSTR_EVENT_VALIDATION_OK;
  g_autofree gchar *signed_id = valid ? nostr_event_get_id(event) : NULL;
  valid = valid && signed_id && g_strcmp0(signed_id, upload->auth_id) == 0 &&
    nostr_bud02_validate_auth_event(event, NOSTR_BUD02_ACTION_UPLOAD, &options) == NOSTR_BUD02_OK;
  nostr_event_free(event);
  if (!valid) {
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
  if (g_task_return_error_if_cancelled(task)) { g_object_unref(task); return; }
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
  NostrEvent *event = nostr_bud02_create_auth_event(NOSTR_BUD02_ACTION_UPLOAD,
      upload->sha256, 0, upload->server);
  if (!event) {
    g_task_return_new_error(task, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_SIGNER,
                            "Could not prepare upload authorization");
    g_object_unref(task);
    return;
  }
  /* A keyed upload is never the account's: the key is the blob's owner. */
  upload->as_account = !upload->upload_key.ptr && self->sign_async &&
                       g_hash_table_contains(self->consent, upload->server);
  if (upload->as_account) {
    nostr_event_set_pubkey(event, self->account_pubkey);
    gboolean valid = nostr_event_compute_id(event, upload->auth_id) == NOSTR_EVENT_VALIDATION_OK;
    char *raw = valid ? nostr_event_serialize_compact(event) : NULL;
    nostr_event_free(event);
    g_autofree gchar *unsigned_json = raw ? g_strdup(raw) : NULL;
    free(raw);
    if (!unsigned_json) {
      g_task_return_new_error(task, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_SIGNER,
                              "Could not prepare upload authorization");
      g_object_unref(task);
      return;
    }
    self->sign_async(self->sign_data, unsigned_json, g_task_get_cancellable(task),
                     on_account_signed, task);
    return;
  }
  g_autofree gchar *auth = auth_sign_ephemeral(event, upload->upload_key.ptr);
  nostr_event_free(event);
  if (!auth) {
    g_task_return_new_error(task, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_SIGNER,
                            "Could not sign the upload");
    g_object_unref(task);
    return;
  }
  upload_put(task, auth);
}

static GTask *upload_start(GhBlossomClient *self, GBytes *ciphertext, const gchar *sha256_hex,
                           const gchar *const *servers, const guint8 *upload_key,
                           gboolean public_only, GCancellable *cancellable,
                           GAsyncReadyCallback callback, gpointer user_data);
static void upload_begin(GTask *task);

void
gh_blossom_client_upload_async(GhBlossomClient *self, GBytes *ciphertext, const gchar *sha256_hex,
                               GCancellable *cancellable, GAsyncReadyCallback callback,
                               gpointer user_data)
{
  g_return_if_fail(GH_IS_BLOSSOM_CLIENT(self));
  g_return_if_fail(ciphertext != NULL && sha256_hex != NULL);
  upload_begin(upload_start(self, ciphertext, sha256_hex, NULL, NULL, FALSE, cancellable,
                            callback, user_data));
}

void
gh_blossom_client_upload_on_servers_async(GhBlossomClient *self, const gchar *const *servers,
                                               GBytes *ciphertext, const gchar *sha256_hex,
                                               GCancellable *cancellable,
                                               GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GH_IS_BLOSSOM_CLIENT(self));
  g_return_if_fail(servers != NULL && ciphertext != NULL && sha256_hex != NULL);
  upload_begin(upload_start(self, ciphertext, sha256_hex, servers, NULL, FALSE, cancellable,
                            callback, user_data));
}

void
gh_blossom_client_upload_keyed_async(GhBlossomClient *self, const gchar *const *servers,
                                     GBytes *ciphertext, const gchar *sha256_hex,
                                     const guint8 upload_key[32], GCancellable *cancellable,
                                     GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GH_IS_BLOSSOM_CLIENT(self));
  g_return_if_fail(servers != NULL && ciphertext != NULL && sha256_hex != NULL &&
                   upload_key != NULL);
  upload_begin(upload_start(self, ciphertext, sha256_hex, servers, upload_key, TRUE, cancellable,
                            callback, user_data));
}

void
gh_blossom_client_upload_public_async(GhBlossomClient *self, const gchar *const *servers,
                                      GBytes *bytes, const gchar *sha256_hex,
                                      const gchar *content_type, const gchar *extension,
                                      GCancellable *cancellable, GAsyncReadyCallback callback,
                                      gpointer user_data)
{
  g_return_if_fail(GH_IS_BLOSSOM_CLIENT(self));
  g_return_if_fail(servers != NULL && bytes != NULL && sha256_hex != NULL);
  g_return_if_fail(extension == NULL || extension_ok(extension));
  GTask *task = upload_start(self, bytes, sha256_hex, servers, NULL, TRUE, cancellable, callback,
                             user_data);
  Upload *upload = task ? g_task_get_task_data(task) : NULL;
  if (upload) {
    upload->public_file = TRUE;
    upload->content_type = g_strdup(content_type ? content_type : "application/octet-stream");
    upload->extension = extension ? g_ascii_strdown(extension, -1) : NULL;
  }
  upload_begin(task);
}

/* The upload's task, ready for upload_begin(); NULL if it already
 * finished (an invalid file or a cancellable already cancelled). */
static GTask *
upload_start(GhBlossomClient *self, GBytes *ciphertext, const gchar *sha256_hex,
             const gchar *const *servers, const guint8 *upload_key, gboolean public_only,
             GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, gh_blossom_client_upload_async);
  gsize size = g_bytes_get_size(ciphertext);
  g_autofree gchar *sha256 = g_ascii_strdown(sha256_hex, -1);
  if (!lower_hex64(sha256) || size == 0) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "An upload needs a non-empty file and its SHA-256");
    g_object_unref(task);
    return NULL;
  }
  if (size > max_ciphertext(self)) {
    g_task_return_new_error(task, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_TOO_LARGE,
                            "Files can be at most %" G_GSIZE_FORMAT " MB",
                            self->max_file_size / (1024 * 1024));
    g_object_unref(task);
    return NULL;
  }
  Upload *upload = g_new0(Upload, 1);
  upload->ciphertext = g_bytes_ref(ciphertext);
  memcpy(upload->sha256, sha256, 65);
  upload->servers = servers ? g_strdupv((gchar **)servers) : gh_blossom_client_dup_servers(self);
  if (upload_key) {
    upload->upload_key = secure_alloc(32);
    if (!upload->upload_key.ptr) {
      upload_free(upload);
      g_task_return_new_error(task, GH_BLOSSOM_ERROR, GH_BLOSSOM_ERROR_SIGNER,
                              "Could not hold the upload key");
      g_object_unref(task);
      return NULL;
    }
    memcpy(upload->upload_key.ptr, upload_key, 32);
  }
  if (public_only) {
    /* Servers someone else named (a group's), or a file whose address goes
     * into a public issue: public addresses only, as for every download of a
     * URL someone else chose (W25 review M1). */
    upload->public_only = !self->allow_private_hosts;
    g_auto(GStrv) usable = gh_blossom_client_dup_public_servers(self, servers);
    g_strfreev(upload->servers);
    upload->servers = g_steal_pointer(&usable);
  }
  g_task_set_task_data(task, upload, upload_free);
  if (g_task_return_error_if_cancelled(task)) {
    g_object_unref(task);
    return NULL;
  }
  return task;
}

static void
upload_begin(GTask *task)
{
  if (task)
    upload_next(task);
}

gchar *
gh_blossom_client_upload_finish(GhBlossomClient *self, GAsyncResult *result, gchar **out_server,
                                GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self), NULL);
  GError *local = NULL;
  gchar *url = g_task_propagate_pointer(G_TASK(result), &local);
  if (out_server) {
    /* The server used, or the one that asked for a known account (the
     * user's consent is per server). */
    gboolean named = url || g_error_matches(local, GH_BLOSSOM_ERROR,
                                            GH_BLOSSOM_ERROR_AUTH_REQUIRED);
    Upload *upload = named ? g_task_get_task_data(G_TASK(result)) : NULL;
    *out_server = upload ? g_strdup(upload->server) : NULL;
  }
  if (local)
    g_propagate_error(error, local);
  return url;
}

/* ---- download -------------------------------------------------------------------- */

void
gh_blossom_client_set_allow_private_hosts(GhBlossomClient *self, gboolean allow)
{
  g_return_if_fail(GH_IS_BLOSSOM_CLIENT(self));
  self->allow_private_hosts = allow;
}

/* App policy for sender-supplied downloads and public-only server lists:
 * reject local/search-domain names even through a proxy. Transport guards
 * enforce resolved addresses and Tor routing; they cannot classify a name
 * resolved remotely by a proxy. */
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
    return gh_net_address_is_public(address);
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

GStrv
gh_blossom_client_dup_public_servers(GhBlossomClient *self, const gchar *const *servers)
{
  g_return_val_if_fail(GH_IS_BLOSSOM_CLIENT(self), NULL);
  g_autoptr(GStrvBuilder) out = g_strv_builder_new();
  for (guint i = 0; servers && servers[i]; i++) {
    g_autofree gchar *host = NULL;
    g_autofree gchar *normalized = normalize_server(servers[i], &host);
    if (normalized && (self->allow_private_hosts || gh_net_http_allows_local_network(self->http) ||
                       host_public(host)))
      g_strv_builder_add(out, servers[i]);
  }
  return g_strv_builder_end(out);
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

/* Only media addressing and public-host policy here. Generic URL syntax,
 * scheme, credentials, query/fragment and connected-address guards belong to
 * GnBlossomClient/libblossom and also apply on redirects. */
static gboolean
download_url_allowed(GhBlossomClient *self, const gchar *url, const gchar *sha256,
                     GError **error)
{
  g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_ENCODED, NULL);
  const gchar *host = uri ? g_uri_get_host(uri) : NULL;
  if (!uri || !host || !hex64_any_case(sha256) ||
      !path_names_blob(g_uri_get_path(uri), sha256)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        "This file's address is not a Blossom address for it, so it isn't "
                        "downloaded");
    return FALSE;
  }
  if (!self->allow_private_hosts && !gh_net_http_allows_local_network(self->http) &&
      !host_public(host)) {
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
  GBytes *body = gn_blossom_client_download_finish(GN_BLOSSOM_CLIENT(source), result, &error);
  if (transfer_revoked(GN_BLOSSOM_CLIENT(source), &error))
    g_clear_pointer(&body, g_bytes_unref);
  if (body)
    g_task_return_pointer(task, body, (GDestroyNotify)g_bytes_unref);
  else {
    if (error->domain == GN_BLOSSOM_ERROR) {
      GQuark domain = error->code == GN_BLOSSOM_ERROR_BAD_RESPONSE ? GH_ATTACHMENT_ERROR : G_IO_ERROR;
      gint code = error->code == GN_BLOSSOM_ERROR_BAD_RESPONSE ? GH_ATTACHMENT_ERROR_DAMAGED :
        error->code == GN_BLOSSOM_ERROR_TOO_LARGE ? G_IO_ERROR_MESSAGE_TOO_LARGE : G_IO_ERROR_FAILED;
      GError *mapped = g_error_new_literal(domain, code, error->message);
      g_error_free(error); error = mapped;
    }
    g_task_return_error(task, error);
  }
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
  g_autofree gchar *base = g_strndup(url, (gsize)(strrchr(url, '/') - url));
  g_autoptr(GnBlossomClient) client = transfer_new(self, base, limit, TRUE, cancellable, &refused);
  if (!client) { g_task_return_error(task, refused); g_object_unref(task); return; }
  Transfer *transfer = g_object_get_data(G_OBJECT(client), "groundhog-route");
  gn_blossom_client_download_async(client, url, sha256_hex, transfer->cancel, on_downloaded, task);
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
