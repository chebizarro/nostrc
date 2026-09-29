#include "gh-net-http.h"

#include <libsoup/soup.h>
#include <string.h>

struct _GhNetHttp {
  GObject parent_instance;
  GSettings *settings;
  SoupSession *session;
  gchar *session_mode; /* the network mode session was made for */
};

G_DEFINE_FINAL_TYPE(GhNetHttp, gh_net_http, G_TYPE_OBJECT)

typedef struct {
  SoupMessage *message;
  GInputStream *stream;
  guint8 *buffer;
  gsize max_bytes;
} Request;

static void
request_free(gpointer data)
{
  Request *request = data;
  g_clear_object(&request->message);
  g_clear_object(&request->stream);
  g_free(request->buffer);
  g_free(request);
}

static gchar *
network_mode(GhNetHttp *self)
{
  if (!self->settings)
    return g_strdup("system");
  g_autoptr(GSettingsSchema) schema = NULL;
  g_object_get(self->settings, "settings-schema", &schema, NULL);
  if (!schema || !g_settings_schema_has_key(schema, "network-mode"))
    return g_strdup("system");
  return g_settings_get_string(self->settings, "network-mode");
}

/* The session for mode, made again when the mode changed. NULL for a mode
 * that has no connection path (Tor, until G09). */
static SoupSession *
session_for(GhNetHttp *self, const gchar *mode)
{
  if (self->session && g_strcmp0(self->session_mode, mode) == 0)
    return self->session;
  g_clear_object(&self->session);
  g_clear_pointer(&self->session_mode, g_free);
  if (g_str_equal(mode, "system")) {
    /* NULL proxy-resolver would disable proxies; the default follows the
     * desktop settings. */
    self->session = soup_session_new_with_options(
      "timeout", GH_NET_HTTP_TIMEOUT_S, "idle-timeout", GH_NET_HTTP_TIMEOUT_S,
      "proxy-resolver", g_proxy_resolver_get_default(), "user-agent", NULL,
      "accept-language-auto", FALSE, NULL);
  } else if (g_str_equal(mode, "none")) {
    g_autoptr(GProxyResolver) direct = g_simple_proxy_resolver_new(NULL, NULL);
    self->session = soup_session_new_with_options(
      "timeout", GH_NET_HTTP_TIMEOUT_S, "idle-timeout", GH_NET_HTTP_TIMEOUT_S,
      "proxy-resolver", direct, "user-agent", NULL, "accept-language-auto", FALSE, NULL);
  } else {
    return NULL;
  }
  /* No cookie jar, cache, HSTS or auth store is added; drop any content
   * sniffer a default session may carry (nothing here renders content). */
  soup_session_remove_feature_by_type(self->session, SOUP_TYPE_CONTENT_SNIFFER);
  self->session_mode = g_strdup(mode);
  return self->session;
}

static gboolean
loopback_host(const gchar *host)
{
  if (!host)
    return FALSE;
  g_autoptr(GInetAddress) address = g_inet_address_new_from_string(host);
  return address && g_inet_address_get_is_loopback(address);
}

static void
on_read(GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GTask) task = data;
  Request *request = g_task_get_task_data(task);
  gsize read = 0;
  GError *error = NULL;
  if (!g_input_stream_read_all_finish(G_INPUT_STREAM(source), result, &read, &error)) {
    g_task_return_error(task, error);
    return;
  }
  (void)g_input_stream_close(request->stream, NULL, NULL);
  if (read > request->max_bytes) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE,
                            "The server's answer is larger than %" G_GSIZE_FORMAT " bytes",
                            request->max_bytes);
    return;
  }
  g_task_return_pointer(task, g_bytes_new(request->buffer, read),
                        (GDestroyNotify)g_bytes_unref);
}

static void
on_sent(GObject *source, GAsyncResult *result, gpointer data)
{
  g_autoptr(GTask) task = data;
  Request *request = g_task_get_task_data(task);
  GError *error = NULL;
  request->stream = soup_session_send_finish(SOUP_SESSION(source), result, &error);
  if (!request->stream) {
    g_task_return_error(task, error);
    return;
  }
  guint status = soup_message_get_status(request->message);
  if (SOUP_STATUS_IS_REDIRECTION(status)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "The server redirected the request (HTTP %u), which is not followed",
                            status);
    return;
  }
  if (status != SOUP_STATUS_OK) {
    g_task_return_new_error(task, G_IO_ERROR,
                            status == SOUP_STATUS_NOT_FOUND ? G_IO_ERROR_NOT_FOUND
                                                            : G_IO_ERROR_FAILED,
                            "The server answered HTTP %u", status);
    return;
  }
  goffset length = soup_message_headers_get_content_length(
    soup_message_get_response_headers(request->message));
  if (length > 0 && (guint64)length > request->max_bytes) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE,
                            "The server's answer is larger than %" G_GSIZE_FORMAT " bytes",
                            request->max_bytes);
    return;
  }
  /* One byte more than allowed tells an oversized body apart. */
  request->buffer = g_malloc(request->max_bytes + 1);
  g_input_stream_read_all_async(request->stream, request->buffer, request->max_bytes + 1,
                                G_PRIORITY_DEFAULT, g_task_get_cancellable(task), on_read,
                                g_object_ref(task));
}

void
gh_net_http_get_async(GhNetHttp *self, const gchar *uri, gsize max_bytes,
                      GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GH_IS_NET_HTTP(self));
  g_return_if_fail(uri != NULL && max_bytes > 0 && max_bytes < G_MAXSIZE);
  g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, gh_net_http_get_async);
  g_autoptr(GUri) parsed = g_uri_parse(uri, G_URI_FLAGS_ENCODED, NULL);
  const gchar *scheme = parsed ? g_uri_get_scheme(parsed) : NULL;
  gboolean allowed = scheme && g_uri_get_host(parsed) && !g_uri_get_userinfo(parsed) &&
                     (g_ascii_strcasecmp(scheme, "https") == 0 ||
                      (g_ascii_strcasecmp(scheme, "http") == 0 &&
                       loopback_host(g_uri_get_host(parsed))));
  if (!allowed) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Only https addresses are fetched");
    return;
  }
  g_autofree gchar *mode = network_mode(self);
  SoupSession *session = session_for(self, mode);
  if (!session) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "Groundhog can't connect in the chosen network mode yet");
    return;
  }
  Request *request = g_new0(Request, 1);
  request->max_bytes = max_bytes;
  request->message = soup_message_new_from_uri(SOUP_METHOD_GET, parsed);
  g_task_set_task_data(task, request, request_free);
  soup_message_add_flags(request->message, SOUP_MESSAGE_NO_REDIRECT);
  SoupMessageHeaders *headers = soup_message_get_request_headers(request->message);
  soup_message_headers_replace(headers, "Accept", "application/json");
  soup_session_send_async(session, request->message, G_PRIORITY_DEFAULT, cancellable, on_sent,
                          g_steal_pointer(&task));
}

GBytes *
gh_net_http_get_finish(GhNetHttp *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void
transport_get_async(gpointer data, const gchar *uri, gsize max_bytes, GCancellable *cancellable,
                    GAsyncReadyCallback callback, gpointer user_data)
{
  gh_net_http_get_async(GH_NET_HTTP(data), uri, max_bytes, cancellable, callback, user_data);
}

static GBytes *
transport_get_finish(gpointer data, GAsyncResult *result, GError **error)
{
  return gh_net_http_get_finish(GH_NET_HTTP(data), result, error);
}

const GhHttpTransport *
gh_net_http_transport(void)
{
  static const GhHttpTransport transport = { transport_get_async, transport_get_finish };
  return &transport;
}

GhNetHttp *
gh_net_http_new(GSettings *settings)
{
  g_return_val_if_fail(!settings || G_IS_SETTINGS(settings), NULL);
  GhNetHttp *self = g_object_new(GH_TYPE_NET_HTTP, NULL);
  self->settings = settings ? g_object_ref(settings) : NULL;
  return self;
}

static void
gh_net_http_dispose(GObject *object)
{
  GhNetHttp *self = GH_NET_HTTP(object);
  if (self->session)
    soup_session_abort(self->session);
  g_clear_object(&self->session);
  g_clear_object(&self->settings);
  G_OBJECT_CLASS(gh_net_http_parent_class)->dispose(object);
}

static void
gh_net_http_finalize(GObject *object)
{
  g_free(GH_NET_HTTP(object)->session_mode);
  G_OBJECT_CLASS(gh_net_http_parent_class)->finalize(object);
}

static void
gh_net_http_class_init(GhNetHttpClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = gh_net_http_dispose;
  G_OBJECT_CLASS(klass)->finalize = gh_net_http_finalize;
}

static void
gh_net_http_init(GhNetHttp *self)
{
  (void)self;
}
