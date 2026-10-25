/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gn-blossom-client.h"
#include <blossom/client.h>
#include <stdlib.h>
#include <string.h>

struct _GnBlossomClient {
  GObject parent_instance;
  gchar *endpoint;
  gchar *user_agent;
  GProxyResolver *resolver;
  GnBlossomNetworkFlags flags;
  gsize max_bytes;
  guint timeout;
};
G_DEFINE_FINAL_TYPE(GnBlossomClient, gn_blossom_client, G_TYPE_OBJECT)
G_DEFINE_QUARK(gn-blossom-error-quark, gn_blossom_error)

typedef struct {
  gchar *endpoint, *agent, *hash, *type, *auth, *url;
  GBytes *bytes;
  GProxyResolver *resolver;
  GCancellable *cancellable;
  GnBlossomNetworkFlags flags;
  gsize max_bytes;
  guint timeout;
  enum { REQUEST_UPLOAD, REQUEST_DOWNLOAD, REQUEST_LIST, REQUEST_DELETE } operation;
  GError *route_error;
} Request;

static void request_free(gpointer data)
{
  Request *r = data;
  g_free(r->endpoint); g_free(r->agent); g_free(r->hash);
  g_free(r->type); g_free(r->auth); g_free(r->url);
  g_clear_pointer(&r->bytes, g_bytes_unref);
  g_clear_object(&r->resolver); g_clear_object(&r->cancellable);
  g_clear_error(&r->route_error);
  g_free(r);
}
static bool cancelled(void *data)
{
  Request *r = data;
  return r->cancellable && g_cancellable_is_cancelled(r->cancellable);
}
static gboolean onion_host(const gchar *host)
{
  g_autofree gchar *s = g_ascii_strdown(host, -1);
  gsize n = strlen(s);
  while (n && s[n - 1] == '.') s[--n] = 0;
  return g_str_has_suffix(s, ".onion");
}
static blossom_error_t resolve_direct(Request *r, const gchar *host, char **addresses)
{
  g_autoptr(GResolver) resolver = g_resolver_get_default();
  GList *list = g_resolver_lookup_by_name(resolver, host, r->cancellable, &r->route_error);
  if (!list) return cancelled(r) ? BLOSSOM_ERR_CANCELLED : BLOSSOM_ERR_NETWORK;
  GString *pins = g_string_new(NULL);
  for (GList *p = list; p; p = p->next) {
    g_autofree gchar *ip = g_inet_address_to_string(p->data);
    if (pins->len) g_string_append_c(pins, ',');
    if (g_inet_address_get_family(p->data) == G_SOCKET_FAMILY_IPV6) g_string_append_printf(pins, "[%s]", ip);
    else g_string_append(pins, ip);
  }
  g_resolver_free_addresses(list);
  *addresses = strdup(pins->str); g_string_free(pins, TRUE);
  return *addresses ? BLOSSOM_OK : BLOSSOM_ERR_NOMEM;
}
static blossom_error_t prepare(const char *url, char **proxy, char **addresses, void *data)
{
  Request *r = data;
  g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_ENCODED, NULL);
  const gchar *host = uri ? g_uri_get_host(uri) : NULL;
  const gchar *scheme = uri ? g_uri_get_scheme(uri) : NULL;
  gboolean tor = (r->flags & GN_BLOSSOM_NETWORK_TOR) != 0;
  g_autoptr(GInetAddress) address = host ? g_inet_address_new_from_string(host) : NULL;
  gboolean onion = host && onion_host(host);
  gboolean http_ok = (tor && onion) ||
    ((r->flags & GN_BLOSSOM_NETWORK_LOCAL_TEST) && address && g_inet_address_get_is_loopback(address));
  if (!host || !scheme || g_uri_get_userinfo(uri) || g_uri_get_query(uri) || g_uri_get_fragment(uri) ||
      (onion && !tor) || (g_ascii_strcasecmp(scheme, "https") &&
       !(http_ok && !g_ascii_strcasecmp(scheme, "http"))))
    return BLOSSOM_ERR_POLICY;
  if (!r->resolver) {
    if (tor) return BLOSSOM_ERR_POLICY;
    *proxy = strdup("");
    if (!*proxy) return BLOSSOM_ERR_NOMEM;
    return address ? BLOSSOM_OK : resolve_direct(r, host, addresses);
  }
  g_clear_error(&r->route_error);
  g_auto(GStrv) routes = g_proxy_resolver_lookup(r->resolver, url, r->cancellable, &r->route_error);
  if (!routes || !routes[0]) return cancelled(r) ? BLOSSOM_ERR_CANCELLED : BLOSSOM_ERR_POLICY;
  const gchar *route = routes[0];
  g_autofree gchar *remote = NULL;
  if (g_str_has_prefix(route, "socks5://")) remote = g_strconcat("socks5h://", route + 9, NULL);
  else if (g_str_has_prefix(route, "socks4://")) remote = g_strconcat("socks4a://", route + 9, NULL);
  else remote = g_strdup(route);
  gboolean socks5 = g_str_has_prefix(remote, "socks5h://");
  gboolean direct = g_str_equal(remote, "direct://");
  if ((tor && !socks5) || (!direct && !socks5 && !g_str_has_prefix(remote, "socks4a://") &&
      !g_str_has_prefix(remote, "http://") && !g_str_has_prefix(remote, "https://")))
    return BLOSSOM_ERR_POLICY;
  *proxy = strdup(direct ? "" : remote);
  if (!*proxy) return BLOSSOM_ERR_NOMEM;
  return direct && !address ? resolve_direct(r, host, addresses) : BLOSSOM_OK;
}
static GError *operation_error(Request *r, blossom_error_t code, blossom_client_t *client)
{
  if (r->route_error) return g_steal_pointer(&r->route_error);
  if (cancelled(r) || code == BLOSSOM_ERR_CANCELLED)
    return g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "Blossom request cancelled");
  if (code == BLOSSOM_ERR_POLICY || code == BLOSSOM_ERR_INVALID_ARG)
    return g_error_new_literal(G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Blossom address or route refused by network policy");
  if (code == BLOSSOM_ERR_UNSUPPORTED)
    return g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Private TLS requires OpenSSL-backed libcurl");
  if (code == BLOSSOM_ERR_TIMEOUT)
    return g_error_new_literal(G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Blossom server timed out");
  long status = blossom_client_http_status(client);
  if (code == BLOSSOM_ERR_NOT_FOUND || status == 410)
    return g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Blossom server no longer has this file");
  if (code == BLOSSOM_ERR_NETWORK && !status)
    return g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NETWORK_UNREACHABLE, "Cannot reach Blossom server through the selected route");
  GnBlossomError error = GN_BLOSSOM_ERROR_FAILED;
  const gchar *message = "Blossom request failed";
  if (code == BLOSSOM_ERR_AUTH) { error = GN_BLOSSOM_ERROR_AUTH; message = "Blossom server refused authorization"; }
  else if (code == BLOSSOM_ERR_TOO_LARGE) { error = GN_BLOSSOM_ERROR_TOO_LARGE; message = "Blossom file or response exceeds the size limit"; }
  else if (code == BLOSSOM_ERR_HASH_MISMATCH) { error = GN_BLOSSOM_ERROR_BAD_RESPONSE; message = "Blossom server answered with a different file"; }
  if (status && (status < 200 || status >= 300))
    return g_error_new(GN_BLOSSOM_ERROR, error, "%s (HTTP %ld%s%s)", message, status,
        *blossom_client_http_reason(client) ? ":" : "", blossom_client_http_reason(client));
  return g_error_new_literal(GN_BLOSSOM_ERROR, error, message);
}
static void worker(GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
  (void)source; (void)cancellable;
  Request *r = data;
  if (g_task_return_error_if_cancelled(task)) return;
  blossom_transport_t transport = {
    .prepare = prepare, .cancelled = cancelled, .user_data = r,
    .allow_http = (r->flags & (GN_BLOSSOM_NETWORK_LOCAL_TEST | GN_BLOSSOM_NETWORK_TOR)) != 0,
    .allow_private_addresses = !(r->flags & GN_BLOSSOM_NETWORK_TOR) &&
      (r->flags & (GN_BLOSSOM_NETWORK_LOCAL_TEST | GN_BLOSSOM_NETWORK_ALLOW_LOCAL)),
    .disable_tls_sessions = (r->flags & GN_BLOSSOM_NETWORK_PRIVATE_TLS) != 0
  };
  blossom_client_opts_t opts = {
    .endpoint = r->endpoint, .timeout_seconds = r->timeout,
    .max_blob_bytes = r->max_bytes, .user_agent = r->agent, .transport = &transport
  };
  blossom_client_t *client = NULL;
  blossom_error_t rc = blossom_client_new(&opts, NULL, &client);
  gpointer answer = NULL;
  if (rc == BLOSSOM_OK && r->bytes) {
    rc = blossom_client_set_upload_content_type(client, r->type);
    blossom_blob_descriptor_t *desc = NULL;
    gsize size = 0;
    const guint8 *bytes = g_bytes_get_data(r->bytes, &size);
    if (rc == BLOSSOM_OK) rc = blossom_client_put_signed(client, bytes, size, r->hash, r->auth, &desc);
    if (rc == BLOSSOM_OK)
      answer = g_strdup(desc->url ? desc->url : "");
    blossom_blob_descriptor_free(desc);
  } else if (rc == BLOSSOM_OK && r->operation == REQUEST_DELETE) {
    rc = blossom_client_delete_signed(client, r->hash, r->auth);
  } else if (rc == BLOSSOM_OK && r->operation == REQUEST_LIST) {
    char *json = NULL; size_t size = 0;
    rc = blossom_client_list(client, r->hash, &json, &size);
    if (rc == BLOSSOM_OK) answer = g_bytes_new_with_free_func(json, size, free, json);
  } else if (rc == BLOSSOM_OK) {
    uint8_t *bytes = NULL; size_t size = 0;
    rc = blossom_client_get_url(client, r->url, r->hash, &bytes, &size);
    if (rc == BLOSSOM_OK) {
      /* libblossom uses malloc, not a GLib allocator. */
      answer = g_bytes_new_with_free_func(bytes, size, free, bytes);
    }
  }
  if (rc != BLOSSOM_OK) g_task_return_error(task, operation_error(r, rc, client));
  else if (r->operation == REQUEST_DELETE) g_task_return_boolean(task, TRUE);
  else g_task_return_pointer(task, answer, r->bytes ? g_free : (GDestroyNotify)g_bytes_unref);
  blossom_client_free(client);
}
static GTask *request_new(GnBlossomClient *self, GCancellable *cancel,
                          GAsyncReadyCallback callback, gpointer data)
{
  GTask *task = g_task_new(self, cancel, callback, data);
  Request *r = g_new0(Request, 1);
  r->endpoint = g_strdup(self->endpoint); r->agent = g_strdup(self->user_agent);
  r->resolver = self->resolver ? g_object_ref(self->resolver) : NULL;
  r->cancellable = cancel ? g_object_ref(cancel) : NULL;
  r->flags = self->flags; r->max_bytes = self->max_bytes; r->timeout = self->timeout;
  g_task_set_task_data(task, r, request_free);
  return task;
}
void gn_blossom_client_upload_async(GnBlossomClient *self, GBytes *bytes,
    const gchar *hash, const gchar *type, const gchar *auth, GCancellable *cancel,
    GAsyncReadyCallback callback, gpointer data)
{
  g_return_if_fail(GN_IS_BLOSSOM_CLIENT(self) && bytes && hash && auth);
  GTask *task = request_new(self, cancel, callback, data);
  g_task_set_source_tag(task, gn_blossom_client_upload_async);
  Request *r = g_task_get_task_data(task);
  r->bytes = g_bytes_ref(bytes); r->hash = g_strdup(hash);
  r->type = g_strdup(type); r->auth = g_strdup(auth);
  g_task_run_in_thread(task, worker); g_object_unref(task);
}
gchar *gn_blossom_client_upload_finish(GnBlossomClient *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self) &&
    g_task_get_source_tag(G_TASK(result)) == gn_blossom_client_upload_async, NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}
void gn_blossom_client_download_async(GnBlossomClient *self, const gchar *url,
    const gchar *hash, GCancellable *cancel, GAsyncReadyCallback callback, gpointer data)
{
  g_return_if_fail(GN_IS_BLOSSOM_CLIENT(self) && url && hash);
  GTask *task = request_new(self, cancel, callback, data);
  g_task_set_source_tag(task, gn_blossom_client_download_async);
  Request *r = g_task_get_task_data(task);
  r->operation = REQUEST_DOWNLOAD;
  r->url = g_strdup(url); r->hash = g_strdup(hash);
  g_task_run_in_thread(task, worker); g_object_unref(task);
}
GBytes *gn_blossom_client_download_finish(GnBlossomClient *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self) &&
    g_task_get_source_tag(G_TASK(result)) == gn_blossom_client_download_async, NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}
void gn_blossom_client_list_async(GnBlossomClient *self, const gchar *pubkey,
    GCancellable *cancel, GAsyncReadyCallback callback, gpointer data)
{
  g_return_if_fail(GN_IS_BLOSSOM_CLIENT(self) && pubkey);
  GTask *task = request_new(self, cancel, callback, data);
  g_task_set_source_tag(task, gn_blossom_client_list_async);
  Request *r = g_task_get_task_data(task);
  r->operation = REQUEST_LIST; r->hash = g_strdup(pubkey);
  g_task_run_in_thread(task, worker); g_object_unref(task);
}
GBytes *gn_blossom_client_list_finish(GnBlossomClient *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self) &&
    g_task_get_source_tag(G_TASK(result)) == gn_blossom_client_list_async, NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}
void gn_blossom_client_delete_async(GnBlossomClient *self, const gchar *hash,
    const gchar *auth, GCancellable *cancel, GAsyncReadyCallback callback, gpointer data)
{
  g_return_if_fail(GN_IS_BLOSSOM_CLIENT(self) && hash && auth);
  GTask *task = request_new(self, cancel, callback, data);
  g_task_set_source_tag(task, gn_blossom_client_delete_async);
  Request *r = g_task_get_task_data(task);
  r->operation = REQUEST_DELETE; r->hash = g_strdup(hash); r->auth = g_strdup(auth);
  g_task_run_in_thread(task, worker); g_object_unref(task);
}
gboolean gn_blossom_client_delete_finish(GnBlossomClient *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self) &&
    g_task_get_source_tag(G_TASK(result)) == gn_blossom_client_delete_async, FALSE);
  return g_task_propagate_boolean(G_TASK(result), error);
}
GnBlossomClient *gn_blossom_client_new(const gchar *endpoint)
{
  g_return_val_if_fail(endpoint != NULL, NULL);
  GnBlossomClient *self = g_object_new(GN_TYPE_BLOSSOM_CLIENT, NULL);
  self->endpoint = g_strdup(endpoint);
  gsize n = strlen(self->endpoint);
  while (n && self->endpoint[n - 1] == '/') self->endpoint[--n] = 0;
  return self;
}
void gn_blossom_client_set_network(GnBlossomClient *self, GProxyResolver *resolver, GnBlossomNetworkFlags flags)
{
  g_return_if_fail(GN_IS_BLOSSOM_CLIENT(self));
  g_set_object(&self->resolver, resolver); self->flags = flags;
}
void gn_blossom_client_set_limits(GnBlossomClient *self, gsize limit, guint timeout, const gchar *agent)
{
  g_return_if_fail(GN_IS_BLOSSOM_CLIENT(self));
  self->max_bytes = limit; self->timeout = timeout;
  g_free(self->user_agent); self->user_agent = g_strdup(agent);
}
static void gn_blossom_client_finalize(GObject *object)
{
  GnBlossomClient *self = GN_BLOSSOM_CLIENT(object);
  g_free(self->endpoint); g_free(self->user_agent); g_clear_object(&self->resolver);
  G_OBJECT_CLASS(gn_blossom_client_parent_class)->finalize(object);
}
static void gn_blossom_client_class_init(GnBlossomClientClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = gn_blossom_client_finalize;
}
static void gn_blossom_client_init(GnBlossomClient *self) { (void)self; }
