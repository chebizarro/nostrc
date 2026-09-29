#include "gh-nip11.h"

#include <json-glib/json-glib.h>
#include <string.h>

G_DEFINE_QUARK(gh-nip11-error-quark, gh_nip11_error)

/* GhNetHttp's own rule: plain http only to a loopback address. */
static gboolean
loopback_host(const gchar *host)
{
  g_autoptr(GInetAddress) address = g_inet_address_new_from_string(host);
  return address && g_inet_address_get_is_loopback(address);
}

gchar *
gh_nip11_document_url(const gchar *relay_url, GError **error)
{
  g_autoptr(GUri) uri = relay_url ? g_uri_parse(relay_url, G_URI_FLAGS_ENCODED, NULL) : NULL;
  const gchar *scheme = uri ? g_uri_get_scheme(uri) : NULL;
  const gchar *host = uri ? g_uri_get_host(uri) : NULL;
  gboolean secure = g_strcmp0(scheme, "wss") == 0;
  if (!uri || (!secure && g_strcmp0(scheme, "ws") != 0) || !host || !*host ||
      g_uri_get_userinfo(uri)) {
    g_set_error_literal(error, GH_NIP11_ERROR, GH_NIP11_ERROR_INVALID_URL,
                        "Not a relay address with a host");
    return NULL;
  }
  if (!secure && !loopback_host(host)) {
    g_set_error_literal(error, GH_NIP11_ERROR, GH_NIP11_ERROR_PLAINTEXT,
                        "The relay does not use TLS, so its information is not fetched");
    return NULL;
  }
  const gchar *path = g_uri_get_path(uri);
  return g_uri_join(G_URI_FLAGS_ENCODED, secure ? "https" : "http", NULL, host,
                    g_uri_get_port(uri), path && *path ? path : "/",
                    g_uri_get_query(uri), NULL);
}

static gboolean
hex_key(const gchar *value)
{
  if (!value || strlen(value) != 64)
    return FALSE;
  for (const gchar *p = value; *p; p++)
    if (!g_ascii_isxdigit(*p))
      return FALSE;
  return TRUE;
}

gchar *
gh_nip11_parse_relay_key(const gchar *document, gssize length, GError **error)
{
  g_autoptr(JsonParser) parser = json_parser_new();
  g_autoptr(GError) parse_error = NULL;
  if (!document || !json_parser_load_from_data(parser, document, length, &parse_error) ||
      !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
    g_set_error(error, GH_NIP11_ERROR, GH_NIP11_ERROR_MALFORMED,
                "The relay information document is not a JSON object%s%s",
                parse_error ? ": " : "", parse_error ? parse_error->message : "");
    return NULL;
  }
  JsonObject *object = json_node_get_object(json_parser_get_root(parser));
  /* "self" only: "pubkey" is the administrator's contact key (NIP-11), and
   * pinning it would let that person speak for every group on the relay
   * while the relay's real 39000-39003 were held as foreign (W15 review
   * non-blocking #4). Without "self" the relay key is unavailable. */
  JsonNode *node = json_object_get_member(object, "self");
  const gchar *value = node && JSON_NODE_HOLDS_VALUE(node) &&
                       json_node_get_value_type(node) == G_TYPE_STRING
                         ? json_node_get_string(node) : NULL;
  if (hex_key(value))
    return g_ascii_strdown(value, -1);
  g_set_error_literal(error, GH_NIP11_ERROR, GH_NIP11_ERROR_NO_KEY,
                      "The relay does not publish its key (NIP-11 self)");
  return NULL;
}

/* ---- Fetch ---------------------------------------------------------------------- */

static void
on_document(GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GTask) task = data;
  GError *error = NULL;
  g_autoptr(GBytes) body = gh_net_http_get_finish(GH_NET_HTTP(source), result, &error);
  if (!body) {
    g_task_return_error(task, error);
    return;
  }
  gsize length = 0;
  const gchar *document = g_bytes_get_data(body, &length);
  gchar *key = gh_nip11_parse_relay_key(length ? document : "", (gssize)length, &error);
  if (!key)
    g_task_return_error(task, error);
  else
    g_task_return_pointer(task, key, g_free);
}

void
gh_nip11_fetch_relay_key_async(GhNetHttp *http, const gchar *relay_url,
                               GCancellable *cancellable, GAsyncReadyCallback callback,
                               gpointer user_data)
{
  g_return_if_fail(GH_IS_NET_HTTP(http));
  g_autoptr(GTask) task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_set_source_tag(task, gh_nip11_fetch_relay_key_async);
  GError *error = NULL;
  g_autofree gchar *url = gh_nip11_document_url(relay_url, &error);
  if (!url) {
    g_task_return_error(task, error);
    return;
  }
  gh_net_http_get_accept_async(http, url, "application/nostr+json", GH_NIP11_MAX_RESPONSE,
                               cancellable, on_document, g_steal_pointer(&task));
}

gchar *
gh_nip11_fetch_relay_key_finish(GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}
